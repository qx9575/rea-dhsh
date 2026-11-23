#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dhsh {

// Result of a stream operation. Diagnostics live in Win32TcpStream::lastError
// and never contain payload bytes or credentials.
enum class StreamStatus {
    ok,
    closed,         // the peer shut down the connection
    timeout,        // the operation exceeded its deadline
    protocol_error, // malformed frame, e.g. a length above the configured bound
    failed,         // socket or resolver failure; see lastError()
};

// Blocking TCP stream for the game's framed protocol: every frame on the wire
// is [u16le length][payload]. Operations block with an explicit deadline, so a
// later synchronous GameSession can be pumped from the UI thread without busy
// spinning; an event-driven pump can replace the internals later without
// changing callers. Winsock startup/cleanup is reference counted across all
// instances, and reconnecting closes the previous socket first, resetting all
// per-connection state.
class Win32TcpStream {
public:
    static constexpr std::uint16_t kDefaultMaxFramePayload = 8192;

    explicit Win32TcpStream(
        std::uint16_t maxFramePayload = kDefaultMaxFramePayload);
    ~Win32TcpStream();

    Win32TcpStream(const Win32TcpStream&) = delete;
    Win32TcpStream& operator=(const Win32TcpStream&) = delete;
    Win32TcpStream(Win32TcpStream&& other) noexcept;
    Win32TcpStream& operator=(Win32TcpStream&& other) noexcept;

    // Resolves host (IPv4 dotted quad or DNS name) and connects with a bounded
    // wait. Calling connect on an already connected stream closes the old
    // connection first, so stale receive state can never leak into the new one.
    StreamStatus connect(const std::string& host, std::uint16_t port,
                         std::chrono::milliseconds timeout =
                             std::chrono::seconds(5));

    bool connected() const;

    // Idempotent; also invoked by the destructor. After close, connected() is
    // false and the stream may be reused via connect().
    void close();

    // Sends payload as one [u16le length][payload] frame, looping over partial
    // sends until every byte is handed to the kernel or the deadline passes.
    //
    // Safety policy: if the frame cannot be sent completely (timeout or socket
    // failure after a partial send), the stream is closed: a half-written
    // frame would corrupt the next writeFrame. A protocol_error rejection
    // happens before anything touches the socket and leaves the connection
    // open.
    StreamStatus writeFrame(const std::uint8_t* payload, std::size_t length,
                            std::chrono::milliseconds timeout =
                                std::chrono::seconds(5));

    // Receives exactly one frame into payload. Returns closed when the peer
    // shut down before a complete frame arrived, timeout when the deadline
    // passed first, or protocol_error when the declared length exceeds
    // maxFramePayload() (the frame is rejected and the connection is closed).
    //
    // Safety policy: an idle timeout (the deadline expired before any byte of
    // the frame was consumed) leaves the connection open. Every other
    // failure (timeout or protocol_error after part of the frame was read,
    // socket error, peer close) closes the stream: connected() becomes false
    // and the caller must reconnect, because the leftover frame bytes would
    // desynchronize the next readFrame.
    StreamStatus readFrame(std::vector<std::uint8_t>& payload,
                           std::chrono::milliseconds timeout =
                               std::chrono::seconds(5));

    std::uint16_t maxFramePayload() const { return maxFramePayload_; }

    // Diagnostic text for the most recent non-ok status.
    const std::string& lastError() const { return lastError_; }

private:
    using SteadyClock = std::chrono::steady_clock;

    StreamStatus readFull(std::uint8_t* data, std::size_t length,
                          SteadyClock::time_point deadline,
                          std::size_t* progress);
    StreamStatus sendFull(const std::uint8_t* data, std::size_t length,
                          SteadyClock::time_point deadline);
    // Applies the read-side safety policy from readFrame: only an idle timeout
    // (consumed == 0) keeps the connection; anything else closes it.
    StreamStatus finishRead(StreamStatus status, std::size_t consumed);
    StreamStatus finish(StreamStatus status);

    // Winsock SOCKET kept opaque so this header stays free of Windows types.
    std::uintptr_t socket_ = kInvalidSocket;
    std::uint16_t maxFramePayload_ = kDefaultMaxFramePayload;
    std::string lastError_;

    static constexpr std::uintptr_t kInvalidSocket =
        ~static_cast<std::uintptr_t>(0);
};

} // namespace dhsh
