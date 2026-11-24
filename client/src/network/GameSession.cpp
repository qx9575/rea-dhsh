#include "network/GameSession.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace dhsh {

namespace {

// Observed gateway s2c commands during the plaintext handshake (both
// 2026-11-23 connections of .rea/login_full_retry_20261123.pcapng). Anything
// else during the handshake is a protocol surprise and fails closed. 0x2000
// (role records) is handled explicitly before this list is consulted.
bool isObservedGatewayCommand(std::uint16_t command) {
    switch (command) {
    case 0x2001:  // command ack echoing the c2s cmd
    case 0x2002:  // redirect to the game server
    case 0x2003:  // server list block
    case 0x2005:  // graph-key challenge; decoded on the post-selection path
                  // above, inert gateway frame before/after that
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

std::uint32_t readLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
        (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) |
        (static_cast<std::uint32_t>(p[3]) << 24);
}

// Formats a command id for diagnostics as 0x + 4 lowercase hex digits; the
// id alone carries no payload content.
std::string commandIdForDiagnostic(std::uint16_t command) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "0x%04x",
        static_cast<unsigned>(command));
    return buffer;
}

// Defensive byte bound for one chat message, enforced on the converted GBK
// bytes. The shipped client caps the input length in its RichEdit UI (200
// characters); this layer only enforces protocol/defense bounds for the
// replication core and does not claim any evidenced original maximum.
constexpr std::size_t kMaxChatGbkBytes = 4000;

// Input-side overflow guard only. A GBK-representable character costs 1..3
// UTF-8 bytes and produces 1..2 GBK bytes, so the converted size is always
// at least 2/3 of the UTF-8 size; any message that can still fit the GBK
// bound above is therefore at most kMaxChatUtf8InputBytes long. The guard
// keeps the intermediate wide/GBK buffers bounded for hostile sizes, while
// the exact GBK-side check after the conversion stays authoritative (e.g.
// 2000 CJK characters = 6000 UTF-8 bytes = exactly 4000 GBK bytes passes).
constexpr std::size_t kMaxChatUtf8InputBytes = kMaxChatGbkBytes / 2 * 3;

// Strict UTF-8 -> GBK (Win32 CP936). Rejects empty input, input over the
// overflow guard, invalid UTF-8 (strict validation, no best-fit guessing),
// embedded NUL bytes, characters without a CP936 representation (the
// conversion reports the default-character fallback instead of silently
// degrading text), and results over kMaxChatGbkBytes.
std::optional<std::string> utf8ToGbk(const std::string& text) {
    if (text.empty() || text.size() > kMaxChatUtf8InputBytes ||
            text.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    const int utf8Size = static_cast<int>(text.size());
    const int wideSize = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), utf8Size, nullptr, 0);
    if (wideSize <= 0) {
        return std::nullopt;
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(wideSize));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), utf8Size, wide.data(), wideSize) <= 0) {
        return std::nullopt;
    }
    const char defaultChar = '?';
    BOOL usedDefaultChar = FALSE;
    const int gbkSize = WideCharToMultiByte(936, 0, wide.data(), wideSize,
        nullptr, 0, &defaultChar, &usedDefaultChar);
    if (gbkSize <= 0 || usedDefaultChar) {
        return std::nullopt;
    }
    std::string gbk(static_cast<std::size_t>(gbkSize), '\0');
    usedDefaultChar = FALSE;
    if (WideCharToMultiByte(936, 0, wide.data(), wideSize,
            gbk.data(), gbkSize, &defaultChar, &usedDefaultChar) <= 0 ||
            usedDefaultChar) {
        return std::nullopt;
    }
    if (gbk.size() > kMaxChatGbkBytes) {
        return std::nullopt;
    }
    return gbk;
}

