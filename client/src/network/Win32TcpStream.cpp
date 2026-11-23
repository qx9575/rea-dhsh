#include "network/Win32TcpStream.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <mutex>
#include <stdexcept>

namespace dhsh {

namespace {

std::mutex g_wsaMutex;
int g_wsaUsers = 0;

// WSAStartup/WSACleanup are reference counted so streams and fixtures can be
// created and destroyed in any order without leaking or double-stopping
// Winsock; the last user tears it down.
void wsaAcquire() {
    std::lock_guard<std::mutex> lock(g_wsaMutex);
    if (g_wsaUsers == 0) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("Win32TcpStream: WSAStartup failed");
        }
    }
    ++g_wsaUsers;
}

void wsaRelease() {
    std::lock_guard<std::mutex> lock(g_wsaMutex);
    --g_wsaUsers;
    if (g_wsaUsers == 0) {
        WSACleanup();
    }
}

std::string wsaErrorText(const char* operation, int code) {
    return std::string(operation) + " failed (wsa " + std::to_string(code) + ")";
}

SOCKET asSocket(std::uintptr_t value) {
    return static_cast<SOCKET>(value);
}

timeval toTimeval(std::chrono::milliseconds value) {
    const auto ms = value.count() < 1 ? 1 : value.count();
    timeval tv{};
    tv.tv_sec = static_cast<long>(ms / 1000);
    tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
    return tv;
}

// Winsock's SO_RCVTIMEO/SO_SNDTIMEO take a DWORD millisecond count (unlike
// the BSD timeval); passing anything else silently truncates.
void setSocketTimeout(SOCKET socket, int option,
                      std::chrono::milliseconds value) {
    const auto ms = value.count() < 1 ? 1 : value.count();
    DWORD timeout = static_cast<DWORD>(ms);
    ::setsockopt(socket, SOL_SOCKET, option,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

} // namespace

Win32TcpStream::Win32TcpStream(std::uint16_t maxFramePayload)
    : maxFramePayload_(maxFramePayload) {
    wsaAcquire();
}

Win32TcpStream::~Win32TcpStream() {
    close();
    wsaRelease();
}

Win32TcpStream::Win32TcpStream(Win32TcpStream&& other) noexcept
    : socket_(other.socket_),
      maxFramePayload_(other.maxFramePayload_),
      lastError_(std::move(other.lastError_)) {
    other.socket_ = kInvalidSocket;
}

Win32TcpStream& Win32TcpStream::operator=(Win32TcpStream&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = other.socket_;
        maxFramePayload_ = other.maxFramePayload_;
        lastError_ = std::move(other.lastError_);
        other.socket_ = kInvalidSocket;
    }
    return *this;
}

StreamStatus Win32TcpStream::connect(const std::string& host, std::uint16_t port,
                                     std::chrono::milliseconds timeout) {
    close();
    lastError_.clear();

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string portText = std::to_string(port);
    const int resolve = getaddrinfo(host.c_str(), portText.c_str(), &hints,
                                    &addresses);
    if (resolve != 0) {
        lastError_ = "resolve failed for host (wsa " +
            std::to_string(resolve) + ")";
        return StreamStatus::failed;
    }

    StreamStatus result = StreamStatus::failed;
    int lastCode = 0;
    for (addrinfo* entry = addresses; entry != nullptr; entry = entry->ai_next) {
        SOCKET candidate = ::socket(entry->ai_family, entry->ai_socktype,
                                    entry->ai_protocol);
        if (candidate == INVALID_SOCKET) {
            lastCode = WSAGetLastError();
            continue;
        }
        // Non-blocking connect bounded by select(), then back to blocking I/O.
        u_long nonBlocking = 1;
        ::ioctlsocket(candidate, FIONBIO, &nonBlocking);
        if (::connect(candidate, entry->ai_addr,
                      static_cast<int>(entry->ai_addrlen)) == 0) {
            result = StreamStatus::ok;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(candidate, &writable);
            fd_set except;
            FD_ZERO(&except);
            FD_SET(candidate, &except);
            const timeval tv = toTimeval(timeout);
            const int selected = ::select(0, nullptr, &writable, &except,
                                          &tv);
            if (selected == 0) {
                result = StreamStatus::timeout;
                lastCode = 0;
            } else if (selected > 0) {
                int socketError = 0;
                int length = sizeof(socketError);
                if (::getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                                 reinterpret_cast<char*>(&socketError),
                                 &length) == 0 &&
                    socketError == 0) {
                    result = StreamStatus::ok;
                } else {
                    lastCode = socketError != 0 ? socketError : WSAGetLastError();
                }
            } else {
                lastCode = WSAGetLastError();
            }
        } else {
            lastCode = WSAGetLastError();
        }
        if (result == StreamStatus::ok) {
            nonBlocking = 0;
            ::ioctlsocket(candidate, FIONBIO, &nonBlocking);
            socket_ = static_cast<std::uintptr_t>(candidate);
            break;
        }
        ::closesocket(candidate);
        if (result == StreamStatus::timeout) {
            break;
        }
    }
    freeaddrinfo(addresses);

    if (result != StreamStatus::ok) {
        lastError_ = result == StreamStatus::timeout
            ? "connect timed out"
            : wsaErrorText("connect", lastCode);
    }
    return result;
}

bool Win32TcpStream::connected() const {
    return socket_ != kInvalidSocket;
}

