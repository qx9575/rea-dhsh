#include "network/GameSession.h"

#include <cstring>
#include <utility>
#include <vector>

namespace dhsh {

namespace {

// Observed gateway s2c commands during the plaintext handshake (both
// 2026-11-23 connections of .rea/login_full_retry_20261123.pcapng). Anything
// else during the handshake is a protocol surprise and fails closed.
bool isObservedGatewayCommand(std::uint16_t command) {
    switch (command) {
    case 0x2000:  // auth response (result semantics not closed; passed through)
    case 0x2001:  // command ack echoing the c2s cmd
    case 0x2002:  // redirect to the game server
    case 0x2003:  // server list block
    case 0x2005:  // config/list blob
    case 0x2009:  // zone info
    case 0x200a:  // token
    case 0x200b:  // N/Y feature flags
    case 0x200c:  // empty ack
    case 0x2500:  // ack after the environment report
        return true;
    default:
        return false;
    }
}

void secureZero(void* data, std::size_t length) {
    volatile std::uint8_t* p = static_cast<volatile std::uint8_t*>(data);
    while (length-- > 0) {
        *p++ = 0;
    }
}

void writeLe16(std::uint8_t* p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value & 0xffU);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
}

void writeLe32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value & 0xffU);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    p[2] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    p[3] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
}

std::uint16_t readLe16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
}

} // namespace

GameSession::GameSession(
    SessionCredentials credentials,
    std::function<GatewayAuthMaterial(const SessionCredentials&)> authMaterial,
    std::uint32_t sequenceSeed,
    std::chrono::milliseconds authDeadline)
    : credentials_(std::move(credentials)),
      authMaterial_(std::move(authMaterial)),
      sequenceField_(sequenceSeed),
      authDeadline_(authDeadline) {
}

GameSession::~GameSession() {
    teardown();
}

SessionState GameSession::fail(SessionError error, std::string message) {
    state_ = SessionState::failed;
    error_ = error;
    errorMessage_ = std::move(message);
    gateway_.close();
    game_.close();
    return state_;
}

void GameSession::fillNoneEvent(SessionEvent* event) const {
    if (event == nullptr) {
        return;
    }
    *event = SessionEvent{};
    event->type = SessionEvent::Type::none;
    event->state = state_;
}

void GameSession::fillErrorEvent(SessionEvent* event) const {
    if (event == nullptr) {
        return;
    }
    *event = SessionEvent{};
    event->type = SessionEvent::Type::error;
    event->state = state_;
    event->error = error_;
    event->message = errorMessage_;
}

bool GameSession::deadlineExpired() const {
    return authClockRunning_ &&
        std::chrono::steady_clock::now() - authStart_ > authDeadline_;
}

bool GameSession::start(const std::string& gatewayHost, std::uint16_t gatewayPort,
                        std::chrono::milliseconds connectTimeout) {
    if (state_ != SessionState::idle) {
        fail(SessionError::bad_usage, "start() requires the idle state");
        return false;
    }
    if (authMaterial_) {
        material_ = authMaterial_(credentials_);
    }

    // 0x1005 hello: cmd + 0x0a constant at +2, then zeros (14B payload).
    std::vector<std::uint8_t> hello(14, 0);
    writeLe16(hello.data(), 0x1005);
    hello[2] = 0x0a;

    // 0x1000 auth request (224B payload), field offsets from the capture.
    constexpr std::size_t kAccountField = 0x40 - 0x0e;
    constexpr std::size_t kZoneField = 0x7b - 0x62;
    constexpr std::size_t kVersionField = 0xd6 - 0xaf;
    if (credentials_.account.empty() ||
            credentials_.account.size() > kAccountField ||
            credentials_.zoneNameGbk.size() > kZoneField ||
            credentials_.versionToken.size() > kVersionField) {
        fail(SessionError::bad_usage, "credential field exceeds its wire slot");
        return false;
    }
    std::vector<std::uint8_t> auth(224, 0);
    writeLe16(auth.data(), 0x1000);
    std::memcpy(auth.data() + 0x0e, credentials_.account.data(),
                credentials_.account.size());
    std::memcpy(auth.data() + 0x40, material_.passwordDigestHex.data(), 32);
    writeLe16(auth.data() + 0x60, material_.authParam);
    std::memcpy(auth.data() + 0x62, credentials_.zoneNameGbk.data(),
                credentials_.zoneNameGbk.size());
    std::memcpy(auth.data() + 0x7b, "000000000000", 12);
    std::memcpy(auth.data() + 0xaf, credentials_.versionToken.data(),
                credentials_.versionToken.size());
    std::memcpy(auth.data() + 0xd6, material_.slot2.data(), 2);
    writeLe32(auth.data() + 0xdb, 1);

    // 0x1500 environment report: 12B header, GBK CSV text at +0x0c.
    std::vector<std::uint8_t> env(0x0c + credentials_.environmentReportGbk.size(), 0);
    writeLe16(env.data(), 0x1500);
    std::memcpy(env.data() + 0x0c, credentials_.environmentReportGbk.data(),
                credentials_.environmentReportGbk.size());

    // 0x100b empty follow-up (14B payload).
    std::vector<std::uint8_t> empty(14, 0);
    writeLe16(empty.data(), 0x100b);

    if (gateway_.connect(gatewayHost, gatewayPort, connectTimeout) != StreamStatus::ok) {
        fail(SessionError::connect_failed,
            "gateway connect failed: " + gateway_.lastError());
        return false;
    }
    authStart_ = std::chrono::steady_clock::now();
    authClockRunning_ = true;
    state_ = SessionState::gatewayAuthenticating;

    const struct {
        const std::uint8_t* bytes;
        std::size_t length;
        const char* name;
    } batch[] = {
        {hello.data(), hello.size(), "0x1005 hello"},
        {auth.data(), auth.size(), "0x1000 auth request"},
        {env.data(), env.size(), "0x1500 environment report"},
        {empty.data(), empty.size(), "0x100b"},
    };
    for (const auto& frame : batch) {
        if (gateway_.writeFrame(frame.bytes, frame.length, connectTimeout) !=
                StreamStatus::ok) {
            fail(SessionError::disconnected,
                std::string("gateway rejected ") + frame.name + ": " +
                    gateway_.lastError());
            return false;
        }
    }
    return true;
}

