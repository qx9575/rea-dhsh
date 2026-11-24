#pragma once

#include "chat/ChatModel.h"
#include "network/GameSession.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dhsh {

// OnlineChatWorker: online-chat stage bridge between the UI thread and one
// GameSession. The worker thread exclusively owns the GameSession (start /
// poll / sendInitSequence / sendChat / teardown); the UI thread never blocks
// on a socket. All cross-thread traffic flows through two bounded queues:
//
//   UI -> worker: sendChat(channel, UTF-8 text), bounded by
//     Options::maxOutgoingMessages. A send that would overflow the queue, or
//     that is requested before the session is established, returns false and
//     queues exactly one sendRejected event carrying the channel and the
//     UTF-8 byte length only -- never the text. Nothing is ever echoed back.
//
//   worker -> UI: Event records (statusChanged / sendRejected / sendFailed /
//     chatMessage / diagnostic), bounded by Options::maxEvents. When full,
//     the oldest event is dropped and droppedEventCount() increments. The
//     worker never touches ChatModel or ScpWindow; the UI thread owns them
//     and applies chatMessage events itself.
//
// Login runs the evidenced flow only: start() -> poll() until the role-stage
// inputs complete (0x2000 role records -> UI selects -> 0x100b -> 0x2005
// challenge -> UI submits the graph key -> 0x1002) -> poll() until the first
// inbound game frame makes the session established, then the documented init
// prefix (sendInitSequence). redirect/gatewayFrame poll results update
// internal status and are never re-published as events, so sensitive gateway
// payloads stay inside the session; the exceptions are the sanitized roleList
// event (accumulated role id/GBK-name pairs, display data only) and the bare
// graphKeyChallenge event (Task 2 attaches the decoded image). gameFrame
// results carrying 0x303f are decrypted by GameSession; the worker strips
// the u16le command from the front (SessionEvent.payload includes it, the
// decoder's DecodedMessage.payload must begin at the DHR1 body) and decodes
// the rest with ChatMessageDecoder, queuing the result as chatMessage
// events; any other game command is dropped after incrementing
// ignoredCommandCount(). A 0x303f that fails decoding produces a diagnostic
// event with the decoded-buffer byte count only -- never payload content.
//
// Role/graph-key commands from the UI (selectRole/submitGraphKey) only enter
// a bounded queue; the worker thread applies them to the session, so the UI
// never touches the socket and no auto-selection or auto-submit shortcut
// exists. A command the session refuses produces one loginRejected event
// with a sanitized message -- never the submitted graph key or role name.
//
// Scope: online chat only. This class does not claim NPC sync, movement, or
// any full online-world behavior. Credentials are accepted at construction,
// live only in worker-side memory (moved into the GameSession, which
// teardown() zeroes), and are never copied into events, diagnostics, or logs.
//
// Lifecycle: start() succeeds exactly once (GameSession cannot restart after
// failure); stop() is idempotent, requests termination, tears the session
// down and joins the thread. The destructor calls stop(), so destruction is
// safe in every state, including never-started and already-stopped. A worker
// exception is caught, reported as a failed statusChanged event, and never
// propagates out of the thread (no terminate).

class OnlineChatWorker {
public:
    struct Options {
        std::chrono::milliseconds connectTimeout = std::chrono::seconds(5);
        std::chrono::milliseconds authDeadline = std::chrono::seconds(30);
        std::chrono::milliseconds sendTimeout = std::chrono::seconds(5);
        // Upper bound on one poll() wait; forwarded to GameSession::poll as
        // the whole-frame read deadline, so it must cover the server's
        // observed ~80-90ms batched response time with headroom (the 20ms
        // default it replaced timed out mid-frame against the real gateway).
        // Bounded but coarse: worst-case stop()/sendChat latency is one full
        // wait, about 1s, and the loop never busy-spins while the UI thread
        // floods the outgoing queue.
        std::chrono::milliseconds pollTimeout = std::chrono::seconds(1);
        std::size_t maxOutgoingMessages = 64;
        std::size_t maxLoginCommands = 16;
        std::size_t maxEvents = 256;
    };

    enum class Status {
        idle,         // constructed, start() not called (or start() refused)
        connecting,   // worker thread running, session not established yet
        established,  // login + documented init prefix completed
        failed,       // terminal; see the failed statusChanged event message
        stopped,      // stop() completed (terminal, not an error)
    };

