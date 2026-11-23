#include "network/OnlineChatWorker.h"

#include "network/ChatMessageDecoder.h"
#include "network/DecodedMessage.h"

#include <cstring>
#include <exception>
#include <utility>

namespace dhsh {

OnlineChatWorker::OnlineChatWorker(SessionCredentials credentials,
                                   std::string gatewayHost,
                                   std::uint16_t gatewayPort, Options options)
    : credentials_(std::move(credentials)),
      gatewayHost_(std::move(gatewayHost)),
      gatewayPort_(gatewayPort),
      options_(options) {
}

OnlineChatWorker::~OnlineChatWorker() {
    stop();
}

bool OnlineChatWorker::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return false;  // one shot: GameSession cannot restart after failure
    }
    started_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void OnlineChatWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!failed_) {
        status_ = Status::stopped;
    }
}

OnlineChatWorker::Status OnlineChatWorker::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::uint64_t OnlineChatWorker::ignoredCommandCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ignoredCommands_;
}

std::uint64_t OnlineChatWorker::droppedEventCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return droppedEvents_;
}

bool OnlineChatWorker::sendChat(std::uint32_t channel, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ != Status::established ||
            outgoing_.size() >= options_.maxOutgoingMessages) {
        Event event{};
        event.type = Event::Type::sendRejected;
        event.channel = channel;
        event.textLengthBytes = text.size();
        event.message = status_ != Status::established
            ? "session not established"
            : "outgoing queue full";
        pushEventLocked(std::move(event));
        return false;
    }
    outgoing_.push_back(OutgoingMessage{channel, text});
    return true;
}

bool OnlineChatWorker::tryPopEvent(Event* event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) {
        return false;
    }
    *event = std::move(events_.front());
    events_.pop_front();
    return true;
}

bool OnlineChatWorker::waitEvent(Event* event, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!eventCv_.wait_for(lock, timeout, [this] { return !events_.empty(); })) {
        return false;
    }
    *event = std::move(events_.front());
    events_.pop_front();
    return true;
}

void OnlineChatWorker::pushEventLocked(Event event) {
    if (events_.size() >= options_.maxEvents) {
        events_.pop_front();
        ++droppedEvents_;
    }
    events_.push_back(std::move(event));
    eventCv_.notify_all();
}

void OnlineChatWorker::setStatusLocked(Status status) {
    status_ = status;
}

void OnlineChatWorker::drainOutgoing() {
    while (true) {
        OutgoingMessage message{};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (outgoing_.empty()) {
                return;
            }
            message = std::move(outgoing_.front());
            outgoing_.pop_front();
        }
        if (session_->sendChat(message.channel, message.text,
                               options_.sendTimeout)) {
            continue;
        }
        // The text never reaches any event; only its byte length does.
        Event event{};
        event.type = Event::Type::sendFailed;
        event.channel = message.channel;
        event.textLengthBytes = message.text.size();
        event.message = session_->errorMessage().empty()
            ? "chat send failed"
            : session_->errorMessage();
        const bool sessionDead =
            session_->state() == SessionState::failed;
        const SessionError error = session_->error();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pushEventLocked(std::move(event));
        }
        if (sessionDead) {
            // The failed statusChanged event carries the terminal diagnostic.
            reportFailure(error, session_->errorMessage());
            return;
        }
        // Input-level rejection (invalid UTF-8, embedded NUL, over the byte
        // bound): the session stays established, keep draining the rest.
    }
}