bool GameSession::submitCaptcha(const std::string& captchaText) {
    if (state_ != SessionState::gatewayAuthenticating) {
        errorMessage_ = "submitCaptcha() requires the gatewayAuthenticating state";
        return false;
    }
    if (captchaText.empty() || captchaText.size() > 15) {
        errorMessage_ = "captcha text must be 1..15 bytes";
        return false;
    }
    // 0x1002 (41B payload): stored-password field at +0x0e, captcha at +0x22.
    std::vector<std::uint8_t> submit(41, 0);
    writeLe16(submit.data(), 0x1002);
    std::memcpy(submit.data() + 0x0e, material_.storedPassword.data(), 8);
    std::memcpy(submit.data() + 0x22, captchaText.data(), captchaText.size());
    if (gateway_.writeFrame(submit.data(), submit.size(), std::chrono::seconds(5)) !=
            StreamStatus::ok) {
        fail(SessionError::disconnected,
            "gateway rejected 0x1002 captcha submit: " + gateway_.lastError());
        return false;
    }
    return true;
}

bool GameSession::beginGameLogin(std::chrono::milliseconds timeout,
                                 SessionEvent* event) {
    if (game_.connect(redirect_.host, redirect_.port, timeout) != StreamStatus::ok) {
        fail(SessionError::connect_failed,
            "game connect failed: " + game_.lastError());
        fillErrorEvent(event);
        return false;
    }

    // 0x4004 login frame: 16B header + 93B credential blob (111B plaintext),
    // Blowfish with the static master key, no MD5 trailer on the login frame.
    constexpr std::size_t kAccountSlot = 51;
    constexpr std::size_t kPasswordSlot = 20;
    constexpr std::size_t kMacSlot = kCredentialBlobSize - 0x4b;
    if (credentials_.account.size() > kAccountSlot ||
            credentials_.password.size() > kPasswordSlot ||
            credentials_.macAddress.size() > kMacSlot) {
        fail(SessionError::bad_usage, "credential field exceeds the login blob slot");
        fillErrorEvent(event);
        return false;
    }
    credentialBlob_.assign(kCredentialBlobSize, 0);
    std::memcpy(credentialBlob_.data(), credentials_.account.data(),
                credentials_.account.size());
    std::memcpy(credentialBlob_.data() + 0x33, credentials_.password.data(),
                credentials_.password.size());
    writeLe32(credentialBlob_.data() + 0x47, redirect_.session);
    std::memcpy(credentialBlob_.data() + 0x4b, credentials_.macAddress.data(),
                credentials_.macAddress.size());

    std::vector<std::uint8_t> plain(18 + kCredentialBlobSize, 0);
    writeLe16(plain.data(), 0x4004);
    writeLe32(plain.data() + 0x0e, sequenceField_);
    sequenceField_ = nextSequence(sequenceField_);
    std::memcpy(plain.data() + 18, credentialBlob_.data(), kCredentialBlobSize);

    const auto& master = loginMasterKey();
    BlowfishCipher blowfish(master.data(), master.size());
    blowfish.encryptInPlace(plain.data(), plain.size());
    if (game_.writeFrame(plain.data(), plain.size(), timeout) != StreamStatus::ok) {
        fail(SessionError::disconnected,
            "game rejected 0x4004 login frame: " + game_.lastError());
        fillErrorEvent(event);
        return false;
    }

    std::array<std::uint8_t, kCredentialBlobSize> blobArray{};
    std::memcpy(blobArray.data(), credentialBlob_.data(), kCredentialBlobSize);
    const std::array<std::uint8_t, kRc4SessionKeySize> key = deriveRc4Key(blobArray);
    rc4_.reset(new Rc4Session(key.data(), key.size()));

    state_ = SessionState::gameAuthenticating;
    return true;
}

