// Transport tests for Task 11 Step 2: Win32 TCP framing without any external
// network. Every test talks only to a loopback fixture server (127.0.0.1,
// ephemeral port) owned by this test process. No production server, no login
// flow, no credentials. GameSession itself lands in a later step; this file
// is named for the planned ctest target and currently covers the framed
// transport it will sit on.

#include "network/Win32TcpStream.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using dhsh::StreamStatus;
using dhsh::Win32TcpStream;
using namespace std::chrono_literals;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> makePayload(std::size_t size, std::uint8_t seed = 0) {
    std::vector<std::uint8_t> payload(size);
    for (std::size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<std::uint8_t>(seed + i * 7U);
    }
    return payload;
}

// [u16le length][payload]
std::vector<std::uint8_t> makeFrame(const std::vector<std::uint8_t>& payload) {
    check(payload.size() <= 0xffffU, "fixture frame exceeds the u16 length field");
    std::vector<std::uint8_t> frame(2 + payload.size());
    frame[0] = static_cast<std::uint8_t>(payload.size() & 0xffU);
    frame[1] = static_cast<std::uint8_t>((payload.size() >> 8) & 0xffU);
    std::copy(payload.begin(), payload.end(), frame.begin() + 2);
    return frame;
}

void sendBytes(SOCKET client, const std::vector<std::uint8_t>& bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int n = ::send(client, reinterpret_cast<const char*>(bytes.data() + sent),
                             static_cast<int>(bytes.size() - sent), 0);
        if (n <= 0) {
            throw std::runtime_error("fixture send failed (wsa " +
                std::to_string(WSAGetLastError()) + ")");
        }
        sent += static_cast<std::size_t>(n);
    }
}

// Minimal scripted TCP server on 127.0.0.1 with an ephemeral port. The worker
// thread accepts one connection at a time and executes steps queued by the
// test. Binding port 0 also proves repeated runs never fight over a fixed
// listening port, so nothing is left behind for the next ctest run.
class LoopbackServer {
public:
    LoopbackServer() {
        WSADATA data{};
        check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "fixture WSAStartup failed");
        listenSocket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        check(listenSocket_ != INVALID_SOCKET, "fixture socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        check(::bind(listenSocket_, reinterpret_cast<const sockaddr*>(&address),
                     sizeof(address)) == 0,
              "fixture bind failed (wsa " + std::to_string(WSAGetLastError()) + ")");
        check(::listen(listenSocket_, 4) == 0, "fixture listen failed");
        int length = sizeof(address);
        check(::getsockname(listenSocket_, reinterpret_cast<sockaddr*>(&address),
                            &length) == 0,
              "fixture getsockname failed");
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { run(); });
    }

    ~LoopbackServer() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        condition_.notify_all();
        wakeWorker();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (listenSocket_ != INVALID_SOCKET) {
            ::closesocket(listenSocket_);
        }
        WSACleanup();
    }

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    std::uint16_t port() const { return port_; }

    void push(std::function<void(SOCKET)> step) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            steps_.push_back(std::move(step));
        }
        condition_.notify_all();
    }

    // Queue a step that sends bytes in fixed-size chunks with a delay between
    // chunks; chunkSize 0 sends everything in one call.
    void sendStep(std::vector<std::uint8_t> bytes, std::size_t chunkSize,
                  int chunkDelayMs) {
        push([bytes = std::move(bytes), chunkSize, chunkDelayMs](SOCKET client) {
            std::size_t offset = 0;
            const std::size_t chunk = chunkSize == 0 ? bytes.size() : chunkSize;
            while (offset < bytes.size()) {
                const std::size_t count =
                    std::min(chunk, bytes.size() - offset);
                const int sent = ::send(client,
                    reinterpret_cast<const char*>(bytes.data() + offset),
                    static_cast<int>(count), 0);
                if (sent <= 0) {
                    throw std::runtime_error("fixture send failed (wsa " +
                        std::to_string(WSAGetLastError()) + ")");
                }
                offset += static_cast<std::size_t>(sent);
                if (chunkDelayMs > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(chunkDelayMs));
                }
            }
        });
    }

    // Queue a step that receives exactly count bytes, then publishes them for
    // awaitReceived(). Small per-read delays keep the server's buffer drained
    // slowly so the client must loop over partial sends.
    void recvExactStep(std::size_t count, int perRecvDelayMs) {
        push([this, count, perRecvDelayMs](SOCKET client) {
            std::vector<std::uint8_t> data(count);
            std::size_t received = 0;
            while (received < count) {
                const int n = ::recv(client,
                    reinterpret_cast<char*>(data.data() + received),
                    static_cast<int>(count - received), 0);
                if (n <= 0) {
                    throw std::runtime_error("fixture recv failed after " +
                        std::to_string(received) + " of " +
                        std::to_string(count) + " bytes");
                }
                received += static_cast<std::size_t>(n);
                if (perRecvDelayMs > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(perRecvDelayMs));
                }
            }
            std::lock_guard<std::mutex> lock(receiveMutex_);
            received_ = std::move(data);
            receivedPending_ = true;
            receiveCondition_.notify_all();
        });
    }

    // Queue a step that performs an orderly shutdown of the send side.
    void shutdownStep() {
        push([](SOCKET client) {
            ::shutdown(client, SD_SEND);
        });
    }

    std::vector<std::uint8_t> awaitReceived(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(receiveMutex_);
        if (!receiveCondition_.wait_for(lock, timeout,
                [this] { return receivedPending_; })) {
            throw std::runtime_error(
                "fixture did not receive the expected bytes in time");
        }
        receivedPending_ = false;
        std::vector<std::uint8_t> data = std::move(received_);
        received_.clear();
        return data;
    }

    // Wait until all queued steps finished executing.
    bool waitIdle(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] {
            return steps_.empty() && !executing_;
        });
    }

    // Wait until the worker is back at accept(), i.e. the previous connection
    // is fully closed and a new connect will land on a fresh socket.
    bool waitAccepting(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] { return accepting_; });
    }

    std::string stepError() {
        std::lock_guard<std::mutex> lock(mutex_);
        return stepError_;
    }