void OnlineChatWorker::onGameFrame(const SessionEvent& frame) {
    if (frame.command != 0x303f) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++ignoredCommands_;
        return;
    }
    // SessionEvent.payload includes the u16le command at [0,2); the
    // ChatMessageDecoder contract (DecodedMessage) expects the DHR1 payload
    // that follows it (channel at DHR1+4, sender at +26, text at +144, i.e.
    // wire +6/+28/+146). Strip exactly those two bytes.
    if (frame.payload.size() < 2) {
        // GameSession already rejects sub-2-byte frames (protocol_error);
        // this guard only keeps the decoder contract defensive.
        Event event{};
        event.type = Event::Type::diagnostic;
        event.message = "0x303f frame shorter than the command (" +
            std::to_string(frame.payload.size()) + " byte payload)";
        std::lock_guard<std::mutex> lock(mutex_);
        pushEventLocked(std::move(event));
        return;
    }
    DecodedMessage message{};
    message.command = frame.command;
    message.direction = MessageDirection::server_to_client;
    const std::size_t bodySize = frame.payload.size() - 2;
    message.payload.resize(bodySize);
    std::memcpy(message.payload.data(), frame.payload.data() + 2, bodySize);
    ChatMessageDecoder decoder;
    if (auto chat = decoder.decode(message)) {
        Event event{};
        event.type = Event::Type::chatMessage;
        event.channel = chat->channel;
        event.chat = std::move(*chat);
        std::lock_guard<std::mutex> lock(mutex_);
        pushEventLocked(std::move(event));
        return;
    }
    // Diagnostic carries the decoded-buffer byte count only, never payload
    // content.
    Event event{};
    event.type = Event::Type::diagnostic;
    event.message = "0x303f decode failed (" + std::to_string(bodySize) +
        " byte payload)";
    std::lock_guard<std::mutex> lock(mutex_);
    pushEventLocked(std::move(event));
}

void OnlineChatWorker::reportFailure(SessionError error, std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failed_) {
        return;  // first failure wins; later duplicates are suppressed
    }
    failed_ = true;
    setStatusLocked(Status::failed);
    Event event{};
    event.type = Event::Type::statusChanged;
    event.status = Status::failed;
    event.error = error;
    event.message = std::move(message);
    pushEventLocked(std::move(event));
}

void OnlineChatWorker::run() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        setStatusLocked(Status::connecting);
    }
    try {
        runLoop();
    } catch (const std::exception& error) {
        // A worker bug or transport surprise must surface as failed, never as
        // std::terminate.
        reportFailure(SessionError::none,
                      std::string("worker exception: ") + error.what());
    } catch (...) {
        reportFailure(SessionError::none, "worker exception: unknown");
    }
    if (session_) {
        session_->teardown();
    }
}

void OnlineChatWorker::runLoop() {
    // The session (and with it the credential copies) lives only on this
    // thread until teardown(); teardown() overwrites the credential bytes.
    session_.reset(new GameSession(std::move(credentials_), {}, 1,
                                   options_.authDeadline));
    if (!session_->start(gatewayHost_, gatewayPort_,
                         options_.connectTimeout)) {
        reportFailure(session_->error(), session_->errorMessage());
        return;
    }

    bool initSent = false;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopRequested_) {
                break;
            }
        }
        drainOutgoing();
        if (session_->state() == SessionState::failed) {
            return;  // drainOutgoing already reported the failure
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopRequested_) {
                break;
            }
        }

        SessionEvent event{};
        if (!session_->poll(options_.pollTimeout, &event)) {
            continue;  // none event: poll again, no busy spin
        }
        switch (event.type) {
        case SessionEvent::Type::none:
            break;
        case SessionEvent::Type::gatewayFrame:
        case SessionEvent::Type::redirect:
            // Internal state only. Gateway payloads (and the redirect
            // target) never reach UI events, so sensitive gateway frames
            // stay inside the session.
            break;
        case SessionEvent::Type::gameFrame:
            onGameFrame(event);
            break;
        case SessionEvent::Type::error:
            reportFailure(event.error, event.message);
            return;
        }

        if (!initSent &&
                session_->state() == SessionState::established) {
            if (!session_->sendInitSequence(options_.sendTimeout)) {
                reportFailure(session_->error(),
                    session_->errorMessage().empty()
                        ? "sendInitSequence failed"
                        : session_->errorMessage());
                return;
            }
            initSent = true;
            std::lock_guard<std::mutex> lock(mutex_);
            setStatusLocked(Status::established);
            Event established{};
            established.type = Event::Type::statusChanged;
            established.status = Status::established;
            pushEventLocked(std::move(established));
        }
    }
}

} // namespace dhsh