bool GameSession::pumpGateway(std::chrono::milliseconds timeout,
                              SessionEvent* event) {
    if (deadlineExpired()) {
        fail(SessionError::timeout,
            "authentication deadline expired while waiting for the gateway");
        fillErrorEvent(event);
        return true;
    }
    std::vector<std::uint8_t> payload;
    const StreamStatus status = gateway_.readFrame(payload, timeout);
    if (status == StreamStatus::timeout) {
        if (deadlineExpired()) {
            fail(SessionError::timeout,
                "authentication deadline expired while waiting for the gateway");
            fillErrorEvent(event);
            return true;
        }
        fillNoneEvent(event);
        return false;
    }
    if (status != StreamStatus::ok) {
        const SessionError mapped = status == StreamStatus::protocol_error
            ? SessionError::protocol_error : SessionError::disconnected;
        fail(mapped, "gateway transport error: " + gateway_.lastError());
        fillErrorEvent(event);
        return true;
    }
    if (payload.size() < 2) {
        fail(SessionError::protocol_error, "gateway frame shorter than the command");
        fillErrorEvent(event);
        return true;
    }
    const std::uint16_t command = readLe16(payload.data());
    if (command == 0x2002) {
        // Redirect validation (net-protocol.md §5.1). Wire-frame offsets are
        // +0x10 session / +0x14 ip / +0x18 port; payload here starts at the
        // cmd (2 bytes into the wire frame), so the code reads them at
        // payload-relative 0x0e / 0x12 / 0x16.
        if (payload.size() != 24) {
            fail(SessionError::illegal_redirect,
                "0x2002 redirect payload length " + std::to_string(payload.size()) +
                    " != 24");
            fillErrorEvent(event);
            return true;
        }
        if (payload[0x12] == 0 && payload[0x13] == 0 && payload[0x14] == 0 &&
                payload[0x15] == 0) {
            fail(SessionError::illegal_redirect, "0x2002 redirect carries 0.0.0.0");
            fillErrorEvent(event);
            return true;
        }
        const std::uint16_t port = readLe16(payload.data() + 0x16);
        if (port == 0) {
            fail(SessionError::illegal_redirect, "0x2002 redirect carries port 0");
            fillErrorEvent(event);
            return true;
        }
        redirect_.session = (static_cast<std::uint32_t>(payload[0x0e])) |
            (static_cast<std::uint32_t>(payload[0x0f]) << 8) |
            (static_cast<std::uint32_t>(payload[0x10]) << 16) |
            (static_cast<std::uint32_t>(payload[0x11]) << 24);
        redirect_.host = std::to_string(payload[0x12]) + "." +
            std::to_string(payload[0x13]) + "." + std::to_string(payload[0x14]) +
            "." + std::to_string(payload[0x15]);
        redirect_.port = port;
        gateway_.close();
        if (!beginGameLogin(timeout, event)) {
            return true;  // beginGameLogin already filled the error event
        }
        event->type = SessionEvent::Type::redirect;
        event->state = state_;
        event->redirect = redirect_;
        return true;
    }
    if (!isObservedGatewayCommand(command)) {
        fail(SessionError::unknown_command,
            "gateway sent unlisted command 0x" +
                std::to_string(static_cast<unsigned>(command)));
        fillErrorEvent(event);
        return true;
    }
    event->type = SessionEvent::Type::gatewayFrame;
    event->state = state_;
    event->command = command;
    event->payload = std::move(payload);
    return true;
}