private:
    static bool clientGone(SOCKET client) {
        u_long nonBlocking = 1;
        ::ioctlsocket(client, FIONBIO, &nonBlocking);
        char byte = 0;
        const int peek = ::recv(client, &byte, 1, MSG_PEEK);
        const int error = WSAGetLastError();
        nonBlocking = 0;
        ::ioctlsocket(client, FIONBIO, &nonBlocking);
        return peek == 0 || (peek == SOCKET_ERROR && error != WSAEWOULDBLOCK);
    }

    void wakeWorker() {
        SOCKET wake = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (wake == INVALID_SOCKET) {
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port_);
        ::connect(wake, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        ::closesocket(wake);
    }

    void run() {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                accepting_ = true;
            }
            condition_.notify_all();
            SOCKET client = ::accept(listenSocket_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                accepting_ = false;
            }
            bool keepConnection = true;
            while (keepConnection) {
                std::function<void(SOCKET)> step;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    const bool ready = condition_.wait_for(lock, 25ms, [this] {
                        return quit_ || !steps_.empty();
                    });
                    if (quit_) {
                        lock.unlock();
                        ::closesocket(client);
                        return;
                    }
                    if (!ready) {
                        // Idle with an empty queue: drop the connection if the
                        // client already left so the next accept can proceed.
                        lock.unlock();
                        if (clientGone(client)) {
                            ::closesocket(client);
                            keepConnection = false;
                        }
                        continue;
                    }
                    step = std::move(steps_.front());
                    steps_.pop_front();
                    executing_ = true;
                }
                try {
                    step(client);
                } catch (const std::exception& error) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stepError_ = error.what();
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    executing_ = false;
                }
                condition_.notify_all();
            }
        }
    }

    SOCKET listenSocket_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void(SOCKET)>> steps_;
    bool executing_ = false;
    bool accepting_ = false;
    bool quit_ = false;
    std::string stepError_;
    std::mutex receiveMutex_;
    std::condition_variable receiveCondition_;
    std::vector<std::uint8_t> received_;
    bool receivedPending_ = false;
};