void Win32TcpStream::close() {
    if (socket_ != kInvalidSocket) {
        ::shutdown(asSocket(socket_), SD_BOTH);
        ::closesocket(asSocket(socket_));
        socket_ = kInvalidSocket;
    }
}

StreamStatus Win32TcpStream::writeFrame(const std::uint8_t* payload,
                                        std::size_t length,
                                        std::chrono::milliseconds timeout) {
    if (length > maxFramePayload_ || length > 0xffffU) {
        lastError_ = "payload exceeds the frame length limit";
        return StreamStatus::protocol_error;
    }
    if (payload == nullptr && length > 0) {
        throw std::invalid_argument("writeFrame: null payload");
    }
    if (!connected()) {
        lastError_ = "writeFrame on a closed stream";
        return StreamStatus::failed;
    }

    const auto deadline = SteadyClock::now() + timeout;
    std::uint8_t header[2] = {
        static_cast<std::uint8_t>(length & 0xffU),
        static_cast<std::uint8_t>((length >> 8) & 0xffU),
    };
    StreamStatus status = sendFull(header, sizeof(header), deadline);
    if (status == StreamStatus::ok && length > 0) {
        status = sendFull(payload, length, deadline);
    }
    // A frame that could not be sent completely must not be followed by
    // another frame on the same connection.
    if (status != StreamStatus::ok) {
        close();
    }
    return finish(status);
}

StreamStatus Win32TcpStream::readFrame(std::vector<std::uint8_t>& payload,
                                       std::chrono::milliseconds timeout) {
    payload.clear();
    if (!connected()) {
        lastError_ = "readFrame on a closed stream";
        return StreamStatus::failed;
    }
    const auto deadline = SteadyClock::now() + timeout;

    std::size_t consumed = 0;
    std::uint8_t header[2] = {};
    StreamStatus status = readFull(header, sizeof(header), deadline, &consumed);
    if (status != StreamStatus::ok) {
        return finishRead(status, consumed);
    }
    const std::uint16_t length = static_cast<std::uint16_t>(
        header[0] | (static_cast<std::uint16_t>(header[1]) << 8));
    if (length > maxFramePayload_) {
        close();
        lastError_ = "frame length " + std::to_string(length) +
            " exceeds the limit " + std::to_string(maxFramePayload_);
        return StreamStatus::protocol_error;
    }
    payload.resize(length);
    if (length == 0) {
        return StreamStatus::ok;
    }
    status = readFull(payload.data(), length, deadline, &consumed);
    if (status != StreamStatus::ok) {
        payload.clear();
        return finishRead(status, consumed);
    }
    return StreamStatus::ok;
}

StreamStatus Win32TcpStream::readFull(std::uint8_t* data, std::size_t length,
                                      SteadyClock::time_point deadline,
                                      std::size_t* progress) {
    std::size_t received = 0;
    while (received < length) {
        const auto now = SteadyClock::now();
        if (now >= deadline) {
            lastError_ = "operation timed out";
            *progress += received;
            return StreamStatus::timeout;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        setSocketTimeout(asSocket(socket_), SO_RCVTIMEO, remaining);
        const int n = ::recv(asSocket(socket_),
                             reinterpret_cast<char*>(data) + received,
                             static_cast<int>(length - received), 0);
        if (n > 0) {
            received += static_cast<std::size_t>(n);
            continue;
        }
        *progress += received;
        if (n == 0) {
            close();
            lastError_ = "peer closed the connection";
            return StreamStatus::closed;
        }
        const int code = WSAGetLastError();
        if (code == WSAETIMEDOUT) {
            lastError_ = "operation timed out";
            return StreamStatus::timeout;
        }
        lastError_ = wsaErrorText("recv", code);
        return StreamStatus::failed;
    }
    return StreamStatus::ok;
}

StreamStatus Win32TcpStream::sendFull(const std::uint8_t* data,
                                      std::size_t length,
                                      SteadyClock::time_point deadline) {
    std::size_t sent = 0;
    while (sent < length) {
        const auto now = SteadyClock::now();
        if (now >= deadline) {
            lastError_ = "operation timed out";
            return StreamStatus::timeout;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        setSocketTimeout(asSocket(socket_), SO_SNDTIMEO, remaining);
        const int n = ::send(asSocket(socket_),
                             reinterpret_cast<const char*>(data) + sent,
                             static_cast<int>(length - sent), 0);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        const int code = WSAGetLastError();
        if (code == WSAETIMEDOUT) {
            lastError_ = "operation timed out";
            return StreamStatus::timeout;
        }
        lastError_ = wsaErrorText("send", code);
        return StreamStatus::failed;
    }
    return StreamStatus::ok;
}

StreamStatus Win32TcpStream::finishRead(StreamStatus status, std::size_t consumed) {
    // Only an idle timeout (the deadline expired before any byte of the frame
    // was consumed) keeps the connection open. Every other failure leaves
    // unconsumed frame bytes that would desynchronize the next readFrame, so
    // the stream is closed and the caller must reconnect.
    if (status == StreamStatus::timeout && consumed == 0) {
        return status;
    }
    if (connected()) {
        close();
    }
    return status;
}

StreamStatus Win32TcpStream::finish(StreamStatus status) {
    if (status == StreamStatus::ok) {
        lastError_.clear();
    }
    return status;
}

} // namespace dhsh