bool GameSession::pumpGame(std::chrono::milliseconds timeout, SessionEvent* event) {
    if (authClockRunning_ && deadlineExpired()) {
        fail(SessionError::timeout,
            "authentication deadline expired while waiting for the game server");
        fillErrorEvent(event);
        return true;
    }
    std::vector<std::uint8_t> payload;
    const StreamStatus status = game_.readFrame(payload, timeout);
    if (status == StreamStatus::timeout) {
        if (authClockRunning_ && deadlineExpired()) {
            fail(SessionError::timeout,
                "authentication deadline expired while waiting for the game server");
            fillErrorEvent(event);
            return true;
        }
        fillNoneEvent(event);
        return false;
    }
    if (status != StreamStatus::ok) {
        const SessionError mapped = status == StreamStatus::protocol_error
            ? SessionError::protocol_error : SessionError::disconnected;
        fail(mapped, "game transport error: " + game_.lastError());
        fillErrorEvent(event);
        return true;
    }
    if (payload.size() < 2) {
        fail(SessionError::protocol_error, "game frame shorter than the command");
        fillErrorEvent(event);
        return true;
    }
    if (payload.size() < 0x12) {
        // The table cipher cannot authenticate frames below the header+seed
        // minimum; anything this short on the game connection is garbage.
        fail(SessionError::protocol_error, "game frame below the 0x12 minimum");
        fillErrorEvent(event);
        return true;
    }
    const S2cTableCipher cipher(s2cKeyTable());
    cipher.decryptInPlace(payload.data(), payload.size());
    if (state_ == SessionState::gameAuthenticating) {
        state_ = SessionState::established;
        authClockRunning_ = false;
    }
    event->type = SessionEvent::Type::gameFrame;
    event->state = state_;
    event->command = readLe16(payload.data());
    event->payload = std::move(payload);
    return true;
}

bool GameSession::poll(std::chrono::milliseconds timeout, SessionEvent* event) {
    if (state_ == SessionState::failed) {
        fillErrorEvent(event);
        return true;
    }
    if (state_ == SessionState::idle) {
        fillNoneEvent(event);
        return false;
    }
    if (state_ == SessionState::gatewayAuthenticating) {
        return pumpGateway(timeout, event);
    }
    return pumpGame(timeout, event);
}

bool GameSession::sendInitSequence(std::chrono::milliseconds timeout) {
    if (state_ != SessionState::established) {
        errorMessage_ = "sendInitSequence() requires the established state";
        return false;
    }
    // Static payloads from the 2026-11-23 double-account capture; only the
    // sequence field at +0x0e differs between accounts.
    const struct {
        std::uint16_t cmd;
        std::uint8_t body[12];
    } sequence[] = {
        {0x4022, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {0x4022, {0, 0, 4, 0, 0, 0, 0x20, 0x03, 0, 0, 0, 0}},
        {0x40d0, {0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {0x40d0, {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {0x40d0, {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {0x40d0, {0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {0x4665, {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    };
    for (const auto& frame : sequence) {
        std::uint8_t plain[0x12] = {};
        writeLe16(plain, frame.cmd);
        std::memcpy(plain + 2, frame.body, sizeof(frame.body));
        if (!sendGameFrame(plain, sizeof(plain), timeout)) {
            return false;
        }
    }
    return true;
}

bool GameSession::sendGameFrame(const std::uint8_t* payload, std::size_t length,
                                std::chrono::milliseconds timeout) {
    if (state_ != SessionState::established || rc4_ == nullptr) {
        errorMessage_ = "sendGameFrame() requires the established state";
        return false;
    }
    if (payload == nullptr || length < 0x12 || length + 4 > game_.maxFramePayload()) {
        errorMessage_ = "sendGameFrame() payload outside the wire bounds";
        return false;
    }
    std::vector<std::uint8_t> body(payload, payload + length);
    writeLe32(body.data() + 0x0e, sequenceField_);
    sequenceField_ = nextSequence(sequenceField_);
    rc4_->cryptInPlace(body.data(), body.size());
    const auto trailer = c2sTrailer(body.data(), body.size());
    std::vector<std::uint8_t> wire(body.begin(), body.end());
    wire.insert(wire.end(), trailer.begin(), trailer.end());
    secureZero(body.data(), body.size());
    if (game_.writeFrame(wire.data(), wire.size(), timeout) != StreamStatus::ok) {
        fail(SessionError::disconnected,
            "game rejected c2s frame: " + game_.lastError());
        return false;
    }
    return true;
}

void GameSession::teardown() {
    gateway_.close();
    game_.close();
    secureZero(credentialBlob_.data(), credentialBlob_.size());
    credentialBlob_.clear();
    for (char& c : material_.passwordDigestHex) {
        c = 0;
    }
    for (char& c : material_.storedPassword) {
        c = 0;
    }
    for (char& c : material_.slot2) {
        c = 0;
    }
    secureZero(&credentials_.account[0], credentials_.account.size());
    secureZero(&credentials_.password[0], credentials_.password.size());
    credentials_ = SessionCredentials{};
}

} // namespace dhsh