void test_read_single_frame() {
    LoopbackServer server;
    const auto payload = makePayload(64, 3);
    server.sendStep(makeFrame(payload), 0, 0);
    Win32TcpStream stream;
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    check(stream.connected(), "stream not connected after connect");
    std::vector<std::uint8_t> received;
    check(stream.readFrame(received, 2s) == StreamStatus::ok,
          "readFrame failed: " + stream.lastError());
    check(received == payload, "payload mismatch on single frame");
    check(server.stepError().empty(), "fixture error: " + server.stepError());
}

void test_partial_reads_assemble_one_frame() {
    LoopbackServer server;
    const auto payload = makePayload(100, 11);
    // Header and payload arrive in 3-byte chunks with gaps between them.
    server.sendStep(makeFrame(payload), 3, 2);
    Win32TcpStream stream;
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    std::vector<std::uint8_t> received;
    check(stream.readFrame(received, 10s) == StreamStatus::ok,
          "partial reads did not assemble a frame: " + stream.lastError());
    check(received == payload, "payload mismatch after partial reads");
    check(server.stepError().empty(), "fixture error: " + server.stepError());
}

void test_multiple_frames_arrive_in_order() {
    LoopbackServer server;
    const auto first = makePayload(5, 1);
    const auto second = makePayload(300, 2);
    const auto third = makePayload(17, 3);
    auto wire = makeFrame(first);
    const auto wire2 = makeFrame(second);
    const auto wire3 = makeFrame(third);
    wire.insert(wire.end(), wire2.begin(), wire2.end());
    wire.insert(wire.end(), wire3.begin(), wire3.end());
    server.sendStep(wire, 0, 0);
    Win32TcpStream stream;
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    std::vector<std::uint8_t> received;
    check(stream.readFrame(received, 2s) == StreamStatus::ok, "frame 1 failed");
    check(received == first, "frame 1 payload mismatch");
    check(stream.readFrame(received, 2s) == StreamStatus::ok, "frame 2 failed");
    check(received == second, "frame 2 payload mismatch");
    check(stream.readFrame(received, 2s) == StreamStatus::ok, "frame 3 failed");
    check(received == third, "frame 3 payload mismatch");
    check(server.stepError().empty(), "fixture error: " + server.stepError());
}

void test_zero_length_frame() {
    LoopbackServer server;
    server.sendStep(makeFrame({}), 0, 0);
    Win32TcpStream stream;
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    std::vector<std::uint8_t> received(4, 0xaa);
    check(stream.readFrame(received, 2s) == StreamStatus::ok,
          "zero-length frame rejected: " + stream.lastError());
    check(received.empty(), "zero-length frame did not produce an empty payload");
}

void test_oversized_length_is_rejected() {
    LoopbackServer server;
    // Declares 0xffff bytes; far above the default 8192 payload bound.
    server.sendStep({0xff, 0xff, 1, 2, 3, 4, 5, 6, 7, 8}, 0, 0);
    Win32TcpStream stream;
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    std::vector<std::uint8_t> received;
    check(stream.readFrame(received, 2s) == StreamStatus::protocol_error,
          "oversized frame length was not rejected");
    check(received.empty(), "rejected frame still produced payload");
    check(!stream.connected(),
          "stream stayed open after rejecting an oversized frame");
    check(!stream.lastError().empty(),
          "oversized frame rejection lost its diagnostic when the stream closed");
}

void test_configured_frame_limit_is_enforced() {
    LoopbackServer server;
    const auto payload = makePayload(5, 9);
    server.sendStep(makeFrame(payload), 0, 0);
    Win32TcpStream stream(4);
    check(stream.maxFramePayload() == 4, "custom frame limit not retained");
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    std::vector<std::uint8_t> received;
    check(stream.readFrame(received, 2s) == StreamStatus::protocol_error,
          "frame above the configured limit was not rejected");
    check(!stream.connected(),
          "stream stayed open after rejecting an oversized frame");
    check(!stream.lastError().empty(),
          "oversized frame rejection lost its diagnostic when the stream closed");
}

