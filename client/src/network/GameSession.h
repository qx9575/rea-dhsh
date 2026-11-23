#pragma once

#include "network/GameCrypto.h"
#include "network/Win32TcpStream.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dhsh {

// Task 11 Step 3: evidenced login/session core. The state machine implements
// only transitions observed in the 2026-11-23 double-account full-login
// capture (.rea/login_full_retry_20261123.pcapng) and fields documented in
// docs/formats/net-protocol.md:
//
//   gateway (plaintext):  0x1005 hello -> 0x1000 auth request (+0x1500 env
//     report, +0x100b) -> [0x1002 captcha submit] -> s2c 0x2002 redirect
//   game (encrypted):     0x4004 Blowfish login frame -> RC4 + MD5 trailer
//     for all later c2s frames; s2c table cipher per frame; documented init
//     sequence 0x4022 x2, 0x40d0 x4 (sub 7/2/1/3), 0x4665 after the first
//     inbound game frame.
//
// Anything the evidence does not cover fails closed: unknown gateway commands
// during the handshake, malformed or illegal redirects, timeouts past the
// caller-set authentication deadline, and disconnects all move the session to
// the terminal `failed` state. There is no fallback to replay or any other
// transport.

// Runtime-injected login material. The session never reads credentials from
// files or the command line; the caller owns these strings and may clear them
// after teardown(). The password is used only in memory to build wire digests
// and frames and is never copied into events, diagnostics, or logs.
struct SessionCredentials {
    std::string account;               // 0x1000 +0x0e / login blob +0x00
    std::string password;              // raw password text (in-memory only);
                                       // 0x1000 stores md5UpperHex(password) at +0x41
    std::string securityCode = "000000";  // 0x1000 +0xd2, exactly 6 alphanumeric bytes
                                       // (accountStruct+0xc4). The shipped login UI
                                       // pads shorter input with '0' and sends NUL
                                       // bytes when the account has no code bound;
                                       // this default stands in for accounts without
                                       // a code -- a documented design choice, not
                                       // proven universal. An account with a bound
                                       // code must pass the real 6 characters.
    std::string zoneNameGbk;           // 0x1000 +0x62 zone name (GBK)
    std::string versionToken;          // 0x1000 +0xaf version/permission string
    std::string macAddress;            // login blob +0x4b "aa:bb:cc:dd:ee:ff"
    std::string environmentReportGbk;  // 0x1500 CSV payload (GBK), provider-built
};

// Captcha-only gateway material. The 0x1000 request derives every
// password-derived byte itself (uppercase MD5 hex of the raw password at
// +0x41, closed by the 2026-11-23 static chain FUN_004038f0 -> FUN_007a3720
// (which calls FUN_008170f0 for the MD5) -> FUN_007a2a00, plus the
// login_full_retry capture), so the single field
// left here is the stored-password slot of the 0x1002 captcha submit, whose
// transform is still unclosed (docs/formats/net-protocol.md §8). Inject it
// via the authMaterial provider only for captcha flows; a normal 0x1000
// start() never requires it.
struct GatewayAuthMaterial {
    std::array<char, 8> storedPassword{};  // 0x1002 +0x0e (transform unclosed)
};

enum class SessionState {
    idle,                    // constructed, not started
    gatewayAuthenticating,   // connected to gateway; 0x1005/0x1000(+0x1500/0x100b) sent
    gameAuthenticating,      // redirect accepted; 0x4004 sent; RC4 session active
    established,             // first inbound game frame seen; session open
    failed,                  // terminal; see error()/errorMessage()
};

enum class SessionError {
    none,
    connect_failed,    // TCP connect to gateway or game server failed
    timeout,           // authentication deadline expired with no completion
    disconnected,      // peer closed or transport died before login completed
    illegal_redirect,  // 0x2002 failed validation (length/IP/port/session)
    unknown_command,   // gateway handshake frame outside the observed set
    protocol_error,    // malformed frame (bad length field, oversized, ...)
    bad_usage,         // API called in the wrong state
};

// RedirectInfo fields decoded from the 0x2002 frame. Offsets below are
// wire-frame relative (from the u16 length field, as in net-protocol.md §5.1);
// the implementation reads them payload-relative (2 bytes lower) because its
// receive buffer starts at the cmd.
struct RedirectInfo {
    std::uint32_t session = 0;  // u32le at wire +0x10 (payload +0x0e); echoed
                                // in the login blob credential field
    std::string host;           // dotted quad from the 4 network-order bytes at
                                // wire +0x14 (payload +0x12)
    std::uint16_t port = 0;     // u16le at wire +0x18 (payload +0x16)
};

struct SessionEvent {
    enum class Type {
        none,            // poll() waited without an event; session still running
        redirect,        // 0x2002 accepted; game connection + login frame sent
        gatewayFrame,    // validated gateway s2c frame (command + payload)
        gameFrame,       // decrypted game s2c frame (command + payload)
        error,           // session entered the failed state
    };

    Type type = Type::none;
    SessionState state = SessionState::idle;
    RedirectInfo redirect{};
    std::uint16_t command = 0;          // frame command (decrypted for game frames)
    std::vector<std::uint8_t> payload;  // full frame payload including the cmd u16
    SessionError error = SessionError::none;
    std::string message;                // diagnostic only; never contains credentials
};