// Local random source for the two 0x4050 rand fields, matching the evidenced
// _rand() semantics (15-bit non-negative, FUN_007a3400 stores each draw in a
// u32). One std::mt19937 per session seeded from std::random_device and the
// steady clock: no process-global C rand state is touched, so no srand side
// effects, and tests never see fixed constants.
std::function<std::uint32_t()> makeChatRandSource() {
    std::random_device device;
    const auto now = static_cast<unsigned long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::seed_seq seed{device(), device(), device(), device(),
        static_cast<unsigned>(now), static_cast<unsigned>(now >> 32)};
    auto engine = std::make_shared<std::mt19937>(seed);
    return [engine]() -> std::uint32_t {
        return std::uniform_int_distribution<std::uint32_t>(0, 0x7fff)(*engine);
    };
}

} // namespace

GameSession::GameSession(
    SessionCredentials credentials,
    std::uint32_t sequenceSeed,
    std::chrono::milliseconds authDeadline)
    : credentials_(std::move(credentials)),
      sequenceField_(sequenceSeed),
      authDeadline_(authDeadline),
      randSource_(makeChatRandSource()) {
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

    // 0x1005 hello: cmd + 0x0a constant at +2, then zeros (14B payload).
    std::vector<std::uint8_t> hello(14, 0);
    writeLe16(hello.data(), 0x1005);
    hello[2] = 0x0a;

    // 0x1000 auth request, 224B payload. Offsets are relative to the cmd
    // (the receive buffer starts there). Layout closed by the 2026-11-23
    // static chain and the login_full_retry capture: FUN_007a2a00 copies a
    // 16-byte header plus 210 bytes from accountStruct for cmd < 0x1501, so
    // accountStruct+N lands at payload +0x0e+N. FUN_007a3720 uppercases the
    // raw password at accountStruct+0x33 into MD5 hex before sending, which
    // is exactly md5UpperHex(password) at +0x41..+0x60 with the NUL at +0x61.
    constexpr std::size_t kAccountField = 0x41 - 0x0e;   // accountStruct+0x00
    constexpr std::size_t kZoneField = 0x7b - 0x62;      // zone slot at +0x62
    constexpr std::size_t kVersionField = 0xd2 - 0xaf;   // version slot at +0xaf
    const bool securityCodeValid = credentials_.securityCode.size() == 6 &&
        std::all_of(credentials_.securityCode.begin(), credentials_.securityCode.end(),
            [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z');
            });
    if (credentials_.account.empty() ||
            credentials_.account.size() > kAccountField ||
            credentials_.zoneNameGbk.size() > kZoneField ||
            credentials_.versionToken.size() > kVersionField ||
            !securityCodeValid) {
        fail(SessionError::bad_usage, "credential field exceeds its wire slot");
        return false;
    }
    auto passwordDigest = md5UpperHex(
        reinterpret_cast<const std::uint8_t*>(credentials_.password.data()),
        credentials_.password.size());
    std::vector<std::uint8_t> auth(224, 0);
    writeLe16(auth.data(), 0x1000);
    writeLe32(auth.data() + 2, 1);  // header counter, FUN_007a3720 local_14
    std::memcpy(auth.data() + 0x0e, credentials_.account.data(),
                credentials_.account.size());
    std::memcpy(auth.data() + 0x41, passwordDigest.data(), 32);
    secureZero(passwordDigest.data(), passwordDigest.size());
    std::memcpy(auth.data() + 0x62, credentials_.zoneNameGbk.data(),
                credentials_.zoneNameGbk.size());
    std::memcpy(auth.data() + 0x7b, "000000000000", 12);
    std::memcpy(auth.data() + 0xaf, credentials_.versionToken.data(),
                credentials_.versionToken.size());
    std::memcpy(auth.data() + 0xd2, credentials_.securityCode.data(), 6);
    writeLe32(auth.data() + 0xdc, 1);

    // 0x1500 environment report: 14B header, GBK CSV text at +0x0e
    // (2026-11-23 capture; the earlier +0x0c reading was wrong). The text
    // stays runtime-provided via credentials_.environmentReportGbk; this
    // core never invents a fixed environment.
    std::vector<std::uint8_t> env(0x0e + credentials_.environmentReportGbk.size(), 0);
    writeLe16(env.data(), 0x1500);
    std::memcpy(env.data() + 0x0e, credentials_.environmentReportGbk.data(),
                credentials_.environmentReportGbk.size());

    // Nothing else goes out in start(): the 0x100b follow-up and the 0x1002
    // role/graph frame are explicit role-stage API sends, gated on the 0x2000
    // role records and the 0x2005 challenge (2026-11-24 design closure). The
    // legacy role fields in the credentials are not read.

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
        if (!gateway_.connected()) {
            // The transport consumed part of the frame before the deadline
            // and closed the stream; a late payload would desync the next
            // readFrame. Surface the terminal error now, keeping the
            // original "operation timed out" diagnostic, instead of
            // reporting none and dying with "readFrame on a closed stream"
            // on the next poll.
            fail(SessionError::disconnected,
                "gateway transport error: " + gateway_.lastError());
            fillErrorEvent(event);
            return true;
        }
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
        std::string message = "gateway transport error: " + gateway_.lastError();
        if (status == StreamStatus::protocol_error) {
            // Numeric desync metadata only (see the header note): how many
            // frames and wire bytes were accepted before the stream desynced
            // and the last accepted command id ("none" when nothing arrived
            // yet). Never payload bytes or anything derived from them.
            message += " accepted_frames=" + std::to_string(gatewayAcceptedFrames_) +
                " accepted_wire_bytes=" + std::to_string(gatewayAcceptedWireBytes_) +
                " last_command=" +
                (gatewayLastCommandSeen_
                    ? commandIdForDiagnostic(gatewayLastCommand_)
                    : std::string("none"));
        }
        fail(mapped, std::move(message));
        fillErrorEvent(event);
        return true;
    }
    if (payload.size() < 2) {
        fail(SessionError::protocol_error, "gateway frame shorter than the command");
        fillErrorEvent(event);
        return true;
    }
    const std::uint16_t command = readLe16(payload.data());
    ++gatewayAcceptedFrames_;
    gatewayAcceptedWireBytes_ += payload.size() + 2;  // payload + u16 header
    gatewayLastCommand_ = command;
    gatewayLastCommandSeen_ = true;
    if (command == 0x2000) {
        // Role records: parse and accumulate, then surface the structured
        // roleList event (ids + GBK names, display data only). The raw
        // payload never leaves the session.
        if (!parseRoleRecords(payload)) {
            fillErrorEvent(event);
            return true;
        }
        event->type = SessionEvent::Type::roleList;
        event->state = state_;
        event->roles = roles_;
        return true;
    }
    if (command == 0x2005 && roleSelected_ && !graphKeySeen_) {
        // The challenge that follows the user's role choice: decode the image
        // with GraphKeyDecoder before anything else. A payload that fails the
        // decode is a protocol surprise, not a challenge: fail closed at
        // protocol_error with the decoder's fixed, sanitized diagnostic
        // (never payload bytes), leaving graphKeySeen_ unlatched so the
        // submit stays locked. Only a successful decode latches the gate and
        // surfaces the event, carrying the decoded image only -- the raw
        // payload never leaves this frame handler.
        std::vector<std::byte> encoded(payload.size());
        std::transform(payload.begin(), payload.end(), encoded.begin(),
            [](std::uint8_t byte) { return std::byte{byte}; });
        GraphKeyDecodeResult decoded = GraphKeyDecoder().decode(encoded);
        if (!decoded.ok()) {
            fail(SessionError::protocol_error,
                std::string("0x2005 graph-key challenge rejected: ") +
                    graphKeyDecodeMessage(decoded.error));
            fillErrorEvent(event);
            return true;
        }
        graphKeySeen_ = true;
        if (event != nullptr) {
            *event = SessionEvent{};  // no raw payload (or stale fields) in
                                      // the challenge event
            event->type = SessionEvent::Type::graphKeyChallenge;
            event->state = state_;
            event->graphKeyImage = std::move(decoded.image);
        }
        return true;
    }
    if (command == 0x2002) {
        // Redirect validation (net-protocol.md section 5.1). Wire-frame offsets are
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

bool GameSession::parseRoleRecords(const std::vector<std::uint8_t>& payload) {
    // 0x2000 layout (2026-11-24 design closure; the 2026-11-23 sample is one
    // 147-byte frame: 14-byte header + one 0x85-byte record). Record fields
    // sit payload-relative: id u32le at record+0, GBK name in the 20-byte
    // slot at record+4. Failure strings carry structure facts only -- never
    // record content.
    constexpr std::size_t kHeaderSize = 0x0e;
    constexpr std::size_t kRecordSize = 0x85;
    constexpr std::size_t kNameSlot = 20;  // 19 bytes max + NUL
    if (payload.size() < kHeaderSize) {
        fail(SessionError::protocol_error,
            "0x2000 role list shorter than the 14-byte header");
        return false;
    }
    const std::size_t recordBytes = payload.size() - kHeaderSize;
    if (recordBytes == 0 || recordBytes % kRecordSize != 0) {
        fail(SessionError::protocol_error,
            "0x2000 carries no whole 0x85-byte role records");
        return false;
    }
    std::vector<RoleRecord> parsed;
    parsed.reserve(recordBytes / kRecordSize);
    for (std::size_t offset = kHeaderSize; offset < payload.size();
         offset += kRecordSize) {
        RoleRecord role;
        role.id = readLe32(payload.data() + offset);
        const std::uint8_t* nameSlot = payload.data() + offset + 4;
        std::size_t nameLength = 0;
        while (nameLength < kNameSlot && nameSlot[nameLength] != 0) {
            ++nameLength;
        }
        if (nameLength == kNameSlot) {
            fail(SessionError::protocol_error,
                "0x2000 role name misses the NUL in the 20-byte slot");
            return false;
        }
        if (nameLength == 0) {
            fail(SessionError::protocol_error,
                "0x2000 role record carries an empty name");
            return false;
        }
        role.nameGbk.assign(reinterpret_cast<const char*>(nameSlot), nameLength);
        for (const auto& existing : roles_) {
            if (existing.id == role.id) {
                fail(SessionError::protocol_error,
                    "0x2000 repeats an already listed role id");
                return false;
            }
        }
        for (const auto& added : parsed) {
            if (added.id == role.id) {
                fail(SessionError::protocol_error,
                    "0x2000 repeats a role id within one frame");
                return false;
            }
        }
        parsed.push_back(std::move(role));
    }
    roles_.insert(roles_.end(), parsed.begin(), parsed.end());
    return true;
}

bool GameSession::selectRole(std::uint32_t roleId,
                             std::chrono::milliseconds timeout) {
    if (state_ != SessionState::gatewayAuthenticating) {
        errorMessage_ = "selectRole() requires the gatewayAuthenticating state";
        return false;
    }
    if (roleSelected_) {
        errorMessage_ = "selectRole() already submitted";
        return false;
    }
    const RoleRecord* chosen = nullptr;
    for (const auto& role : roles_) {
        if (role.id == roleId) {
            chosen = &role;
            break;
        }
    }
    if (chosen == nullptr) {
        // Numeric id only: role names are never written to diagnostics.
        errorMessage_ = "selectRole() unknown role id " + std::to_string(roleId);
        return false;
    }
    // 0x100b empty follow-up, 14-byte payload; the evidence places it right
    // after the user's role choice and before the 0x2005 challenge.
    std::vector<std::uint8_t> followUp(14, 0);
    writeLe16(followUp.data(), 0x100b);
    if (gateway_.writeFrame(followUp.data(), followUp.size(), timeout) !=
            StreamStatus::ok) {
        fail(SessionError::disconnected,
            "gateway rejected the 0x100b follow-up: " + gateway_.lastError());
        return false;
    }
    selectedRole_ = *chosen;
    roleSelected_ = true;
    return true;
}

bool GameSession::submitGraphKey(const std::string& graphKey,
                                 std::chrono::milliseconds timeout) {
    if (state_ != SessionState::gatewayAuthenticating) {
        errorMessage_ = "submitGraphKey() requires the gatewayAuthenticating state";
        return false;
    }
    if (!roleSelected_) {
        errorMessage_ = "submitGraphKey() requires a prior role selection";
        return false;
    }
    if (!graphKeySeen_) {
        errorMessage_ = "submitGraphKey() before the 0x2005 graph-key challenge";
        return false;
    }
    if (graphKeySubmitted_) {
        errorMessage_ = "submitGraphKey() already submitted";
        return false;
    }
    // The graphkey.scp input caps at 7 characters; the shipped control takes
    // printable text. Printable non-space ASCII keeps the slot encoding
    // unambiguous; anything else (empty, oversized, NUL, high bytes, spaces)
    // is an input-level rejection: false + diagnostic, session untouched.
    constexpr std::size_t kGraphKeySlot = 7;  // +0x22..+0x28
    const bool graphKeyValid = !graphKey.empty() &&
        graphKey.size() <= kGraphKeySlot &&
        std::all_of(graphKey.begin(), graphKey.end(), [](char c) {
            return c >= 0x21 && c <= 0x7e;
        });
    if (!graphKeyValid) {
        errorMessage_ = "graph key empty, over the 7-byte slot, or outside "
            "printable ASCII";
        return false;
    }

    // 0x1002, 41-byte payload: selected role name at +0x0e (20-byte slot,
    // the parsed record is at most 19 bytes) and the submitted input at
    // +0x22 (7-byte slot). Nothing here reads the legacy credential fields.
    std::vector<std::uint8_t> frame(41, 0);
    writeLe16(frame.data(), 0x1002);
    std::memcpy(frame.data() + 0x0e, selectedRole_.nameGbk.data(),
                selectedRole_.nameGbk.size());
    std::memcpy(frame.data() + 0x22, graphKey.data(), graphKey.size());
    if (gateway_.writeFrame(frame.data(), frame.size(), timeout) !=
            StreamStatus::ok) {
        fail(SessionError::disconnected,
            "gateway rejected the 0x1002 frame: " + gateway_.lastError());
        return false;
    }
    graphKeySubmitted_ = true;
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
        if (!game_.connected()) {
            // Mid-frame timeout: the stream is already closed, so fail now
            // with the original diagnostic instead of polling a dead stream
            // (same policy as pumpGateway).
            fail(SessionError::disconnected,
                "game transport error: " + game_.lastError());
            fillErrorEvent(event);
            return true;
        }
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

bool GameSession::sendChat(std::uint32_t channel, const std::string& text,
                           std::chrono::milliseconds timeout) {
    if (state_ != SessionState::established) {
        errorMessage_ = "sendChat() requires the established state";
        return false;
    }
    auto gbk = utf8ToGbk(text);
    if (!gbk.has_value()) {
        errorMessage_ = "sendChat() text rejected: empty, invalid UTF-8, "
            "embedded NUL, unconvertible character, or over the byte bound";
        return false;
    }

    // Payload layout after FUN_007a2a00 (.rea/chat_send_packet_builder.json):
    // +0x00 cmd, +0x02 rand2, +0x06 channel, +0x0a rand1, +0x0e sequence
    // (written by sendGameFrame), +0x12 GBK text + NUL. The FUN_007a3400
    // length rule wireLength = GBK bytes + 0x13 stays inside the u16 length
    // field and the stream bound thanks to kMaxChatGbkBytes; sendGameFrame
    // enforces the frame bound and owns RC4, trailer and sequence (+2).
    std::vector<std::uint8_t> payload(0x12 + gbk->size() + 1, 0);
    writeLe16(payload.data(), 0x4050);
    writeLe32(payload.data() + 2, randSource_());
    writeLe32(payload.data() + 6, channel);
    writeLe32(payload.data() + 0x0a, randSource_());
    std::memcpy(payload.data() + 0x12, gbk->data(), gbk->size());
    secureZero(&(*gbk)[0], gbk->size());
    return sendGameFrame(payload.data(), payload.size(), timeout);
}

void GameSession::teardown() {
    gateway_.close();
    game_.close();
    secureZero(credentialBlob_.data(), credentialBlob_.size());
    credentialBlob_.clear();
    secureZero(&credentials_.account[0], credentials_.account.size());
    secureZero(&credentials_.password[0], credentials_.password.size());
    secureZero(&credentials_.securityCode[0], credentials_.securityCode.size());
    secureZero(&credentials_.roleNameGbk[0], credentials_.roleNameGbk.size());
    secureZero(&credentials_.roleCode[0], credentials_.roleCode.size());
    credentials_ = SessionCredentials{};
}

} // namespace dhsh