void test_write_loops_over_partial_sends() {
    LoopbackServer server;
    // Largest representable frame: the total wire size exceeds the default
    // send buffer, and the server drains slowly in small reads, so the
    // client's send path must loop over partial sends.
    Win32TcpStream stream(0xffff);
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    const auto payload = makePayload(65535, 5);
    server.recvExactStep(payload.size() + 2, 1);
    check(stream.writeFrame(payload.data(), payload.size(), 15s) == StreamStatus::ok,
          "large write failed: " + stream.lastError());
    const auto received = server.awaitReceived(20s);
    check(received == makeFrame(payload),
          "server received bytes differ from the sent frame");
    check(server.stepError().empty(), "fixture error: " + server.stepError());
}

void test_partial_frame_read_timeout_closes_stream() {
    LoopbackServer server;
    // Header promises a 20-byte payload but only 6 payload bytes arrive.
    server.sendStep({0x14, 0x00, 1, 2, 3, 4, 5, 6}, 0, 0);
    Win32TcpStream stream;
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    std::vector<std::uint8_t> received;
    check(stream.readFrame(received, 200ms) == StreamStatus::timeout,
          "partial frame did not time out: " + stream.lastError());
    check(received.empty(), "timed-out partial frame left payload behind");
    check(!stream.connected(),
          "stream stayed open after timing out mid-frame");
    check(!stream.lastError().empty(),
          "mid-frame timeout lost its diagnostic when the stream closed");
    check(stream.readFrame(received, 200ms) == StreamStatus::failed,
          "read on a mid-frame-closed stream did not fail");
}

void test_partial_header_read_timeout_closes_stream() {
    LoopbackServer server;
    // A single byte of the 2-byte length header arrives, then silence.
    server.sendStep({0x07}, 0, 0);
    Win32TcpStream stream;
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    std::vector<std::uint8_t> received;
    check(stream.readFrame(received, 200ms) == StreamStatus::timeout,
          "partial header did not time out: " + stream.lastError());
    check(!stream.connected(),
          "stream stayed open after a partial-header timeout");
    check(!stream.lastError().empty(),
          "partial-header timeout lost its diagnostic when the stream closed");
}

void test_failed_write_closes_stream() {
    LoopbackServer server;
    Win32TcpStream stream(0xffff);
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    // The server accepts and then aborts the connection with a RST; a frame
    // that cannot be written completely must close the stream, because a
    // half-written frame would corrupt the next writeFrame.
    server.push([](SOCKET client) {
        linger abort{1, 0};
        ::setsockopt(client, SOL_SOCKET, SO_LINGER,
                     reinterpret_cast<const char*>(&abort), sizeof(abort));
        ::closesocket(client);
    });
    check(server.waitIdle(2s), "abort step did not run");
    // The RST needs a moment to reach the client; probe with tiny frames and
    // stop as soon as one fails.
    StreamStatus status = StreamStatus::ok;
    for (int attempt = 0; attempt < 50 && status == StreamStatus::ok;
         ++attempt) {
        status = stream.writeFrame(nullptr, 0, 200ms);
        std::this_thread::sleep_for(10ms);
    }
    check(status != StreamStatus::ok,
          "aborted connection did not fail the write");
    check(!stream.lastError().empty(),
          "failed write lost its diagnostic when the stream closed");
    check(!stream.connected(), "stream stayed open after a failed write");
    check(stream.writeFrame(nullptr, 0, 200ms) == StreamStatus::failed,
          "write on a write-closed stream did not fail");
}

void test_orderly_close_is_reported() {
    LoopbackServer server;
    const auto payload = makePayload(32, 7);
    server.sendStep(makeFrame(payload), 0, 0);
    server.shutdownStep();
    Win32TcpStream stream;
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    std::vector<std::uint8_t> received;
    check(stream.readFrame(received, 2s) == StreamStatus::ok,
          "frame before close not delivered: " + stream.lastError());
    check(received == payload, "payload mismatch before close");
    check(stream.readFrame(received, 2s) == StreamStatus::closed,
          "orderly close not reported: " + stream.lastError());
    check(!stream.connected(), "stream still connected after peer close");
}