class GameSession {
public:
    // authMaterial may be empty; when set it is invoked once per start() and
    // only feeds the 0x1002 captcha submit's stored-password field. The 0x1000
    // request derives its digest from `credentials` itself, so a normal start
    // never requires captcha material.
    explicit GameSession(
        SessionCredentials credentials,
        std::function<GatewayAuthMaterial(const SessionCredentials&)> authMaterial =
            {},
        std::uint32_t sequenceSeed = 1,
        std::chrono::milliseconds authDeadline = std::chrono::seconds(30));
    ~GameSession();

    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    SessionState state() const { return state_; }
    SessionError error() const { return error_; }
    const std::string& errorMessage() const { return errorMessage_; }
    const RedirectInfo& redirect() const { return redirect_; }

    // Connects to the gateway and sends the evidenced greeting/auth batch:
    // 0x1005, 0x1000, 0x1500, 0x100b. Fails closed on any transport error.
    bool start(const std::string& gatewayHost, std::uint16_t gatewayPort,
               std::chrono::milliseconds connectTimeout);

    // Sends 0x1002 with the caller-obtained captcha answer. Production logins
    // need the captcha image obtained out of band; loopback fixtures may
    // redirect without ever requiring this call.
    bool submitCaptcha(const std::string& captchaText);

    // Pumps the state machine with at most `timeout` of waiting. On a valid
    // 0x2002 the redirect is validated, the gateway socket is closed, and the
    // game connection + 0x4004 login frame are sent before the redirect event
    // is returned. Returns true when `event` was filled (including errors).
    bool poll(std::chrono::milliseconds timeout, SessionEvent* event);

    // Documented init prefix, byte-identical across both 2026-11-23 accounts
    // apart from the sequence field: 0x4022, 0x4022, 0x40d0 (sub 7/2/1/3),
    // 0x4665. Requires the established state. Later init frames (0x461c/
    // 0x463a/0x4631/0x4210) carry not-fully-closed dynamic fields and must be
    // sent by the caller via sendGameFrame() once their evidence closes.
    bool sendInitSequence(std::chrono::milliseconds timeout);

    // Sends one plaintext c2s game frame (payload includes the u16le command
    // at offset 0). RC4-encrypts, appends the MD5 trailer, and advances the
    // sequence field written at +0x0e. The 0x4004 login frame is sent
    // internally during poll(); callers must not resend it here.
    bool sendGameFrame(const std::uint8_t* payload, std::size_t length,
                       std::chrono::milliseconds timeout);

    // Sends one 0x4050 chat frame. `text` must be strict UTF-8; it is
    // converted to GBK (Win32 CP936) and rejected when empty, invalid UTF-8,
    // carrying an embedded NUL, containing characters unrepresentable in
    // GBK, or past the defensive byte boundary documented in
    // docs/formats/net-protocol.md (a replication-side bound, not an
    // evidenced original limit). Requires the established state. The text is
    // not echoed into any chat model and a write failure is never reported
    // as success. Wire layout (FUN_007a3400 -> FUN_007a2a00,
    // .rea/chat_send_packet_builder.json): u16le cmd 0x4050, u32le rand2 at
    // +2, u32le channel at +6, u32le rand1 at +0xa, u32le sequence at +0x0e
    // (written here through sendGameFrame, +2 per frame), GBK text + NUL at
    // +0x12; the length field = GBK byte count + 0x13.
    bool sendChat(std::uint32_t channel, const std::string& text,
                  std::chrono::milliseconds timeout);

    // Closes both sockets and overwrites the in-memory credential copies.
    void teardown();

private:
    SessionState fail(SessionError error, std::string message);
    void fillNoneEvent(SessionEvent* event) const;
    void fillErrorEvent(SessionEvent* event) const;
    bool beginGameLogin(std::chrono::milliseconds timeout, SessionEvent* event);
    bool pumpGateway(std::chrono::milliseconds timeout, SessionEvent* event);
    bool pumpGame(std::chrono::milliseconds timeout, SessionEvent* event);
    bool deadlineExpired() const;

    SessionCredentials credentials_;
    std::function<GatewayAuthMaterial(const SessionCredentials&)> authMaterial_;
    GatewayAuthMaterial material_;
    std::uint32_t sequenceField_ = 1;
    std::chrono::milliseconds authDeadline_;
    std::chrono::steady_clock::time_point authStart_;
    bool authClockRunning_ = false;

    Win32TcpStream gateway_;
    Win32TcpStream game_;
    std::vector<std::uint8_t> credentialBlob_;
    std::unique_ptr<Rc4Session> rc4_;
    // _rand() equivalent for the two 0x4050 rand fields: 15-bit non-negative
    // draws (MSVC RAND_MAX semantics). A per-session local source; nothing
    // here touches the process-global C rand state.
    std::function<std::uint32_t()> randSource_;

    SessionState state_ = SessionState::idle;
    SessionError error_ = SessionError::none;
    std::string errorMessage_;
    RedirectInfo redirect_;
};

} // namespace dhsh