    struct Event {
        enum class Type {
            statusChanged,  // established | failed (message is diagnostic only)
            sendRejected,   // queue full or not established; length only
            sendFailed,     // wire write failed; length + session diagnostic
            chatMessage,    // decoded 0x303f
            diagnostic,     // e.g. bad 0x303f decode; sanitized, no payload
            roleList,       // accumulated 0x2000 roles: id + GBK name only
            graphKeyChallenge,  // 0x2005 arrived; awaits the user's input
            loginRejected,  // role/graph-key command refused; sanitized only
        };

        Type type = Type::diagnostic;
        Status status = Status::idle;            // statusChanged
        SessionError error = SessionError::none; // statusChanged when failed
        std::string message;                     // sanitized diagnostic only
        std::uint32_t channel = 0;               // sendRejected/sendFailed/chatMessage
        std::size_t textLengthBytes = 0;  // sendRejected/sendFailed: UTF-8 byte
                                          // length of the attempted text only
        ChatMessage chat;                 // chatMessage
        std::vector<RoleRecord> roles;    // roleList: display data only
    };

    // Copies the credentials into worker-side memory; the caller may clear
    // its own copies afterwards. gatewayHost/gatewayPort name the plaintext
    // login gateway (the game endpoint comes from the evidenced 0x2002
    // redirect; nothing here hardcodes accounts, passwords, or endpoints).
    OnlineChatWorker(SessionCredentials credentials, std::string gatewayHost,
                     std::uint16_t gatewayPort, Options options = {});
    ~OnlineChatWorker();

    OnlineChatWorker(const OnlineChatWorker&) = delete;
    OnlineChatWorker& operator=(const OnlineChatWorker&) = delete;

    // Starts the worker thread. Returns false when a start was already
    // requested (including after stop()); the worker cannot be restarted.
    bool start();

    // Idempotent: requests termination, tears the session down, joins the
    // thread. Leaves status() at stopped unless the session already failed.
    void stop();

    // Thread-safe accessors; never touch the session or the UI side.
    Status status() const;
    std::uint64_t ignoredCommandCount() const;
    std::uint64_t droppedEventCount() const;

    // Queues one chat send. Returns false -- and queues exactly one
    // sendRejected event carrying channel and text byte length -- when the
    // outgoing queue is full or the session is not established. A true return
    // only means queued; a later wire failure arrives as sendFailed and, when
    // the session dies, a failed statusChanged.
    bool sendChat(std::uint32_t channel, const std::string& text);

    // Queues the user's role choice / graph-key input for the worker thread
    // to apply to the session. Both return false -- and queue exactly one
    // loginRejected event with a sanitized message -- when the worker is not
    // connecting or the login-command queue is full; the session itself
    // additionally refuses (same event) unknown role ids, duplicate
    // submissions, and a graph-key submit before the 0x2005 challenge. The
    // submitted graph-key text never reaches any event or log.
    bool selectRole(std::uint32_t roleId);
    bool submitGraphKey(const std::string& graphKey);

    // Pops the next worker->UI event. tryPopEvent is non-blocking; waitEvent
    // waits up to `timeout`. Both are UI-thread only by contract.
    bool tryPopEvent(Event* event);
    bool waitEvent(Event* event, std::chrono::milliseconds timeout);

private:
    struct OutgoingMessage {
        std::uint32_t channel = 0;
        std::string text;
    };
    struct LoginCommand {
        enum class Type {
            selectRole,
            submitGraphKey,
        };
        Type type = Type::selectRole;
        std::uint32_t roleId = 0;  // selectRole
        std::string text;          // submitGraphKey (never copied into events)
    };

    void run();  // worker thread body; exclusively owns session_ while alive
    void runLoop();  // session lifecycle; run() always tears down afterwards
    void drainOutgoing();
    void drainLoginCommands();
    void onGameFrame(const SessionEvent& frame);
    void reportFailure(SessionError error, std::string message);

    void pushEventLocked(Event event);  // requires mutex_
    void setStatusLocked(Status status);  // requires mutex_

    // Immutable after construction; credentials_ is moved out by run().
    SessionCredentials credentials_;
    std::string gatewayHost_;
    std::uint16_t gatewayPort_;
    Options options_;

    mutable std::mutex mutex_;
    std::condition_variable eventCv_;
    std::deque<OutgoingMessage> outgoing_;
    std::deque<LoginCommand> loginCommands_;
    std::deque<Event> events_;
    Status status_ = Status::idle;
    bool started_ = false;
    bool stopRequested_ = false;
    bool failed_ = false;
    std::uint64_t ignoredCommands_ = 0;
    std::uint64_t droppedEvents_ = 0;

    std::unique_ptr<GameSession> session_;  // created and used by run() only
    std::thread thread_;
};

} // namespace dhsh