void test_read_timeout_then_recover() {
    LoopbackServer server;
    const auto payload = makePayload(48, 13);
    server.push([payload](SOCKET client) {
        std::this_thread::sleep_for(300ms);
        sendBytes(client, makeFrame(payload));
    });
    Win32TcpStream stream;
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    std::vector<std::uint8_t> received;
    check(stream.readFrame(received, 100ms) == StreamStatus::timeout,
          "idle peer did not time out");
    check(stream.connected(), "timeout dropped the connection");
    check(stream.readFrame(received, 3s) == StreamStatus::ok,
          "frame after timeout not delivered: " + stream.lastError());
    check(received == payload, "payload mismatch after timeout");
    check(server.stepError().empty(), "fixture error: " + server.stepError());
}

void test_reconnect_resets_receive_state() {
    LoopbackServer first;
    // Header declares a 20-byte payload but only 5 payload bytes arrive.
    first.sendStep({0x14, 0x00, 1, 2, 3, 4, 5}, 0, 0);
    Win32TcpStream stream;
    check(stream.connect("127.0.0.1", first.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    std::vector<std::uint8_t> received;
    check(stream.readFrame(received, 200ms) == StreamStatus::timeout,
          "partial frame did not time out");
    check(received.empty(), "timed-out frame left partial payload behind");

    LoopbackServer second;
    const auto fresh = makePayload(24, 21);
    second.sendStep(makeFrame(fresh), 0, 0);
    check(stream.connect("127.0.0.1", second.port(), 2s) == StreamStatus::ok,
          "reconnect failed: " + stream.lastError());
    check(stream.readFrame(received, 2s) == StreamStatus::ok,
          "frame after reconnect failed: " + stream.lastError());
    check(received == fresh, "stale bytes from the previous connection leaked");
}

void test_repeated_connections_and_wsa_lifecycle() {
    {
        LoopbackServer server;
        for (int i = 0; i < 8; ++i) {
            const auto payload = makePayload(8, static_cast<std::uint8_t>(i));
            check(server.waitAccepting(5s), "fixture did not return to accept()");
            server.sendStep(makeFrame(payload), 0, 0);
            Win32TcpStream stream;
            check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
                  "connect failed: " + stream.lastError());
            std::vector<std::uint8_t> received;
            check(stream.readFrame(received, 2s) == StreamStatus::ok,
                  "frame failed on repeated connection " + std::to_string(i));
            check(received == payload, "payload mismatch on repeated connection " +
                  std::to_string(i));
        }
        check(server.stepError().empty(), "fixture error: " + server.stepError());
    }
    // A fresh fixture and stream after all prior ones were destroyed proves
    // the reference-counted WSA startup/cleanup left Winsock usable.
    LoopbackServer server;
    const auto payload = makePayload(16, 42);
    server.sendStep(makeFrame(payload), 0, 0);
    Win32TcpStream moved;
    check(moved.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed after WSA lifecycle: " + moved.lastError());
    Win32TcpStream stream(std::move(moved));
    std::vector<std::uint8_t> received;
    check(stream.readFrame(received, 2s) == StreamStatus::ok,
          "readFrame failed on moved stream");
    check(received == payload, "payload mismatch on moved stream");
}

} // namespace

int main() {
    try {
        test_read_single_frame();
        test_partial_reads_assemble_one_frame();
        test_multiple_frames_arrive_in_order();
        test_zero_length_frame();
        test_oversized_length_is_rejected();
        test_configured_frame_limit_is_enforced();
        test_write_loops_over_partial_sends();
        test_orderly_close_is_reported();
        test_partial_frame_read_timeout_closes_stream();
        test_partial_header_read_timeout_closes_stream();
        test_failed_write_closes_stream();
        test_read_timeout_then_recover();
        test_reconnect_resets_receive_state();
        test_repeated_connections_and_wsa_lifecycle();
        std::cout << "game_session tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "game_session test failure: " << error.what() << '\n';
        return 1;
    }
}
