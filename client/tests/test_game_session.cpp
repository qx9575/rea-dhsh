// Transport + GameSession core tests for Task 11 Steps 2/3. Every test talks
// only to loopback fixture servers (127.0.0.1, ephemeral ports) owned by this
// test process: one plays the plaintext login gateway, one the encrypted game
// server. No production server is ever contacted. All credentials are
// synthetic SYNTH_* values; the real 2026-11-23 capture material stays in
// .rea/ and is never embedded here.

#include "network/GameCrypto.h"
#include "network/GameSession.h"
#include "network/OnlineChatWorker.h"
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
#include <cstring>
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

    // Queue a step that waits up to `timeoutMs` for bytes and publishes
    // whatever arrived (possibly none). Asserting an empty result proves the
    // client sent nothing in the window -- e.g. no 0x100b before an explicit
    // role selection. Winsock takes a DWORD millisecond SO_RCVTIMEO (a BSD
    // timeval is silently misread); the timeout is scoped to this step and
    // restored to infinite afterwards so later exact-length receives behave.
    void recvQuietStep(int timeoutMs) {
        push([this, timeoutMs](SOCKET client) {
            DWORD timeout = static_cast<DWORD>(timeoutMs < 0 ? 0 : timeoutMs);
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&timeout),
                         sizeof(timeout));
            std::vector<std::uint8_t> data(256);
            const int n = ::recv(client, reinterpret_cast<char*>(data.data()),
                                 static_cast<int>(data.size()), 0);
            DWORD infinite = 0;
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&infinite),
                         sizeof(infinite));
            std::lock_guard<std::mutex> lock(receiveMutex_);
            received_.assign(data.begin(),
                             n > 0 ? data.begin() + n : data.begin());
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

// Header fully consumed, payload never arrives: the 2026-11-24 online root
// cause. The complete length header was taken off the wire, so a timeout now
// must close the stream -- leaving it open would leave the connection
// positioned mid-frame and the next readFrame would misinterpret the late
// payload's first bytes as a new length header (the observed "frame length
// 8195" desync). Only a timeout with zero bytes consumed may keep the
// connection (see test_read_timeout_then_recover).
void test_header_only_read_timeout_closes_stream() {
    LoopbackServer server;
    // A complete 14B-payload length header, then silence.
    server.sendStep({0x0e, 0x00}, 0, 0);
    Win32TcpStream stream;
    check(stream.connect("127.0.0.1", server.port(), 2s) == StreamStatus::ok,
          "connect failed: " + stream.lastError());
    std::vector<std::uint8_t> received;
    check(stream.readFrame(received, 200ms) == StreamStatus::timeout,
          "header-only frame did not time out: " + stream.lastError());
    check(received.empty(), "header-only timeout left payload behind");
    check(!stream.connected(),
          "stream stayed open after a timeout that consumed the header");
    check(!stream.lastError().empty(),
          "header-only timeout lost its diagnostic when the stream closed");
    check(stream.readFrame(received, 200ms) == StreamStatus::failed,
          "read on a header-consumed-closed stream did not fail");
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

// ---------------------------------------------------------------------------
// GameSession core tests (Task 11 Step 3). The fixture pair below simulates
// both ends evidenced in the 2026-11-23 capture: a plaintext login gateway
// (0x1005/0x1000/0x1500/0x100b/0x1002 c2s in the captured start order,
// 0x2002 redirect s2c) and the encrypted game server (0x4004 Blowfish login,
// RC4 + trailer afterwards, table-cipher s2c). 0x1002 is role selection
// (FUN_005277e0/FUN_00528190, .rea/auth_1002_builders.json), not a captcha
// submit. All values are SYNTH_* fiction on 127.0.0.1.

using dhsh::GameSession;
using dhsh::SessionCredentials;
using dhsh::SessionError;
using dhsh::SessionEvent;
using dhsh::SessionState;

constexpr std::uint32_t kSynthSession = 0x51face42;
const std::string kSynthAccount = "SYNTH_ACCT_ZZ_0001";
const std::string kSynthPassword = "SYNTH-PW-9f8e7d";
const std::string kSynthMac = "aa:bb:cc:dd:ee:ff";
// Fictional zone name in GBK bytes; the codec treats it as opaque bytes.
const std::string kSynthZoneGbk = "\xb2\xe2\xca\xd4\xc7\xf8";
const std::string kSynthVersion = "SYNTH-VER,2-6-2,9-11,";
// Synthetic CSV payload with a GBK label byte sequence (time: ...).
const std::string kSynthEnvReport =
    "\"\xca\xb1\xbc\xe4\xa3\xba" "2026-11-23 00:00:00\",\"SYNTH-OS\"";
// 6-byte synthetic security code (alphanumeric, as the shipped UI requires).
const std::string kSynthSecurityCode = "SYNTH1";
// Synthetic role material for the role stage: 0x2000 carries a 14-byte header
// plus N consecutive 0x85-byte role records (id u32le at record+0, GBK name
// in the 20-byte slot at record+4 -- 19 bytes max plus the NUL); 0x1002
// carries the selected record's name at +0x0e (20-byte slot) and the
// graph-key input at +0x22 (7-byte slot, .rea/auth_1002_builders.json +
// graphkey.scp's 7-character cap). Role A's name is 角色, role B's is 测试
// (both GBK); the SYNTH "role code" doubles as the graph-key text the tests
// type after the 0x2005 challenge.
const std::uint32_t kSynthRoleIdA = 0x00005101;
const std::uint32_t kSynthRoleIdB = 0x00005102;
const std::string kSynthRoleNameGbk = "\xbd\xc5\xc9\xab";   // role A
const std::string kSynthRoleNameGbkB = "\xb2\xe2\xca\xd4";  // role B
const std::string kSynthRoleCode = "SYN01";  // SYNTH graph-key text

// The two-role SYNTH set most fixtures send in one 0x2000.
std::vector<std::pair<std::uint32_t, std::string>> synthRoleSet() {
    return {{kSynthRoleIdA, kSynthRoleNameGbk},
            {kSynthRoleIdB, kSynthRoleNameGbkB}};
}
// MD5(kSynthPassword) as 32 uppercase hex chars, computed offline with an
// independent implementation; pins the digest the session must derive.
const char kSynthPasswordDigest[] = "5E5C48F1F02F4F116ED141291FB44FD2";

SessionCredentials synthCredentials() {
    SessionCredentials credentials;
    credentials.account = kSynthAccount;
    credentials.password = kSynthPassword;
    credentials.securityCode = kSynthSecurityCode;
    credentials.zoneNameGbk = kSynthZoneGbk;
    credentials.versionToken = kSynthVersion;
    credentials.macAddress = kSynthMac;
    credentials.environmentReportGbk = kSynthEnvReport;
    return credentials;
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

void writeLe32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value & 0xffU);
    p[1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    p[2] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    p[3] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
}

std::uint8_t rol8(std::uint8_t value, unsigned shift) {
    if (shift == 0) {
        return value;
    }
    return static_cast<std::uint8_t>((value << shift) | (value >> (8 - shift)));
}

// Exact inverse of dhsh::S2cTableCipher::decryptInPlace so the fixture can
// encrypt synthetic s2c game frames onto the real wire format: the client
// reads the pos seed from the cipher payload at +0x0e (payload-relative, cmd
// included, per net-protocol.md section 3) before undoing the head rotation,
// so the seed field [0x0e, 0x13) must stay raw, bytes below 0x0e carry both
// the body inverse and the head rotation, and the rest carry the body only.
// A roundtrip against the sanitized real 30a0 vector below pins this to the
// captured wire format.
void tableCipherEncryptInPlace(std::uint8_t* payload, std::size_t length) {
    const auto& key = dhsh::s2cKeyTable();
    if (length < 0x12) {
        return;
    }
    std::uint32_t pos = (readLe32(payload + 0x0e) - 0x0dU) & 0xffffffffU;
    if (pos > 0xfffU) {
        pos = 0;
    }
    for (std::size_t i = 0; i < length; ++i) {
        if (i >= 0x0e && i < 0x13) {
            continue;
        }
        const unsigned shift = pos & 7U;
        const std::uint8_t plain = i < 16 ? rol8(payload[i], 3) : payload[i];
        const std::uint8_t mixed = static_cast<std::uint8_t>(plain ^ key[pos]);
        payload[i] = rol8(mixed, shift);
        pos = (pos + 1) & 0xfffU;
    }
}


// Sanitized real s2c 0x30a0 frame (numeric-only payload, no names/chat/
// credentials), identical to the vectors in test_game_crypto.cpp. Used to
// prove the fixture encryptor above reproduces the captured wire format:
// encrypt(plain) must equal the on-wire cipher byte for byte.
namespace {
static const std::uint8_t kS2cReal30a0PlainRef[] = {
    0xa0, 0x30, 0x00, 0x00, 0x00, 0x00, 0x24, 0xaa, 0x6b, 0x01, 0x01, 0x00, 0x00, 0x00, 0x50, 0x40, 0x00, 0x00, 0xbf, 0x00,
    0x67, 0x00, 0x01, 0x00, 0xd2, 0x69, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x71, 0x00, 0x28, 0x00, 0x64, 0x00, 0x00, 0x00,
    0x79, 0x00, 0x00, 0x00, 0x79, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79, 0x00, 0x00, 0x06, 0xbc, 0x1b, 0x00, 0x00,
    0xbc, 0x1b, 0x00, 0x00, 0xbd, 0x1b, 0xe6, 0x00, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd0, 0x00, 0x3a, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,
};

static const std::uint8_t kS2cReal30a0CipherRef[] = {
    0x5f, 0xff, 0x14, 0xc6, 0x64, 0x9d, 0x6c, 0x62, 0xc1, 0xd1, 0x42, 0xb0, 0x4e, 0x6d, 0x82, 0x02, 0x00, 0x00, 0xbf, 0x4b,
    0xfa, 0x0b, 0xf2, 0xdd, 0x8f, 0x95, 0xff, 0x0f, 0xeb, 0x22, 0x3c, 0x5c, 0xe9, 0x7a, 0x4b, 0x80, 0xcd, 0x70, 0x48, 0xfe,
    0x15, 0x6b, 0xf3, 0x52, 0x22, 0x7a, 0x74, 0x96, 0x79, 0xc9, 0x4d, 0xd4, 0xc1, 0xa8, 0x3e, 0x58, 0x00, 0xa4, 0x3a, 0x96,
    0xc2, 0x91, 0xfe, 0xa5, 0x67, 0xd3, 0xec, 0x0f, 0x50, 0xcf, 0xf2, 0x52, 0x41, 0x26, 0xac, 0x13, 0x2d, 0xd8, 0x39, 0x67,
    0xda, 0x95, 0x80, 0x7f, 0x10, 0x89, 0xdd, 0x1b, 0xfe, 0x3d, 0xf9, 0x86, 0x4b, 0xe3, 0x7c, 0x95, 0x4d, 0xb5, 0x6f, 0xac,
    0x89, 0x32, 0x24, 0x57, 0x71, 0x1d, 0xb6, 0x57, 0x0a, 0xba, 0xe4, 0xb5, 0xd0, 0xa3, 0x3a, 0x86, 0x0e, 0xa4, 0xf1, 0xd7,
    0x2f,
};

} // namespace

void test_fixture_s2c_encrypt_matches_capture() {
    check(sizeof(kS2cReal30a0PlainRef) == sizeof(kS2cReal30a0CipherRef),
          "30a0 reference vector length mismatch");
    // The decrypted reference plaintext carries the client's head rotation at
    // [0x0e/0x0f] (the [0x0e, 0x13) seed field is cipher metadata, not
    // plaintext content); restore the wire form of those two bytes before
    // encrypting so the comparison covers every byte the fixture encryptor
    // actually produces.
    std::vector<std::uint8_t> wireForm(kS2cReal30a0PlainRef,
        kS2cReal30a0PlainRef + sizeof(kS2cReal30a0PlainRef));
    wireForm[0x0e] = rol8(wireForm[0x0e], 3);
    wireForm[0x0f] = rol8(wireForm[0x0f], 3);
    tableCipherEncryptInPlace(wireForm.data(), wireForm.size());
    check(wireForm == std::vector<std::uint8_t>(kS2cReal30a0CipherRef,
            kS2cReal30a0CipherRef + sizeof(kS2cReal30a0CipherRef)),
          "fixture s2c encryptor does not reproduce the captured wire format");
}

// Builds one synthetic s2c game frame: cmd + body (body < 12 bytes so the
// cipher seed field at +0x0e stays fixture-owned), encrypted with the table.
std::vector<std::uint8_t> makeS2cGameFrame(std::uint16_t command,
                                           std::vector<std::uint8_t> body) {
    check(body.size() <= 10, "fixture s2c body overlaps the seed field");
    std::vector<std::uint8_t> plain(2 + body.size());
    plain[0] = static_cast<std::uint8_t>(command & 0xffU);
    plain[1] = static_cast<std::uint8_t>(command >> 8);
    std::copy(body.begin(), body.end(), plain.begin() + 2);
    if (plain.size() < 0x12) {
        plain.resize(0x12, 0);
    }
    plain[0x0e] = 0x39;  // arbitrary valid seed; bytes 0x0e..0x12 pass through
    plain[0x0f] = 0x00;
    plain[0x10] = 0x00;
    plain[0x11] = 0x00;
    tableCipherEncryptInPlace(plain.data(), plain.size());
    return makeFrame(plain);
}

// Builds the evidenced gateway redirect frame (26 bytes on the wire):
// +0 u16le length=24, +2 cmd=0x2002, +0x10 u32le session, +0x14 4B IPv4
// network order, +0x18 u16le port (wire-frame offsets). The payload buffer
// below starts at the cmd, so the coded fields sit payload-relative at
// 0x0e / 0x12 / 0x16 (2 bytes lower than the wire offsets).
std::vector<std::uint8_t> makeRedirectFrame(std::uint32_t session,
                                            std::uint8_t ip0, std::uint8_t ip1,
                                            std::uint8_t ip2, std::uint8_t ip3,
                                            std::uint16_t port) {
    std::vector<std::uint8_t> payload(24, 0);
    payload[0] = 0x02;
    payload[1] = 0x20;
    payload[0x0e] = static_cast<std::uint8_t>(session & 0xffU);
    payload[0x0f] = static_cast<std::uint8_t>((session >> 8) & 0xffU);
    payload[0x10] = static_cast<std::uint8_t>((session >> 16) & 0xffU);
    payload[0x11] = static_cast<std::uint8_t>((session >> 24) & 0xffU);
    payload[0x12] = ip0;
    payload[0x13] = ip1;
    payload[0x14] = ip2;
    payload[0x15] = ip3;
    payload[0x16] = static_cast<std::uint8_t>(port & 0xffU);
    payload[0x17] = static_cast<std::uint8_t>(port >> 8);
    return makeFrame(payload);
}

// ---------------------------------------------------------------------------
// Role-stage fixtures (2026-11-24 design closure). A 0x2000 payload is a
// 14-byte header plus N consecutive 0x85-byte role records; record+0 is the
// u32le role id, record+4 is the GBK name in the 20-byte wire slot (19 bytes
// max plus the NUL terminator). A 0x2005 is the graph-key challenge in the
// Task 2 encoding: 0x16-byte header (cmd, width u16le at +0x12, height u16le
// at +0x14) followed by width*height little-endian RGB565 pixels XORed with a
// repeating two-byte key, phase 0 at payload +0x16 (the black background is
// the most frequent aligned ciphertext u16, so the decoder's key recovery
// holds by construction). All ids/names/pixels are SYNTH fiction; the same
// builder feeds every 0x2005 the fixtures send, before and after the role
// selection.

constexpr std::size_t kRoleHeaderSize = 0x0e;
constexpr std::size_t kRoleRecordSize = 0x85;
constexpr std::size_t kRoleNameSlot = 20;  // 19 bytes max + NUL
constexpr std::size_t kRoleCodeSlot = 7;   // 0x1002 graph-key slot

std::vector<std::uint8_t> makeRoleRecord(std::uint32_t id,
                                         const std::string& nameGbk) {
    check(nameGbk.size() < kRoleNameSlot,
          "fixture role name exceeds the 20-byte wire slot");
    std::vector<std::uint8_t> record(kRoleRecordSize, 0);
    writeLe32(record.data(), id);
    std::memcpy(record.data() + 4, nameGbk.data(), nameGbk.size());
    return record;
}

std::vector<std::uint8_t> makeRoleListFrame(
        const std::vector<std::pair<std::uint32_t, std::string>>& roles) {
    std::vector<std::uint8_t> payload(kRoleHeaderSize, 0);
    payload[0] = 0x00;
    payload[1] = 0x20;
    for (const auto& role : roles) {
        const auto record = makeRoleRecord(role.first, role.second);
        payload.insert(payload.end(), record.begin(), record.end());
    }
    return makeFrame(payload);
}

// Synthetic 96x32 graph-key image: black background with a handful of known
// colored pixels, enough to pin the decode pixel by pixel without resembling
// any real captcha content.
constexpr std::uint16_t kGraphKeyImageWidth = 96;
constexpr std::uint16_t kGraphKeyImageHeight = 32;

std::vector<std::uint16_t> synthGraphKeyPixels() {
    std::vector<std::uint16_t> pixels(
        static_cast<std::size_t>(kGraphKeyImageWidth) * kGraphKeyImageHeight, 0x0000);
    auto set = [&pixels](std::size_t x, std::size_t y, std::uint16_t v) {
        pixels[y * kGraphKeyImageWidth + x] = v;
    };
    set(0, 0, 0xf800);        // red
    set(95, 0, 0x07e0);       // green
    set(0, 31, 0x001f);       // blue
    set(95, 31, 0xffff);      // white
    set(48, 16, 0xf81f);      // magenta
    set(10, 5, 0x07ff);       // cyan
    set(20, 27, 0xffe0);      // yellow
    set(33, 8, 0x8410);       // mid gray
    set(80, 20, 0x2104);      // dark pixel
    return pixels;
}

// Independent RGB565 -> RGBA expansion (r*255/31, g*255/63), duplicated from
// test_graphkey.cpp so the loopback pixel expectations stay a separate
// implementation from the decoder under test.
std::uint8_t expand5To8(std::uint8_t v) {
    return static_cast<std::uint8_t>(v * 255 / 31);
}

std::uint8_t expand6To8(std::uint8_t v) {
    return static_cast<std::uint8_t>(v * 255 / 63);
}

std::vector<std::uint8_t> expectedGraphKeyRgba(
        const std::vector<std::uint16_t>& pixels) {
    std::vector<std::uint8_t> rgba;
    rgba.reserve(pixels.size() * 4);
    for (const std::uint16_t v : pixels) {
        rgba.push_back(expand5To8(static_cast<std::uint8_t>((v >> 11) & 0x1f)));
        rgba.push_back(expand6To8(static_cast<std::uint8_t>((v >> 5) & 0x3f)));
        rgba.push_back(expand5To8(static_cast<std::uint8_t>(v & 0x1f)));
        rgba.push_back(255);
    }
    return rgba;
}

// Wraps the pixel stream in the Task 2 0x2005 encoding with the given key;
// the most frequent aligned ciphertext u16 is then exactly the key (the
// black background dominates), matching the decoder's recovery premise.
std::vector<std::uint8_t> encodeGraphKeyPayload(
        const std::vector<std::uint16_t>& pixels, std::uint8_t key0,
        std::uint8_t key1) {
    std::vector<std::uint8_t> payload(0x16 + pixels.size() * 2, 0);
    payload[0x00] = 0x05;  // 0x2005
    payload[0x01] = 0x20;
    payload[0x12] = static_cast<std::uint8_t>(kGraphKeyImageWidth & 0xffU);
    payload[0x13] = static_cast<std::uint8_t>(kGraphKeyImageWidth >> 8);
    payload[0x14] = static_cast<std::uint8_t>(kGraphKeyImageHeight & 0xffU);
    payload[0x15] = static_cast<std::uint8_t>(kGraphKeyImageHeight >> 8);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        payload[0x16 + i * 2] =
            static_cast<std::uint8_t>((pixels[i] & 0xffU) ^ key0);
        payload[0x16 + i * 2 + 1] =
            static_cast<std::uint8_t>((pixels[i] >> 8) ^ key1);
    }
    return payload;
}

// One legal synthetic 96x32 RGB565 graph-key challenge frame (synthetic key
// bytes, not the observed production values).
std::vector<std::uint8_t> makeGraphKeyFrame() {
    return makeFrame(encodeGraphKeyPayload(synthGraphKeyPixels(), 0x3c, 0xa5));
}

// A same-size 0x2005 whose pixel stream has no dominant background (four
// evenly spread ciphertext values): the decoder must reject it as
// weak_background. Never usable as a fake-empty payload stand-in.
std::vector<std::uint8_t> makeInvalidGraphKeyFrame() {
    std::vector<std::uint16_t> pixels(
        static_cast<std::size_t>(kGraphKeyImageWidth) * kGraphKeyImageHeight);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<std::uint16_t>(0x1000 * (i % 4));
    }
    return makeFrame(encodeGraphKeyPayload(pixels, 0x00, 0x00));
}

void expectZeros(const std::uint8_t* data, std::size_t length,
                 const std::string& message) {
    for (std::size_t i = 0; i < length; ++i) {
        check(data[i] == 0, message + " (nonzero at offset " + std::to_string(i) + ")");
    }
}

// Public known-answer vectors (RFC 1321 suite) pinning dhsh::md5UpperHex,
// plus the offline-computed SYNTH password digest. Non-sensitive standard
// vectors only; they guard the digest the session writes at 0x1000 +0x41.
void test_md5_known_answers() {
    const struct {
        const char* input;
        const char* expected;  // 32 uppercase hex characters
    } vectors[] = {
        {"", "D41D8CD98F00B204E9800998ECF8427E"},
        {"abc", "900150983CD24FB0D6963F7D28E17F72"},
        {"123456789012345678901234567890123456789012345678901234567890"
         "12345678901234567890",
         "57EDF4A22BE3C955AC49DA2E2107B67A"},
    };
    for (const auto& vector : vectors) {
        const auto hex = dhsh::md5UpperHex(
            reinterpret_cast<const std::uint8_t*>(vector.input),
            std::strlen(vector.input));
        check(std::memcmp(hex.data(), vector.expected, 32) == 0,
              std::string("md5UpperHex known-answer mismatch for input '") +
                  vector.input + "'");
    }
    const auto synth = dhsh::md5UpperHex(
        reinterpret_cast<const std::uint8_t*>(kSynthPassword.data()),
        kSynthPassword.size());
    check(std::memcmp(synth.data(), kSynthPasswordDigest, 32) == 0,
          "md5UpperHex(SYNTH password) differs from the offline-computed value");
}

// Validates the 0x1000 gateway auth request byte layout against the offsets
// closed by the 2026-11-23 static chain (FUN_004038f0 -> FUN_007a3720, which
// calls FUN_008170f0 for the MD5, -> FUN_007a2a00; .rea/auth_*.json) and the
// login_full_retry capture. Payload offsets are cmd-relative: the serializer
// FUN_007a2a00 copies a 16-byte
// header (u32 counter = 1 at +2) plus 210 bytes from accountStruct, so
// accountStruct+0x33 (the raw password slot, uppercased MD5-hexed in place by
// FUN_007a3720 for cmd 0x1000) maps to payload +0x41. Only SYNTH values are
// compared.
void validateAuthRequest(const std::vector<std::uint8_t>& wire) {
    check(wire.size() == 226, "0x1000 wire size mismatch");
    const auto* p = wire.data() + 2;  // skip the length header
    check(readLe16(p) == 0x1000, "0x1000 command mismatch");

    // Account at +0x0e, NUL padded up to the password slot at +0x41.
    check(std::memcmp(p + 0x0e, kSynthAccount.data(), kSynthAccount.size()) == 0,
          "0x1000 account field mismatch");
    expectZeros(p + 0x0e + kSynthAccount.size(),
                0x41 - (0x0e + kSynthAccount.size()), "0x1000 account not NUL padded");

    // Password digest slot: 32 uppercase MD5 hex characters of the raw
    // password at +0x41..+0x60, NUL at +0x61. The previously documented
    // "+0x40 digest / +0x60 u16 param" reading is disproven: +0x40 is the
    // account field's last NUL-pad byte and the "u16" was only the digest's
    // final ASCII character (0x34='4' / 0x45='E') plus this terminator.
    const auto digest = dhsh::md5UpperHex(
        reinterpret_cast<const std::uint8_t*>(kSynthPassword.data()),
        kSynthPassword.size());
    check(std::memcmp(digest.data(), kSynthPasswordDigest, 32) == 0,
          "session MD5 derivation differs from the offline-computed digest");
    check(std::memcmp(p + 0x41, digest.data(), 32) == 0,
          "0x1000 password digest field mismatch");
    check(p[0x61] == 0, "0x1000 digest not NUL terminated at +0x61");

    // 16-byte wire header copied verbatim for cmd < 0x1501: u32 counter = 1
    // at +2 (FUN_007a3720 local_14), zeros through +0x0d.
    check(readLe32(p + 2) == 1, "0x1000 header counter mismatch");
    expectZeros(p + 6, 8, "0x1000 header not zeroed");

    check(std::memcmp(p + 0x62, kSynthZoneGbk.data(), kSynthZoneGbk.size()) == 0,
          "0x1000 zone field mismatch");
    expectZeros(p + 0x62 + kSynthZoneGbk.size(),
                0x7b - (0x62 + kSynthZoneGbk.size()), "0x1000 zone not NUL padded");
    check(std::memcmp(p + 0x7b, "000000000000", 12) == 0, "0x1000 +0x7b marker mismatch");
    expectZeros(p + 0x87, 0xaf - 0x87, "0x1000 +0x87 region not zeroed");
    check(std::memcmp(p + 0xaf, kSynthVersion.data(), kSynthVersion.size()) == 0,
          "0x1000 version token mismatch");
    expectZeros(p + 0xaf + kSynthVersion.size(),
                0xd2 - (0xaf + kSynthVersion.size()), "0x1000 version not NUL padded");

    // 6-byte security code at +0xd2..+0xd7 (accountStruct+0xc4; the shipped
    // login UI stores it at loginObject+0x598). The previously documented
    // "+0xd6 two-character slot" was only this code's tail characters.
    check(std::memcmp(p + 0xd2, kSynthSecurityCode.data(), 6) == 0,
          "0x1000 security code field mismatch");
    expectZeros(p + 0xd8, 4, "0x1000 +0xd8 padding mismatch");
    check(readLe32(p + 0xdc) == 1, "0x1000 +0xdc constant mismatch");
}

void validateHelloOrEmpty(const std::vector<std::uint8_t>& wire,
                          std::uint16_t command, const std::string& name) {
    check(wire.size() == 16, name + " wire size mismatch");
    const auto* p = wire.data() + 2;
    check(readLe16(p) == command, name + " command mismatch");
    if (command == 0x1005) {
        check(p[2] == 0x0a && p[3] == 0x00, name + " +2 constant mismatch");
        expectZeros(p + 4, 10, name + " body not zeroed");
    } else {
        expectZeros(p + 2, 12, name + " body not zeroed");
    }
}

void validateEnvReport(const std::vector<std::uint8_t>& wire) {
    const std::size_t payloadSize = 0x0e + kSynthEnvReport.size();
    check(wire.size() == 2 + payloadSize, "0x1500 wire size mismatch");
    const auto* p = wire.data() + 2;
    check(readLe16(p) == 0x1500, "0x1500 command mismatch");
    expectZeros(p + 2, 0x0c, "0x1500 header not zeroed");
    check(std::memcmp(p + 0x0e, kSynthEnvReport.data(), kSynthEnvReport.size()) == 0,
          "0x1500 report text mismatch");
}

// start() sends exactly three frames in the evidenced order: 0x1005 hello,
// 0x1000 auth request, 0x1500 environment report. The 0x100b follow-up and
// the 0x1002 role/graph frame leave only after the explicit role-selection
// and graph-key APIs (2026-11-24 design); role fields are not part of the
// credentials anymore. Each receive step is queued right before its await so
// the fixture never holds two completed receives.
void expectGatewayStartBatch(LoopbackServer* gateway) {
    gateway->recvExactStep(16, 0);
    validateHelloOrEmpty(gateway->awaitReceived(5s), 0x1005, "0x1005");
    gateway->recvExactStep(226, 0);
    validateAuthRequest(gateway->awaitReceived(5s));
    gateway->recvExactStep(2 + 0x0e + kSynthEnvReport.size(), 0);
    validateEnvReport(gateway->awaitReceived(5s));
}

// Validates the 0x1002 frame (selected role name + graph-key input) closed by
// the 2026-11-23 static evidence (.rea/auth_1002_builders.json,
// FUN_005277e0/FUN_00528190) and the graphkey.scp 7-character input cap.
// Payload offsets are cmd-relative: +0x0e role name (GBK, 20-byte slot),
// +0x22 graph-key input (7-byte slot), 41-byte payload total. A 7-byte input
// fills its slot to the end, so the NUL-terminator and tail checks only run
// when room remains.
void validateRoleSelect(const std::vector<std::uint8_t>& wire,
                        const std::string& expectedNameGbk,
                        const std::string& expectedCode) {
    check(wire.size() == 43, "0x1002 wire size mismatch");
    const auto* p = wire.data() + 2;
    check(readLe16(p) == 0x1002, "0x1002 command mismatch");
    expectZeros(p + 2, 0x0c, "0x1002 header not zeroed");
    check(std::memcmp(p + 0x0e, expectedNameGbk.data(),
                      expectedNameGbk.size()) == 0,
          "0x1002 role name field mismatch");
    expectZeros(p + 0x0e + expectedNameGbk.size(),
                0x22 - (0x0e + expectedNameGbk.size()),
                "0x1002 role name not NUL padded");
    check(std::memcmp(p + 0x22, expectedCode.data(), expectedCode.size()) == 0,
          "0x1002 graph-key field mismatch");
    if (expectedCode.size() < kRoleCodeSlot) {
        check(p[0x22 + expectedCode.size()] == 0, "0x1002 graph key not NUL terminated");
        expectZeros(p + 0x22 + expectedCode.size() + 1,
                    kRoleCodeSlot - expectedCode.size() - 1,
                    "0x1002 tail not zeroed");
    }
}

// Decrypts the 0x4004 login frame in the fixture and validates the credential
// blob; returns the blob for RC4 derivation.
std::array<std::uint8_t, dhsh::kCredentialBlobSize> validateGameLogin(
        const std::vector<std::uint8_t>& wire, std::uint32_t expectedSession,
        std::uint32_t expectedSeq) {
    check(wire.size() == 113, "0x4004 wire size mismatch");
    std::vector<std::uint8_t> cipher(wire.begin() + 2, wire.end());
    const auto& master = dhsh::loginMasterKey();
    dhsh::BlowfishCipher blowfish(master.data(), master.size());
    blowfish.decryptInPlace(cipher.data(), cipher.size());
    check(cipher.size() == 111, "0x4004 plaintext size mismatch");
    check(readLe16(cipher.data()) == 0x4004, "0x4004 command mismatch");
    check(readLe32(cipher.data() + 0x0e) == expectedSeq,
          "0x4004 sequence field mismatch");
    std::array<std::uint8_t, dhsh::kCredentialBlobSize> blob{};
    std::memcpy(blob.data(), cipher.data() + 18, blob.size());
    check(std::memcmp(blob.data(), kSynthAccount.data(), kSynthAccount.size()) == 0,
          "login blob account mismatch");
    check(blob[kSynthAccount.size()] == 0, "login blob account not NUL terminated");
    check(std::memcmp(blob.data() + 0x33, kSynthPassword.data(), kSynthPassword.size()) == 0,
          "login blob password mismatch");
    check(readLe32(blob.data() + 0x47) == expectedSession,
          "login blob session mismatch");
    check(std::memcmp(blob.data() + 0x4b, kSynthMac.data(), kSynthMac.size()) == 0,
          "login blob MAC mismatch");
    check(blob[0x4b + kSynthMac.size()] == 0, "login blob MAC not NUL terminated");
    return blob;
}

// Decrypts one RC4 c2s game frame in the fixture and checks cmd, trailer and
// sequence; returns the plaintext.
std::vector<std::uint8_t> decryptGameFrame(
        const std::vector<std::uint8_t>& wire, dhsh::Rc4Session* rc4,
        std::uint16_t expectedCommand, std::uint32_t expectedSeq) {
    check(wire.size() >= 2 + 0x12 + 4, "c2s game frame too small");
    const std::uint16_t length = static_cast<std::uint16_t>(wire[0] | (wire[1] << 8));
    check(length == wire.size() - 2, "c2s game frame length field mismatch");
    std::vector<std::uint8_t> body(wire.begin() + 2, wire.end() - 4);
    check(dhsh::c2sTrailerMatches(body.data(), body.size(),
            reinterpret_cast<const char*>(wire.data() + wire.size() - 4)),
          "c2s game frame trailer mismatch");
    rc4->cryptInPlace(body.data(), body.size());
    check(readLe16(body.data()) == expectedCommand, "c2s game frame cmd mismatch");
    check(readLe32(body.data() + 0x0e) == expectedSeq,
          "c2s game frame sequence mismatch");
    return body;
}

// Drives poll() until an event other than none arrives; fails the test when
// the overall deadline passes first.
SessionEvent awaitEvent(GameSession* session, std::chrono::milliseconds deadline) {
    const auto start = std::chrono::steady_clock::now();
    SessionEvent event{};
    while (std::chrono::steady_clock::now() - start < deadline) {
        if (session->poll(100ms, &event)) {
            return event;
        }
    }
    throw std::runtime_error("awaitEvent: no session event before deadline");
}

// Drives poll() until the roleList event arrives and pins the accumulated
// role count; the caller validates ids/names.
SessionEvent awaitRoleListEvent(GameSession* session, std::size_t expectedCount) {
    const auto start = std::chrono::steady_clock::now();
    SessionEvent event{};
    while (std::chrono::steady_clock::now() - start < 10s) {
        if (session->poll(100ms, &event)) {
            check(event.type == SessionEvent::Type::roleList,
                  "expected a roleList event, got type " +
                      std::to_string(static_cast<int>(event.type)));
            check(event.roles.size() == expectedCount,
                  "roleList carries " + std::to_string(event.roles.size()) +
                      " roles, expected " + std::to_string(expectedCount));
            return event;
        }
    }
    throw std::runtime_error("awaitRoleListEvent: no roleList event before deadline");
}

void checkSynthRoleA(const dhsh::RoleRecord& role) {
    check(role.id == kSynthRoleIdA, "role A id mismatch");
    check(role.nameGbk == kSynthRoleNameGbk, "role A GBK name mismatch");
}

void checkSynthRoleB(const dhsh::RoleRecord& role) {
    check(role.id == kSynthRoleIdB, "role B id mismatch");
    check(role.nameGbk == kSynthRoleNameGbkB, "role B GBK name mismatch");
}

// Waits for the 0x2005 graph-key challenge and pins the decoded image: the
// event carries exactly the 96x32 RGBA the fixture encoded -- pixel for
// pixel, against the independently expanded expectation -- and never the raw
// 0x2005 payload (the event is reset before the type-specific fields fill).
SessionEvent awaitGraphKeyChallengeEvent(GameSession* session) {
    const SessionEvent event = awaitEvent(session, 10s);
    check(event.type == SessionEvent::Type::graphKeyChallenge,
          "expected the graphKeyChallenge event, got type " +
              std::to_string(static_cast<int>(event.type)));
    check(event.graphKeyImage.width == kGraphKeyImageWidth &&
              event.graphKeyImage.height == kGraphKeyImageHeight,
          "challenge image size mismatch: " +
              std::to_string(event.graphKeyImage.width) + "x" +
              std::to_string(event.graphKeyImage.height));
    const std::vector<std::uint8_t> expected = expectedGraphKeyRgba(synthGraphKeyPixels());
    check(event.graphKeyImage.rgba.size() == expected.size(),
          "challenge RGBA byte count mismatch");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (event.graphKeyImage.rgba[i] != expected[i]) {
            throw std::runtime_error("challenge RGBA mismatch at byte offset " +
                std::to_string(i) + ": expected " + std::to_string(expected[i]) +
                ", got " + std::to_string(event.graphKeyImage.rgba[i]));
        }
    }
    check(event.payload.empty(),
          "raw 0x2005 payload leaked into the challenge event");
    return event;
}

// Drives the evidenced role stage on a raw GameSession: the server sends an
// initial 0x2005 before the role records (it stays an inert gateway frame),
// then one 0x2000 with the SYNTH role set, the accumulated roleList event,
// the explicit role selection and its 0x100b follow-up, and finally the
// 0x2005 challenge that arrives only after the selection, unlocking the
// explicit graph-key submit and its 0x1002 frame. The gateway fixture stays
// connected for the caller to push the redirect.
void driveSessionRoleAndGraphKey(LoopbackServer* gateway, GameSession* session) {
    gateway->sendStep(makeGraphKeyFrame(), 0, 0);
    const SessionEvent initial = awaitEvent(session, 10s);
    check(initial.type == SessionEvent::Type::gatewayFrame &&
          initial.command == 0x2005,
          "pre-selection 0x2005 did not pass through as an inert gateway frame");

    gateway->sendStep(makeRoleListFrame(synthRoleSet()), 0, 0);
    const SessionEvent roles = awaitRoleListEvent(session, 2);
    checkSynthRoleA(roles.roles[0]);
    checkSynthRoleB(roles.roles[1]);

    check(session->selectRole(kSynthRoleIdA, 2s),
          "role selection refused: " + session->errorMessage());
    gateway->recvExactStep(16, 0);
    validateHelloOrEmpty(gateway->awaitReceived(5s), 0x100b, "0x100b");

    gateway->sendStep(makeGraphKeyFrame(), 0, 0);
    awaitGraphKeyChallengeEvent(session);
    check(session->submitGraphKey(kSynthRoleCode, 2s),
          "graph-key submit refused: " + session->errorMessage());
    gateway->recvExactStep(43, 0);
    validateRoleSelect(gateway->awaitReceived(5s), kSynthRoleNameGbk,
                       kSynthRoleCode);
}

// Full evidenced flow: gateway start batch -> 0x2000 roles -> explicit role
// selection (0x100b) -> 0x2005 challenge -> explicit graph-key submit
// (0x1002) -> 0x2002 redirect -> game login -> first s2c frame -> documented
// init sequence -> teardown.
void test_session_full_login_loopback() {
    LoopbackServer gateway;
    LoopbackServer game;

    GameSession session(synthCredentials(), 1, 30s);
    check(session.state() == SessionState::idle, "session not idle at start");

    check(session.start("127.0.0.1", gateway.port(), 2s),
          "start failed: " + session.errorMessage());
    check(session.state() == SessionState::gatewayAuthenticating,
          "session not authenticating after start");
    expectGatewayStartBatch(&gateway);
    // start() must not send anything else: no early 0x100b/0x1002.
    gateway.recvQuietStep(300);
    check(gateway.awaitReceived(5s).empty(),
          "start() sent more than the three evidenced frames");

    driveSessionRoleAndGraphKey(&gateway, &session);

    // Gateway pushes the flag frame, then redirects to the game fixture.
    gateway.sendStep(makeFrame([] {
        std::vector<std::uint8_t> p(26, 0);
        p[0] = 0x0b; p[1] = 0x20;
        p[2] = 'N'; p[3] = 'N'; p[4] = 'Y';
        return p;
    }()), 0, 0);
    gateway.sendStep(makeRedirectFrame(kSynthSession, 127, 0, 0, 1,
                                       game.port()), 0, 0);

    SessionEvent event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::gatewayFrame &&
          event.command == 0x200b, "expected 0x200b gateway frame first");
    event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::redirect, "redirect event missing");
    check(event.redirect.session == kSynthSession, "redirect session mismatch");
    check(event.redirect.host == "127.0.0.1", "redirect host mismatch");
    check(event.redirect.port == game.port(), "redirect port mismatch");
    check(session.state() == SessionState::gameAuthenticating,
          "session not waiting on game login");
    check(session.redirect().session == kSynthSession,
          "redirect accessor mismatch");

    // Game fixture: receive the Blowfish login frame, answer with a
    // table-cipher s2c frame (synthetic 0x300b-shaped payload).
    game.recvExactStep(113, 0);
    const auto blob =
        validateGameLogin(game.awaitReceived(5s), kSynthSession, /*seq=*/1);
    const auto rc4Key = dhsh::deriveRc4Key(blob);
    game.sendStep(makeS2cGameFrame(0x300b, {0x01, 0x00, 0x00, 0x00}), 0, 0);

    event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::gameFrame, "game frame event missing");
    check(event.command == 0x300b, "decrypted game frame cmd mismatch");
    check(event.payload.size() == 0x12 && event.payload[0] == 0x0b &&
          event.payload[1] == 0x30, "game frame payload mismatch");
    check(session.state() == SessionState::established, "session not established");

    // Documented init sequence: 0x4022, 0x4022, 0x40d0 x4 (sub 7/2/1/3),
    // 0x4665. Byte expectations come from the 2026-11-23 double-account
    // capture apart from the sequence field.
    struct {
        std::uint16_t cmd;
        std::vector<std::uint8_t> body;  // bytes after the cmd u16, before seq
        std::uint32_t seq;
    } expected[] = {
        {0x4022, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 3},
        {0x4022, {0, 0, 4, 0, 0, 0, 0x20, 0x03, 0, 0, 0, 0}, 5},
        {0x40d0, {0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 7},
        {0x40d0, {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 9},
        {0x40d0, {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 11},
        {0x40d0, {0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 13},
        {0x4665, {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 15},
    };
    dhsh::Rc4Session fixtureRc4(rc4Key.data(), rc4Key.size());
    check(session.sendInitSequence(2s), "sendInitSequence failed");
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        game.recvExactStep(24, 0);
        const auto wire = game.awaitReceived(5s);
        const auto plain = decryptGameFrame(wire, &fixtureRc4, expected[i].cmd,
                                            expected[i].seq);
        check(std::memcmp(plain.data() + 2, expected[i].body.data(), 12) == 0,
              "init frame body mismatch at index " + std::to_string(i));
    }

    // One caller-built frame keeps the same RC4 continuity and sequence.
    std::vector<std::uint8_t> custom(0x12, 0);
    custom[0] = 0x31; custom[1] = 0x46;
    custom[2 + 4] = 0x01;  // 0x4631 subcommand slot, static payload prefix
    game.recvExactStep(24, 0);
    check(session.sendGameFrame(custom.data(), custom.size(), 2s),
          "sendGameFrame failed");
    const auto wire = game.awaitReceived(5s);
    const auto plain = decryptGameFrame(wire, &fixtureRc4, 0x4631, 17);
    check(plain[2 + 4] == 0x01, "custom frame payload mismatch");

    session.teardown();
    check(game.stepError().empty(), "game fixture error: " + game.stepError());
}

// A session built without touching securityCode must send the documented
// "000000" default in the 0x1000 security-code slot (+0xd2..+0xd7).
void test_session_default_security_code() {
    LoopbackServer gateway;
    SessionCredentials credentials;
    credentials.account = kSynthAccount;
    credentials.password = kSynthPassword;
    credentials.zoneNameGbk = kSynthZoneGbk;
    credentials.versionToken = kSynthVersion;
    credentials.macAddress = kSynthMac;
    credentials.environmentReportGbk = kSynthEnvReport;
    // Role fields intentionally untouched: start() must not require them.
    // securityCode intentionally untouched: the struct default applies.
    GameSession session(std::move(credentials), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    gateway.recvExactStep(16, 0);
    gateway.awaitReceived(5s);  // 0x1005 hello
    gateway.recvExactStep(226, 0);
    const auto wire = gateway.awaitReceived(5s);
    check(wire.size() == 226, "0x1000 wire size mismatch");
    const auto* p = wire.data() + 2;
    check(readLe16(p) == 0x1000, "0x1000 command mismatch");
    check(std::memcmp(p + 0xd2, "000000", 6) == 0,
          "0x1000 default security code mismatch");
    check(readLe32(p + 0xdc) == 1, "0x1000 +0xdc constant mismatch");
    session.teardown();
}

// start() sends exactly the three evidenced frames and requires no role
// fields in the credentials; nothing else may leave before the server speaks.
void test_session_start_sends_only_three_frames() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s),
          "start failed: " + session.errorMessage());
    expectGatewayStartBatch(&gateway);
    gateway.recvQuietStep(300);
    check(gateway.awaitReceived(5s).empty(),
          "start() sent more than the three evidenced frames");
    check(session.state() == SessionState::gatewayAuthenticating,
          "session left the gatewayAuthenticating state on its own");
    session.teardown();
}

// One 0x2000 with two role records parses into two structured roles: u32le id
// at record+0, GBK name in the 20-byte slot at record+4 (19 bytes max).
void test_session_role_list_single_frame_two_roles() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);

    gateway.sendStep(makeRoleListFrame(synthRoleSet()), 0, 0);
    const SessionEvent event = awaitRoleListEvent(&session, 2);
    checkSynthRoleA(event.roles[0]);
    checkSynthRoleB(event.roles[1]);

    // The parse produced exactly one event; the session idles afterwards.
    SessionEvent extra{};
    check(!session.poll(200ms, &extra), "role list produced a spurious extra event");
    session.teardown();
}

// Roles accumulate across multiple 0x2000 frames: each event carries the full
// list so far (dialog data updates, per the 2026-11-24 design).
void test_session_role_list_accumulates_across_frames() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);

    gateway.sendStep(makeRoleListFrame({{kSynthRoleIdA, kSynthRoleNameGbk}}),
                     0, 0);
    SessionEvent event = awaitRoleListEvent(&session, 1);
    checkSynthRoleA(event.roles[0]);

    gateway.sendStep(makeRoleListFrame({{kSynthRoleIdB, kSynthRoleNameGbkB}}),
                     0, 0);
    event = awaitRoleListEvent(&session, 2);
    checkSynthRoleA(event.roles[0]);
    checkSynthRoleB(event.roles[1]);
    session.teardown();
}

// Malformed 0x2000 frames fail closed at protocol_error: header-only (zero
// records), a non-integral record tail, an empty role name, a name without
// the NUL terminator inside the 20-byte slot, and duplicate role ids both
// within one frame and across frames. The game fixture is never contacted.
void test_session_role_list_rejects_malformed_frames() {
    struct {
        const char* name;
        std::vector<std::uint8_t> wire;
    } cases[] = {
        {"header-only 0x2000 (zero records)", makeRoleListFrame({})},
        {"non-integral record tail", [] {
            // The rejection shape seen on failed auth: a 20-byte 0x2000 with
            // a flag byte at +2 is not a whole number of 0x85-byte records.
            std::vector<std::uint8_t> p(kRoleHeaderSize + 6, 0);
            p[0] = 0x00; p[1] = 0x20;
            p[2] = 0x01;
            return makeFrame(p);
        }()},
        {"empty role name", makeRoleListFrame({{kSynthRoleIdA, ""}})},
        {"name missing the NUL terminator", [] {
            auto wire = makeRoleListFrame({{kSynthRoleIdA, kSynthRoleNameGbk}});
            // Record starts at payload +0x0e (wire +2+0x0e); fill the whole
            // 20-byte name slot with nonzero bytes.
            const std::size_t slot = 2 + kRoleHeaderSize + 4;
            std::fill(wire.begin() + slot, wire.begin() + slot + kRoleNameSlot,
                      'N');
            return wire;
        }()},
        {"duplicate role id in one frame",
         makeRoleListFrame({{kSynthRoleIdA, kSynthRoleNameGbk},
                            {kSynthRoleIdA, kSynthRoleNameGbkB}})},
    };
    for (const auto& c : cases) {
        LoopbackServer gateway;
        LoopbackServer game;
        GameSession session(synthCredentials(), 1, 30s);
        check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
        expectGatewayStartBatch(&gateway);
        gateway.sendStep(c.wire, 0, 0);

        SessionEvent event = awaitEvent(&session, 10s);
        check(event.type == SessionEvent::Type::error,
              std::string(c.name) + ": expected error event");
        check(event.error == SessionError::protocol_error,
              std::string(c.name) + ": expected protocol_error");
        check(session.state() == SessionState::failed,
              std::string(c.name) + ": session not failed");
        check(game.waitAccepting(300ms),
              std::string(c.name) + ": game fixture was contacted");
    }
    // A duplicate id arriving in a later 0x2000 also fails closed.
    {
        LoopbackServer gateway;
        LoopbackServer game;
        GameSession session(synthCredentials(), 1, 30s);
        check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
        expectGatewayStartBatch(&gateway);
        gateway.sendStep(makeRoleListFrame({{kSynthRoleIdA, kSynthRoleNameGbk}}),
                         0, 0);
        awaitRoleListEvent(&session, 1);
        gateway.sendStep(makeRoleListFrame({{kSynthRoleIdA, kSynthRoleNameGbkB}}),
                         0, 0);
        SessionEvent event = awaitEvent(&session, 10s);
        check(event.type == SessionEvent::Type::error &&
              event.error == SessionError::protocol_error,
              "duplicate id across frames: expected protocol_error");
        check(session.state() == SessionState::failed, "session not failed");
        check(game.waitAccepting(300ms),
              "game fixture was contacted after a duplicate role id");
    }
}

// The 0x100b follow-up leaves only after an explicit role selection: nothing
// is auto-sent (no first-role default), unknown ids and duplicate submissions
// fail closed without touching the wire, and the session stays alive.
void test_session_select_role_sends_100b_after_explicit_choice() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);

    gateway.sendStep(makeRoleListFrame(synthRoleSet()), 0, 0);
    const SessionEvent event = awaitRoleListEvent(&session, 2);
    checkSynthRoleA(event.roles[0]);
    checkSynthRoleB(event.roles[1]);

    // No auto-selection: the wire stays silent until the explicit choice.
    gateway.recvQuietStep(300);
    check(gateway.awaitReceived(5s).empty(),
          "session auto-sent a role-stage frame before the selection");

    check(!session.selectRole(0x0000deadU, 100ms),
          "unknown role id accepted");
    check(session.state() == SessionState::gatewayAuthenticating,
          "unknown-id rejection changed the session state");
    check(!session.errorMessage().empty(), "unknown-id rejection lost its diagnostic");
    gateway.recvQuietStep(300);
    check(gateway.awaitReceived(5s).empty(),
          "unknown-id selection touched the wire");

    check(session.selectRole(kSynthRoleIdB, 2s),
          "valid role selection refused: " + session.errorMessage());
    gateway.recvExactStep(16, 0);
    validateHelloOrEmpty(gateway.awaitReceived(5s), 0x100b, "0x100b");

    check(!session.selectRole(kSynthRoleIdA, 100ms),
          "duplicate role selection accepted");
    gateway.recvQuietStep(300);
    check(gateway.awaitReceived(5s).empty(),
          "duplicate selection sent a second 0x100b");
    session.teardown();
}

// The 0x1002 frame is gated on the 0x2005 challenge: submits fail before a
// role selection and before the challenge, succeed only after both, and a
// repeat submit fails closed. The frame carries the selected role name plus
// the submitted graph-key bytes.
void test_session_graph_key_gates_1002_until_2005() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);
    gateway.sendStep(makeRoleListFrame(synthRoleSet()), 0, 0);
    awaitRoleListEvent(&session, 2);

    check(!session.submitGraphKey(kSynthRoleCode, 100ms),
          "graph key accepted before a role selection");
    check(session.state() == SessionState::gatewayAuthenticating,
          "pre-selection submit changed the session state");

    check(session.selectRole(kSynthRoleIdA, 2s),
          "role selection refused: " + session.errorMessage());
    gateway.recvExactStep(16, 0);
    validateHelloOrEmpty(gateway.awaitReceived(5s), 0x100b, "0x100b");

    check(!session.submitGraphKey(kSynthRoleCode, 100ms),
          "graph key accepted before the 0x2005 challenge");
    gateway.recvQuietStep(300);
    check(gateway.awaitReceived(5s).empty(),
          "pre-challenge submit touched the wire");

    gateway.sendStep(makeGraphKeyFrame(), 0, 0);
    awaitGraphKeyChallengeEvent(&session);

    check(session.submitGraphKey(kSynthRoleCode, 2s),
          "graph-key submit refused: " + session.errorMessage());
    gateway.recvExactStep(43, 0);
    validateRoleSelect(gateway.awaitReceived(5s), kSynthRoleNameGbk,
                       kSynthRoleCode);

    check(!session.submitGraphKey("SYN02", 100ms),
          "duplicate graph-key submit accepted");
    gateway.recvQuietStep(300);
    check(gateway.awaitReceived(5s).empty(),
          "duplicate submit sent a second 0x1002");
    session.teardown();
}

// The server sends an initial 0x2005 BEFORE the role records/selection. That
// frame must stay inert: an ordinary gateway frame, no challenge event, and
// no unlock. Only a 0x2005 that arrives AFTER the explicit role selection
// emits the graphKeyChallenge event and unlocks the 0x1002 submit; a repeat
// 0x2005 afterwards stays inert too.
void test_session_early_2005_does_not_unlock_challenge() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);

    // Initial 0x2005 before any role records: passes through as a gateway
    // frame -- never a challenge -- and must not unlock the submit.
    gateway.sendStep(makeGraphKeyFrame(), 0, 0);
    SessionEvent event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::gatewayFrame &&
          event.command == 0x2005,
          "pre-selection 0x2005 did not pass through as an inert gateway frame");

    gateway.sendStep(makeRoleListFrame(synthRoleSet()), 0, 0);
    awaitRoleListEvent(&session, 2);

    check(!session.submitGraphKey(kSynthRoleCode, 100ms),
          "graph key accepted before a role selection");

    check(session.selectRole(kSynthRoleIdA, 2s),
          "role selection refused: " + session.errorMessage());
    gateway.recvExactStep(16, 0);
    validateHelloOrEmpty(gateway.awaitReceived(5s), 0x100b, "0x100b");

    // The early 0x2005 must not have unlocked anything: the submit is still
    // refused and nothing hits the wire.
    check(!session.submitGraphKey(kSynthRoleCode, 100ms),
          "pre-selection 0x2005 unlocked the graph-key submit");
    gateway.recvQuietStep(300);
    check(gateway.awaitReceived(5s).empty(),
          "locked submit after the early 0x2005 touched the wire");

    // A NEW 0x2005 after the selection is the real challenge: exactly one
    // graphKeyChallenge event, and the submit then sends the 0x1002.
    gateway.sendStep(makeGraphKeyFrame(), 0, 0);
    awaitGraphKeyChallengeEvent(&session);

    // A repeat 0x2005 stays inert: no second challenge, still a plain
    // gateway frame.
    gateway.sendStep(makeGraphKeyFrame(), 0, 0);
    event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::gatewayFrame &&
          event.command == 0x2005,
          "repeat 0x2005 did not stay an inert gateway frame");

    check(session.submitGraphKey(kSynthRoleCode, 2s),
          "graph-key submit refused after the challenge: " +
              session.errorMessage());
    gateway.recvExactStep(43, 0);
    validateRoleSelect(gateway.awaitReceived(5s), kSynthRoleNameGbk,
                       kSynthRoleCode);
    session.teardown();
}

// Graph-key input validation: every rejected input fails closed without
// touching the wire or changing the state, and a follow-up valid submit still
// works (the received 0x1002 is the proof).
void test_session_rejects_invalid_graph_key() {
    struct {
        const char* name;
        std::string code;
    } cases[] = {
        {"empty graph key", ""},
        {"embedded NUL", std::string("SYN\0", 4)},
        {"over the 7-byte slot", std::string(8, 'g')},
        {"non-ASCII bytes", "\xc4\xe3\xba\xc3"},
        {"space in graph key", "S Y N"},
    };
    for (const auto& c : cases) {
        LoopbackServer gateway;
        GameSession session(synthCredentials(), 1, 30s);
        check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
        expectGatewayStartBatch(&gateway);
        gateway.sendStep(makeRoleListFrame(synthRoleSet()), 0, 0);
        awaitRoleListEvent(&session, 2);
        check(session.selectRole(kSynthRoleIdA, 2s),
              "role selection refused: " + session.errorMessage());
        gateway.recvExactStep(16, 0);
        validateHelloOrEmpty(gateway.awaitReceived(5s), 0x100b, "0x100b");
        gateway.sendStep(makeGraphKeyFrame(), 0, 0);
        awaitGraphKeyChallengeEvent(&session);

        check(!session.submitGraphKey(c.code, 100ms),
              std::string(c.name) + ": submit accepted");
        check(session.state() == SessionState::gatewayAuthenticating,
              std::string(c.name) + ": rejection changed the session state");
        check(!session.errorMessage().empty(),
              std::string(c.name) + ": rejection lost its diagnostic");
        gateway.recvQuietStep(300);
        check(gateway.awaitReceived(5s).empty(),
              std::string(c.name) + ": rejection touched the wire");

        check(session.submitGraphKey(kSynthRoleCode, 2s),
              std::string(c.name) + ": valid submit after the rejection failed");
        gateway.recvExactStep(43, 0);
        validateRoleSelect(gateway.awaitReceived(5s), kSynthRoleNameGbk,
                           kSynthRoleCode);
        session.teardown();
    }
}

// Slot boundary acceptance: a role name of exactly 19 GBK bytes (the most the
// 20-byte record slot holds with its NUL) and a graph key of exactly 7 bytes
// fill their 0x1002 slots completely and must go out on the wire (the 7-byte
// code leaves no room for a NUL inside the payload).
void test_session_1002_slot_boundary() {
    LoopbackServer gateway;
    const std::string name19(19, 'n');
    const std::string code7(7, 'c');
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s),
          "start failed at the slot boundary");
    expectGatewayStartBatch(&gateway);

    gateway.sendStep(makeRoleListFrame({{kSynthRoleIdA, name19}}), 0, 0);
    const SessionEvent event = awaitRoleListEvent(&session, 1);
    check(event.roles[0].nameGbk == name19,
          "19-byte role name did not survive the record parse");

    check(session.selectRole(kSynthRoleIdA, 2s),
          "role selection refused: " + session.errorMessage());
    gateway.recvExactStep(16, 0);
    validateHelloOrEmpty(gateway.awaitReceived(5s), 0x100b, "0x100b");
    gateway.sendStep(makeGraphKeyFrame(), 0, 0);
    awaitGraphKeyChallengeEvent(&session);
    check(session.submitGraphKey(code7, 2s),
          "7-byte graph key refused: " + session.errorMessage());
    gateway.recvExactStep(43, 0);
    validateRoleSelect(gateway.awaitReceived(5s), name19, code7);
    session.teardown();
}

// The post-selection 0x2005 decodes to the synthetic 96x32 image pixel for
// pixel before the challenge event leaves the session (full byte comparison
// in awaitGraphKeyChallengeEvent; corner pixels pinned here on top), the
// event carries the image only -- never the raw payload -- and the submit
// then sends the evidenced 41-byte 0x1002.
void test_session_graph_key_challenge_decodes_image() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);
    gateway.sendStep(makeRoleListFrame(synthRoleSet()), 0, 0);
    awaitRoleListEvent(&session, 2);

    check(session.selectRole(kSynthRoleIdA, 2s),
          "role selection refused: " + session.errorMessage());
    gateway.recvExactStep(16, 0);
    validateHelloOrEmpty(gateway.awaitReceived(5s), 0x100b, "0x100b");

    gateway.sendStep(makeGraphKeyFrame(), 0, 0);
    const SessionEvent challenge = awaitGraphKeyChallengeEvent(&session);
    const auto& rgba = challenge.graphKeyImage.rgba;
    // RGB565 0xf800 -> (255, 0, 0, 255) at (0,0); 0x07e0 -> (0, 255, 0, 255)
    // at (95,0); the helper already compared all 96*32*4 bytes.
    check(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255,
          "challenge pixel (0,0) mismatch");
    const std::size_t topRight =
        (static_cast<std::size_t>(kGraphKeyImageWidth) - 1) * 4;
    check(rgba[topRight] == 0 && rgba[topRight + 1] == 255 &&
              rgba[topRight + 2] == 0 && rgba[topRight + 3] == 255,
          "challenge pixel (95,0) mismatch");

    check(session.submitGraphKey(kSynthRoleCode, 2s),
          "graph-key submit refused: " + session.errorMessage());
    gateway.recvExactStep(43, 0);
    validateRoleSelect(gateway.awaitReceived(5s), kSynthRoleNameGbk,
                       kSynthRoleCode);
    session.teardown();
}

// A post-selection 0x2005 whose image does not decode (no dominant background)
// fails closed: protocol_error with the decoder's fixed, sanitized diagnostic
// -- never payload bytes -- the session is terminal, the submit stays locked,
// and nothing reaches the wire. The game fixture is never contacted.
void test_session_invalid_graph_key_challenge_fails_closed() {
    LoopbackServer gateway;
    LoopbackServer game;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);
    gateway.sendStep(makeRoleListFrame(synthRoleSet()), 0, 0);
    awaitRoleListEvent(&session, 2);

    check(session.selectRole(kSynthRoleIdA, 2s),
          "role selection refused: " + session.errorMessage());
    gateway.recvExactStep(16, 0);
    validateHelloOrEmpty(gateway.awaitReceived(5s), 0x100b, "0x100b");

    gateway.sendStep(makeInvalidGraphKeyFrame(), 0, 0);
    SessionEvent event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::error, "expected error event");
    check(event.error == SessionError::protocol_error,
          "expected protocol_error, got " +
              std::to_string(static_cast<int>(event.error)));
    check(event.message.find("graph-key background candidate is too weak") !=
              std::string::npos,
          "diagnostic lost the fixed decode message: " + event.message);
    check(event.message.find(kSynthAccount) == std::string::npos &&
              event.message.find(kSynthPassword) == std::string::npos,
          "diagnostic leaked credential material");
    check(session.state() == SessionState::failed, "session not failed");
    check(session.error() == SessionError::protocol_error,
          "error accessor mismatch");

    // The submit stays locked: the terminal session refuses it and no 0x1002
    // leaves; the game fixture is never contacted.
    check(!session.submitGraphKey(kSynthRoleCode, 100ms),
          "submit accepted after a rejected challenge");
    gateway.recvQuietStep(300);
    check(gateway.awaitReceived(5s).empty(),
          "submit after a rejected challenge touched the wire");
    check(game.waitAccepting(300ms),
          "game fixture was contacted after a rejected challenge");
}

// Wrong auth: the gateway answers with an error-shaped 0x2000 (a 20-byte
// payload -- not a whole number of role records) and closes. The session must
// fail closed at protocol_error without ever touching the game fixture.
void test_session_gateway_auth_rejected_stops() {
    LoopbackServer gateway;
    LoopbackServer game;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);
    gateway.sendStep(makeFrame([] {
        std::vector<std::uint8_t> p(20, 0);
        p[0] = 0x00; p[1] = 0x20;
        p[2] = 0x01;
        return p;
    }()), 0, 0);
    gateway.shutdownStep();

    SessionEvent event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::error, "expected error event");
    check(event.error == SessionError::protocol_error, "expected protocol_error");
    check(session.state() == SessionState::failed, "session not failed");
    check(session.error() == SessionError::protocol_error, "error accessor mismatch");
    check(game.waitAccepting(300ms), "game fixture was contacted after rejection");
}

// A gateway command outside the observed handshake set fails closed.
void test_session_unknown_gateway_command_stops() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);
    gateway.sendStep(makeFrame([] {
        std::vector<std::uint8_t> p(14, 0);
        p[0] = 0xff; p[1] = 0x2f;  // 0x2fff: never observed on the wire
        return p;
    }()), 0, 0);

    SessionEvent event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::error, "expected error event");
    check(event.error == SessionError::unknown_command, "expected unknown_command");
    check(session.state() == SessionState::failed, "session not failed");
}

// Redirect validation: declared length must be 24, the address must not be
// 0.0.0.0 and the port must be nonzero.
void test_session_illegal_redirect_stops() {
    struct {
        const char* name;
        std::vector<std::uint8_t> wire;
    } cases[] = {
        {"length", [] {
            auto w = makeRedirectFrame(kSynthSession, 127, 0, 0, 1, 8000);
            w[0] = 23;  // declared payload length 23, cmd still 0x2002
            w.resize(2 + 23);
            return w;
        }()},
        {"zero ip", makeRedirectFrame(kSynthSession, 0, 0, 0, 0, 8000)},
        {"zero port", makeRedirectFrame(kSynthSession, 127, 0, 0, 1, 0)},
    };
    for (const auto& c : cases) {
        LoopbackServer gateway;
        LoopbackServer game;
        GameSession session(synthCredentials(), 1, 30s);
        check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
        expectGatewayStartBatch(&gateway);
        gateway.sendStep(c.wire, 0, 0);
        SessionEvent event = awaitEvent(&session, 10s);
        check(event.type == SessionEvent::Type::error,
              std::string(c.name) + ": expected error event");
        check(event.error == SessionError::illegal_redirect,
              std::string(c.name) + ": expected illegal_redirect");
        check(session.state() == SessionState::failed,
              std::string(c.name) + ": session not failed");
        check(game.waitAccepting(200ms),
              std::string(c.name) + ": game fixture was contacted");
    }
}

// A gateway that stays silent must hit the authentication deadline.
void test_session_gateway_timeout_stops() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), 1, 500ms);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    const auto start = std::chrono::steady_clock::now();
    SessionEvent event{};
    bool gotEvent = false;
    while (std::chrono::steady_clock::now() - start < 10s) {
        if (session.poll(100ms, &event)) {
            gotEvent = true;
            break;
        }
    }
    check(gotEvent, "silent gateway produced no deadline event");
    check(event.type == SessionEvent::Type::error, "expected error event");
    check(event.error == SessionError::timeout, "expected timeout");
    check(session.state() == SessionState::failed, "session not failed");
}

// A gateway that sends only a frame header and then stalls must fail the
// session on the first poll: the transport consumes the header bytes, hits
// the per-poll deadline, and closes the stream. The session must surface the
// terminal timeout (keeping the original "timed out" diagnostic) instead of
// reporting none and dying with "readFrame on a closed stream" on the next
// poll.
void test_session_gateway_partial_frame_timeout_stops() {
    LoopbackServer gateway;
    LoopbackServer game;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);
    // Complete 14B-payload length header only, then silence.
    gateway.sendStep({0x0e, 0x00}, 0, 0);

    SessionEvent event{};
    check(session.poll(200ms, &event),
          "first poll did not produce an event for a stalled gateway frame");
    check(event.type == SessionEvent::Type::error, "expected error event");
    check(event.error == SessionError::disconnected,
          "expected disconnected for a mid-frame gateway timeout");
    check(session.state() == SessionState::failed, "session not failed");
    check(session.error() == SessionError::disconnected, "error accessor mismatch");
    check(event.message.find("timed out") != std::string::npos,
          "diagnostic lost the original timeout: " + event.message);
    check(event.message.find("readFrame on a closed stream") == std::string::npos,
          "diagnostic masked the timeout as a closed-stream error: " + event.message);
    check(game.waitAccepting(300ms),
          "game fixture was contacted after a gateway stall");
}

// A gateway that closes mid-handshake fails the session.
void test_session_gateway_disconnect_stops() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);
    gateway.sendStep(makeFrame([] {
        std::vector<std::uint8_t> p(14, 0);
        p[0] = 0x0c; p[1] = 0x20;
        return p;
    }()), 0, 0);
    gateway.shutdownStep();

    SessionEvent event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::gatewayFrame &&
          event.command == 0x200c, "expected 0x200c gateway frame");
    event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::error, "expected error event");
    check(event.error == SessionError::disconnected, "expected disconnected");
    check(session.state() == SessionState::failed, "session not failed");
}

// Gateway desync diagnosis (2026-11-24 online run: the transport died with
// "frame length 8195 exceeds the limit 8192", a length header landing on
// non-frame-aligned bytes after several good frames). The protocol_error
// diagnostic must carry desync metadata only -- accepted frame count,
// cumulative wire bytes (payload + 2), and the last accepted command as a hex
// id -- never payload bytes, credentials, or addresses.
void test_session_gateway_desync_diagnostic_is_sanitized() {
    LoopbackServer gateway;
    LoopbackServer game;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);
    // One valid 0x200c ack: 14B payload -> 16 wire bytes. The payload is a
    // recognizable marker so the assertions below prove no payload byte (nor
    // any reconstruction of one) reaches the diagnostic.
    gateway.sendStep(makeFrame([] {
        std::vector<std::uint8_t> p(14, 0);
        p[0] = 0x0c; p[1] = 0x20;
        std::fill(p.begin() + 2, p.end(), 'P');
        return p;
    }()), 0, 0);
    // Then a length header of 0xffff -- far above the 8192 payload bound --
    // followed by more marker bytes.
    gateway.sendStep({0xff, 0xff, 'P', 'P', 'P', 'P', 'P', 'P', 'P', 'P'}, 0, 0);

    SessionEvent event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::gatewayFrame &&
          event.command == 0x200c, "expected 0x200c gateway frame first");
    event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::error, "expected error event");
    check(event.error == SessionError::protocol_error, "expected protocol_error");
    check(session.error() == SessionError::protocol_error, "error accessor mismatch");
    check(session.state() == SessionState::failed, "session not failed");
    check(event.message.find("accepted_frames=1") != std::string::npos,
          "diagnostic lost accepted_frames: " + event.message);
    check(event.message.find("accepted_wire_bytes=16") != std::string::npos,
          "diagnostic lost accepted_wire_bytes: " + event.message);
    check(event.message.find("last_command=0x200c") != std::string::npos,
          "diagnostic lost last_command: " + event.message);
    check(event.message.find("PPPP") == std::string::npos,
          "diagnostic leaked payload bytes: " + event.message);
    check(event.message.find(kSynthAccount) == std::string::npos &&
          event.message.find(kSynthPassword) == std::string::npos,
          "diagnostic leaked credential material");
    check(game.waitAccepting(300ms),
          "game fixture was contacted after a gateway desync");
}

// A game server that drops the connection before the first s2c frame fails
// the login; no fallback.
void test_session_game_disconnect_stops() {
    LoopbackServer gateway;
    LoopbackServer game;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);
    gateway.sendStep(makeRedirectFrame(kSynthSession, 127, 0, 0, 1,
                                       game.port()), 0, 0);
    SessionEvent event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::redirect, "redirect event missing");
    check(session.state() == SessionState::gameAuthenticating,
          "session not waiting on game login");

    game.recvExactStep(113, 0);
    const auto wire = game.awaitReceived(5s);
    const auto blob =
        validateGameLogin(wire, kSynthSession, /*seq=*/1);
    (void)blob;
    game.shutdownStep();

    event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::error, "expected error event");
    check(event.error == SessionError::disconnected, "expected disconnected");
    check(session.state() == SessionState::failed, "session not failed");
}

// The game side mirrors the gateway: after the 0x4004 login frame, a server
// that sends only a frame header and then stalls must fail the session on the
// first poll with the original "timed out" diagnostic, not a none event
// followed by "readFrame on a closed stream".
void test_session_game_partial_frame_timeout_stops() {
    LoopbackServer gateway;
    LoopbackServer game;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    expectGatewayStartBatch(&gateway);
    gateway.sendStep(makeRedirectFrame(kSynthSession, 127, 0, 0, 1,
                                       game.port()), 0, 0);
    SessionEvent event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::redirect, "redirect event missing");
    check(session.state() == SessionState::gameAuthenticating,
          "session not waiting on game login");

    game.recvExactStep(113, 0);
    game.awaitReceived(5s);  // 0x4004 login frame
    // Complete 14B-payload length header only, then silence.
    game.sendStep({0x0e, 0x00}, 0, 0);

    check(session.poll(200ms, &event),
          "first poll did not produce an event for a stalled game frame");
    check(event.type == SessionEvent::Type::error, "expected error event");
    check(event.error == SessionError::disconnected,
          "expected disconnected for a mid-frame game timeout");
    check(session.state() == SessionState::failed, "session not failed");
    check(session.error() == SessionError::disconnected, "error accessor mismatch");
    check(event.message.find("timed out") != std::string::npos,
          "diagnostic lost the original timeout: " + event.message);
    check(event.message.find("readFrame on a closed stream") == std::string::npos,
          "diagnostic masked the timeout as a closed-stream error: " + event.message);
}

// API misuse fails closed instead of guessing: double start and init
// sequence before establishment are rejected.
void test_session_bad_usage_stops() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    check(!session.sendInitSequence(100ms), "init before established accepted");
    check(session.state() == SessionState::gatewayAuthenticating,
          "bad usage changed session state");
    check(!session.start("127.0.0.1", gateway.port(), 100ms),
          "double start accepted");
    check(session.state() == SessionState::failed, "double start not failed");
    check(session.error() == SessionError::bad_usage, "expected bad_usage");
}

// ---------------------------------------------------------------------------
// GameSession::sendChat tests (0x4050). Layout closed by the FUN_007a3400 ->
// FUN_007a2a00 static evidence (.rea/chat_send_packet_builder.json): after the
// rearrangement the plaintext payload starting at the cmd is
//   +0x00 u16le 0x4050
//   +0x02 u32le rand2      (first  _rand() draw, 15-bit non-negative)
//   +0x06 u32le channel
//   +0x0a u32le rand1      (second _rand() draw, 15-bit non-negative)
//   +0x0e u32le sequence   (written by sendGameFrame, +2 per frame)
//   +0x12 GBK text bytes + NUL
// and the wire length field = GBK byte count + 0x13 (empty text rejected by
// FUN_007a3400 itself). RC4 continuity, MD5 trailer and sequence advancement
// all stay inside sendGameFrame; the fixture decryptor pins them on the wire.

// Drives the gateway + game fixtures through the full evidenced login flow up
// to the established state and returns the fixture-side RC4 session used to
// decrypt subsequent c2s frames. Mirrors test_session_full_login_loopback.
dhsh::Rc4Session establishEncryptedSession(LoopbackServer* gateway,
                                           LoopbackServer* game,
                                           GameSession* session) {
    check(session->start("127.0.0.1", gateway->port(), 2s),
          "start failed: " + session->errorMessage());
    expectGatewayStartBatch(gateway);
    driveSessionRoleAndGraphKey(gateway, session);
    gateway->sendStep(makeFrame([] {
        std::vector<std::uint8_t> p(26, 0);
        p[0] = 0x0b; p[1] = 0x20;
        p[2] = 'N'; p[3] = 'N'; p[4] = 'Y';
        return p;
    }()), 0, 0);
    gateway->sendStep(makeRedirectFrame(kSynthSession, 127, 0, 0, 1,
                                        game->port()), 0, 0);

    SessionEvent event = awaitEvent(session, 10s);
    check(event.type == SessionEvent::Type::gatewayFrame &&
          event.command == 0x200b, "expected 0x200b gateway frame first");
    event = awaitEvent(session, 10s);
    check(event.type == SessionEvent::Type::redirect, "redirect event missing");

    game->recvExactStep(113, 0);
    const auto blob = validateGameLogin(game->awaitReceived(5s),
                                        kSynthSession, /*seq=*/1);
    const auto rc4Key = dhsh::deriveRc4Key(blob);
    game->sendStep(makeS2cGameFrame(0x300b, {0x01, 0x00, 0x00, 0x00}), 0, 0);
    event = awaitEvent(session, 10s);
    check(event.type == SessionEvent::Type::gameFrame && event.command == 0x300b,
          "game frame event missing");
    check(session->state() == SessionState::established, "session not established");
    return dhsh::Rc4Session(rc4Key.data(), rc4Key.size());
}

// Validates one decrypted 0x4050 payload (cmd already checked by
// decryptGameFrame) against the evidenced layout. The rand fields are only
// pinned to the 15-bit _rand() range: the public API deliberately keeps the
// random source private, so two successive frames must both decrypt and stay
// in range rather than matching fixed constants.
void validateChatPayload(const std::vector<std::uint8_t>& plain,
                         std::uint32_t channel, const std::string& expectedGbk,
                         std::uint32_t expectedSeq) {
    check(plain.size() == expectedGbk.size() + 0x13,
          "chat payload size != GBK bytes + 0x13");
    const std::uint32_t rand2 = readLe32(plain.data() + 2);
    const std::uint32_t rand1 = readLe32(plain.data() + 0x0a);
    check(rand2 <= 0x7fff, "rand2 outside the 15-bit _rand() range");
    check(rand1 <= 0x7fff, "rand1 outside the 15-bit _rand() range");
    check(readLe32(plain.data() + 6) == channel, "chat channel mismatch");
    check(readLe32(plain.data() + 0x0e) == expectedSeq,
          "chat sequence field mismatch");
    check(std::memcmp(plain.data() + 0x12, expectedGbk.data(),
                      expectedGbk.size()) == 0, "chat GBK text mismatch");
    check(plain[0x12 + expectedGbk.size()] == 0, "chat text not NUL terminated");
    for (std::size_t i = 0x12; i + 1 < plain.size(); ++i) {
        check(plain[i] != 0, "chat text carries an interior NUL");
    }
}

// Full wire check: GBK known answer for the Chinese text above (C4E3 BAC3
// A3AC CAC0 BDE7), both rand fields in range, channel field, sequence
// continuity across two frames (+2 each, seeded 1 -> login frame 1 -> chat
// frames 3 and 5), wire length field, and the MD5 trailer (checked while
// decrypting).
void test_session_send_chat_wire_format_and_seq() {
    LoopbackServer gateway;
    LoopbackServer game;
    GameSession session(synthCredentials(), 1, 30s);
    auto fixtureRc4 = establishEncryptedSession(&gateway, &game, &session);
    // Chinese text "ni hao, shi jie" in explicit UTF-8 bytes (E4BD A0E5 A5BD
    // EFBC 8CE4 B896 E795 8C); the GBK expectation is the known answer C4E3
    // BAC3 A3AC CAC0 BDE7 pinned by the loopback decrypt.
    const std::string utf8Text = "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c";
    const std::string expectedGbk = "\xc4\xe3\xba\xc3\xa3\xac\xca\xc0\xbd\xe7";

    game.recvExactStep(2 + (expectedGbk.size() + 0x13) + 4, 0);
    check(session.sendChat(5, utf8Text, 2s), "sendChat failed");
    const auto wire = game.awaitReceived(5s);
    const auto plain = decryptGameFrame(wire, &fixtureRc4, 0x4050, /*seq=*/3);
    check(wire.size() - 2 == expectedGbk.size() + 0x13 + 4,
          "chat wire length field != GBK bytes + 0x13 + 4 (trailer)");
    validateChatPayload(plain, /*channel=*/5, expectedGbk, /*seq=*/3);

    // Second frame keeps RC4 continuity and advances the sequence by 2.
    game.recvExactStep(2 + (5 + 0x13) + 4, 0);
    check(session.sendChat(9, "hello", 2s), "second sendChat failed");
    const auto wire2 = game.awaitReceived(5s);
    const auto plain2 = decryptGameFrame(wire2, &fixtureRc4, 0x4050, /*seq=*/5);
    validateChatPayload(plain2, /*channel=*/9, "hello", /*seq=*/5);

    session.teardown();
    check(game.stepError().empty(), "game fixture error: " + game.stepError());
}

// Input validation: every rejected call must leave the session established
// and write nothing to the wire; the follow-up valid frame is the proof --
// the fixture reads exactly one frame and it must be the valid one.
void test_session_send_chat_rejects_invalid_input() {
    struct {
        const char* name;
        std::string text;
    } cases[] = {
        {"empty", ""},
        {"invalid utf8 0xff", "\xff"},
        {"invalid utf8 lone continuation", "\x80"},
        {"invalid utf8 truncated", "\xe4\xbd"},
        {"invalid utf8 overlong", "\xc0\xaf"},
        {"embedded nul", std::string("ab\0cd", 5)},
        {"unconvertible", "emoji \xf0\x9f\x98\x80 face"},
    };
    for (const auto& c : cases) {
        LoopbackServer gateway;
        LoopbackServer game;
        GameSession session(synthCredentials(), 1, 30s);
        auto fixtureRc4 = establishEncryptedSession(&gateway, &game, &session);

        check(!session.sendChat(5, c.text, 2s),
              std::string(c.name) + ": sendChat accepted invalid input");
        check(session.state() == SessionState::established,
              std::string(c.name) + ": rejection changed the session state");
        check(!session.errorMessage().empty(),
              std::string(c.name) + ": rejection lost its diagnostic");

        // Nothing hit the wire: the next (valid) frame arrives byte-exact.
        game.recvExactStep(2 + (4 + 0x13) + 4, 0);
        check(session.sendChat(5, "okya", 2s),
              std::string(c.name) + ": valid sendChat after rejection failed");
        const auto wire = game.awaitReceived(5s);
        const auto plain = decryptGameFrame(wire, &fixtureRc4, 0x4050, /*seq=*/3);
        validateChatPayload(plain, /*channel=*/5, "okya", /*seq=*/3);
        session.teardown();
        check(game.stepError().empty(), "game fixture error: " + game.stepError());
    }
}

// Replication-side defensive byte boundary (not an evidenced original limit):
// 4000 GBK bytes still go out on the wire, 4001 are refused.
void test_session_send_chat_size_boundary() {
    LoopbackServer gateway;
    LoopbackServer game;
    GameSession session(synthCredentials(), 1, 30s);
    auto fixtureRc4 = establishEncryptedSession(&gateway, &game, &session);

    const std::string atLimit(4000, 'a');
    game.recvExactStep(2 + (4000 + 0x13) + 4, 0);
    check(session.sendChat(5, atLimit, 2s), "boundary-size sendChat failed");
    const auto wire = game.awaitReceived(5s);
    const auto plain = decryptGameFrame(wire, &fixtureRc4, 0x4050, /*seq=*/3);
    validateChatPayload(plain, /*channel=*/5, atLimit, /*seq=*/3);

    const std::string overLimit(4001, 'a');
    check(!session.sendChat(5, overLimit, 2s),
          "sendChat accepted a payload over the defensive boundary");
    check(session.state() == SessionState::established,
          "oversized rejection changed the session state");

    game.recvExactStep(2 + (4 + 0x13) + 4, 0);
    check(session.sendChat(5, "okya", 2s), "valid sendChat after oversized failed");
    const auto wire2 = game.awaitReceived(5s);
    const auto plain2 = decryptGameFrame(wire2, &fixtureRc4, 0x4050, /*seq=*/5);
    validateChatPayload(plain2, /*channel=*/5, "okya", /*seq=*/5);
    session.teardown();
    check(game.stepError().empty(), "game fixture error: " + game.stepError());
}

// CJK boundary: 2000 x the U+4F60 character used below (UTF-8 3 bytes, GBK
// 2 bytes, both known answers) is exactly 4000 GBK bytes and must go out;
// 2001 x it is 4002 GBK bytes and must be refused. A UTF-8-side input guard
// alone cannot express this:
// the 2000-character message is 6000 UTF-8 bytes while staying inside the
// 4000-byte GBK bound.
void test_session_send_chat_cjk_boundary() {
    std::string text2000;
    std::string expectedGbk;
    text2000.reserve(6000);
    expectedGbk.reserve(4000);
    for (int i = 0; i < 2000; ++i) {
        text2000 += "\xe4\xbd\xa0";  // U+4F60 in UTF-8
        expectedGbk += "\xc4\xe3";   // its two GBK bytes
    }
    std::string text2001 = text2000 + "\xe4\xbd\xa0";

    LoopbackServer gateway;
    LoopbackServer game;
    GameSession session(synthCredentials(), 1, 30s);
    auto fixtureRc4 = establishEncryptedSession(&gateway, &game, &session);

    game.recvExactStep(2 + (4000 + 0x13) + 4, 0);
    check(session.sendChat(5, text2000, 2s),
          "sendChat rejected 2000 CJK characters (exactly 4000 GBK bytes)");
    const auto wire = game.awaitReceived(5s);
    const auto plain = decryptGameFrame(wire, &fixtureRc4, 0x4050, /*seq=*/3);
    check(plain.size() == 4000 + 0x13, "2000-CJK chat payload size mismatch");
    check(plain[0x12 + 4000] == 0, "2000-CJK chat text not NUL terminated");
    check(std::memcmp(plain.data() + 0x12, expectedGbk.data(), 4000) == 0,
          "2000-CJK chat GBK text mismatch");

    check(!session.sendChat(5, text2001, 2s),
          "sendChat accepted 2001 CJK characters (4002 GBK bytes)");
    check(session.state() == SessionState::established,
          "CJK oversized rejection changed the session state");

    // Nothing hit the wire for the rejected 2001-character message.
    game.recvExactStep(2 + (4 + 0x13) + 4, 0);
    check(session.sendChat(5, "okya", 2s), "valid sendChat after rejection failed");
    const auto wire2 = game.awaitReceived(5s);
    const auto plain2 = decryptGameFrame(wire2, &fixtureRc4, 0x4050, /*seq=*/5);
    validateChatPayload(plain2, /*channel=*/5, "okya", /*seq=*/5);
    session.teardown();
    check(game.stepError().empty(), "game fixture error: " + game.stepError());
}

// Only the established state may send: idle and mid-gateway sessions refuse.
void test_session_send_chat_requires_established() {
    {
        LoopbackServer gateway;
        GameSession session(synthCredentials(), 1, 30s);
        check(!session.sendChat(5, "hello", 100ms),
              "sendChat accepted the idle state");
        check(session.state() == SessionState::idle,
              "idle rejection changed the session state");
        check(!session.errorMessage().empty(), "idle rejection lost its diagnostic");
        check(gateway.waitAccepting(300ms), "idle sendChat touched the gateway");
    }
    {
        LoopbackServer gateway;
        GameSession session(synthCredentials(), 1, 30s);
        check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
        expectGatewayStartBatch(&gateway);
        check(!session.sendChat(5, "hello", 100ms),
              "sendChat accepted the gatewayAuthenticating state");
        check(session.state() == SessionState::gatewayAuthenticating,
              "gateway rejection changed the session state");
        session.teardown();
    }
}

// A write failure must not report success: once the game connection is
// aborted, sendChat fails and the session fails closed.
void test_session_send_chat_write_failure_fails_closed() {
    LoopbackServer gateway;
    LoopbackServer game;
    GameSession session(synthCredentials(), 1, 30s);
    auto fixtureRc4 = establishEncryptedSession(&gateway, &game, &session);
    (void)fixtureRc4;
    game.push([](SOCKET client) {
        linger abort{1, 0};
        ::setsockopt(client, SOL_SOCKET, SO_LINGER,
                     reinterpret_cast<const char*>(&abort), sizeof(abort));
        ::closesocket(client);
    });
    check(game.waitIdle(2s), "abort step did not run");
    // The RST needs a moment to reach the client; probe with real chat sends
    // and stop as soon as one reports the failure.
    bool failed = false;
    for (int attempt = 0; attempt < 50 && !failed; ++attempt) {
        failed = !session.sendChat(5, "hello", 200ms);
        std::this_thread::sleep_for(10ms);
    }
    check(failed, "aborted connection did not fail sendChat");
    check(session.state() == SessionState::failed, "session not failed");
    check(session.error() == SessionError::disconnected, "expected disconnected");
    check(!session.errorMessage().empty(), "write failure lost its diagnostic");
}

// ---------------------------------------------------------------------------
// OnlineChatWorker tests (online chat stage). The worker owns the GameSession
// on a dedicated thread; these tests prove the UI-thread contract on loopback
// only (127.0.0.1, ephemeral ports, SYNTH_* credentials): the evidenced login
// + init flow runs without the UI thread ever touching the socket, decrypted
// 0x303f frames arrive as chatMessage events, UI sends hit the wire as real
// 0x4050 frames without any echo, and queue/stop/failure edge cases fail
// closed with sanitized diagnostics.

using dhsh::OnlineChatWorker;

// Encrypts one synthetic s2c game frame of any size: bytes [0x0e,0x12) become
// the table-cipher seed (client-side decryption skips them); bytes below 16
// additionally carry the cipher's head rotation, handled by
// tableCipherEncryptInPlace.
std::vector<std::uint8_t> encryptS2cFrame(std::vector<std::uint8_t> plain) {
    check(plain.size() >= 0x12, "fixture s2c frame below the cipher minimum");
    plain[0x0e] = 0x39;  // arbitrary valid seed, as in makeS2cGameFrame
    plain[0x0f] = 0x00;
    plain[0x10] = 0x00;
    plain[0x11] = 0x00;
    tableCipherEncryptInPlace(plain.data(), plain.size());
    return makeFrame(plain);
}

// Builds one synthetic s2c 0x303f chat frame in the REAL wire layout:
// [u16le cmd 0x303f][DHR1 payload], where the DHR1 layout follows the
// ChatMessageDecoder offsets (channel u32le at DHR1+4, GBK sender NUL-padded
// in [26,58), GBK text NUL-terminated at +144, no interior NULs). On the wire
// those fields therefore land at +6 / +28 / +146 -- the worker must strip the
// 2-byte command before decoding (mirrors test_chat.cpp's make_payload, which
// builds the DHR1 payload without the cmd).
std::vector<std::uint8_t> makeS2cChatFrame(std::uint32_t channel,
                                           const std::string& senderGbk,
                                           const std::string& textGbk) {
    check(senderGbk.size() < 32, "fixture sender exceeds the 32-byte slot");
    check(textGbk.find('\0') == std::string::npos,
          "fixture chat text carries an interior NUL");
    std::vector<std::uint8_t> dhr1(144 + textGbk.size() + 1, 0);
    writeLe32(dhr1.data() + 4, channel);
    std::memcpy(dhr1.data() + 26, senderGbk.data(), senderGbk.size());
    std::memcpy(dhr1.data() + 144, textGbk.data(), textGbk.size());
    std::vector<std::uint8_t> plain(2 + dhr1.size(), 0);
    plain[0] = 0x3f;
    plain[1] = 0x30;
    std::copy(dhr1.begin(), dhr1.end(), plain.begin() + 2);
    return encryptS2cFrame(std::move(plain));
}

// Waits for the roleList event and pins the accumulated count; the caller
// validates the role contents.
void awaitWorkerRoleList(OnlineChatWorker* worker, std::size_t expectedCount) {
    OnlineChatWorker::Event event{};
    check(worker->waitEvent(&event, 10s), "no roleList event before deadline");
    check(event.type == OnlineChatWorker::Event::Type::roleList,
          "worker event is not a role list");
    check(event.roles.size() == expectedCount,
          "roleList carries " + std::to_string(event.roles.size()) +
              " roles, expected " + std::to_string(expectedCount));
}

// Waits for the graph-key challenge event and pins the forwarded image: the
// same 96x32 RGBA the session decoded (pixel for pixel), no diagnostic text,
// and never any raw gateway payload (the worker event type carries no
// payload field at all).
void awaitWorkerGraphKeyChallenge(OnlineChatWorker* worker) {
    OnlineChatWorker::Event event{};
    check(worker->waitEvent(&event, 10s), "no graph-key challenge before deadline");
    check(event.type == OnlineChatWorker::Event::Type::graphKeyChallenge,
          "worker event is not the graph-key challenge");
    check(event.graphKeyImage.width == kGraphKeyImageWidth &&
              event.graphKeyImage.height == kGraphKeyImageHeight,
          "worker challenge image size mismatch");
    const std::vector<std::uint8_t> expected = expectedGraphKeyRgba(synthGraphKeyPixels());
    check(event.graphKeyImage.rgba == expected,
          "worker challenge RGBA does not match the decoded image");
    check(event.message.empty(),
          "worker challenge event carried a diagnostic");
    check(event.roles.empty(),
          "worker challenge event carried non-challenge data");
}

// Waits for a loginRejected event and proves the rejected command's text
// (graph key, role name) never reaches the event message.
void awaitWorkerLoginRejected(OnlineChatWorker* worker,
                              const std::string& secret) {
    OnlineChatWorker::Event event{};
    check(worker->waitEvent(&event, 10s), "no loginRejected event before deadline");
    check(event.type == OnlineChatWorker::Event::Type::loginRejected,
          "worker event is not a login rejection");
    check(event.message.find(secret) == std::string::npos,
          "login rejection leaked the submitted text");
}

// Drives the worker's evidenced gateway login up to and including the 0x1002
// role/graph frame, leaving the gateway fixture connected for the test to
// push any extra frames and the redirect. The server sends an initial 0x2005
// before the role records; the worker must stay silent about it (gateway
// frames are internal and this one precedes the selection). Proves along the
// way: the roleList event carries exactly the accumulated role id/GBK-name
// pairs, the 0x100b follow-up leaves only after the explicit worker
// ->selectRole, and the 0x1002 frame only after the 0x2005 challenge that
// follows the selection and the explicit worker->submitGraphKey. Each receive
// step is queued right before its await so the fixture never holds two
// completed receives.
void driveWorkerLoginThroughGraphKey(OnlineChatWorker* worker,
                                     LoopbackServer* gateway) {
    gateway->recvExactStep(16, 0);
    validateHelloOrEmpty(gateway->awaitReceived(5s), 0x1005, "worker 0x1005");
    gateway->recvExactStep(226, 0);
    validateAuthRequest(gateway->awaitReceived(5s));
    gateway->recvExactStep(2 + 0x0e + kSynthEnvReport.size(), 0);
    validateEnvReport(gateway->awaitReceived(5s));

    gateway->sendStep(makeGraphKeyFrame(), 0, 0);
    gateway->sendStep(makeRoleListFrame(synthRoleSet()), 0, 0);
    awaitWorkerRoleList(worker, 2);
    check(worker->selectRole(kSynthRoleIdA), "worker rejected the role selection");
    gateway->recvExactStep(16, 0);
    validateHelloOrEmpty(gateway->awaitReceived(5s), 0x100b, "worker 0x100b");

    gateway->sendStep(makeGraphKeyFrame(), 0, 0);
    awaitWorkerGraphKeyChallenge(worker);
    check(worker->submitGraphKey(kSynthRoleCode),
          "worker rejected the graph-key submit");
    gateway->recvExactStep(43, 0);
    validateRoleSelect(gateway->awaitReceived(5s), kSynthRoleNameGbk,
                       kSynthRoleCode);
}

// Queues the game-fixture login steps (0x4004 receive + first s2c frame) that
// must precede every later per-connection step, validates the Blowfish login
// frame, and returns the fixture-side RC4 key material.
std::array<std::uint8_t, dhsh::kRc4SessionKeySize> queueWorkerGameLogin(
        LoopbackServer* game) {
    game->recvExactStep(113, 0);
    game->sendStep(makeS2cGameFrame(0x300b, {0x01, 0x00, 0x00, 0x00}), 0, 0);
    const auto blob = validateGameLogin(game->awaitReceived(5s),
                                        kSynthSession, /*seq=*/1);
    return dhsh::deriveRc4Key(blob);
}

// After the role/graph stage, gateway frames (e.g. the 0x200b flags) and the
// redirect stay internal: the next worker event must be the established
// status.
void awaitWorkerEstablished(OnlineChatWorker* worker) {
    OnlineChatWorker::Event event{};
    check(worker->waitEvent(&event, 10s), "no worker event before deadline");
    check(event.type == OnlineChatWorker::Event::Type::statusChanged &&
          event.status == OnlineChatWorker::Status::established,
          "worker event is not the established status");
}

// Validates the documented init prefix (0x4022 x2, 0x40d0 x4 sub 7/2/1/3,
// 0x4665) against the fixture-side RC4 session after establishment.
void expectWorkerInitSequence(LoopbackServer* game, dhsh::Rc4Session* fixtureRc4) {
    struct {
        std::uint16_t cmd;
        std::vector<std::uint8_t> body;
        std::uint32_t seq;
    } expected[] = {
        {0x4022, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 3},
        {0x4022, {0, 0, 4, 0, 0, 0, 0x20, 0x03, 0, 0, 0, 0}, 5},
        {0x40d0, {0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 7},
        {0x40d0, {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 9},
        {0x40d0, {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 11},
        {0x40d0, {0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 13},
        {0x4665, {0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 15},
    };
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        game->recvExactStep(24, 0);
        const auto wire = game->awaitReceived(5s);
        const auto plain = decryptGameFrame(wire, fixtureRc4, expected[i].cmd,
                                            expected[i].seq);
        check(std::memcmp(plain.data() + 2, expected[i].body.data(), 12) == 0,
              "worker init frame body mismatch at index " + std::to_string(i));
    }
}

void awaitIgnoredCount(OnlineChatWorker* worker, std::uint64_t expected) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 5s) {
        if (worker->ignoredCommandCount() == expected) {
            return;
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("ignoredCommandCount did not reach " +
                             std::to_string(expected));
}

// Configuration regression: pollTimeout doubles as the whole-frame read
// deadline (GameSession::poll passes it to readFrame), so the default must
// cover the server's observed ~80-90ms batched responses with headroom --
// the 20ms default it replaced timed out mid-frame against the real gateway
// (2026-11-24 online root cause). Guard the defaults so they cannot silently
// regress.
void test_online_worker_default_options_cover_server_response() {
    OnlineChatWorker::Options options;
    check(options.pollTimeout == std::chrono::seconds(1),
          "default pollTimeout drifted from 1s: " +
              std::to_string(options.pollTimeout.count()) + "ms");
    check(options.pollTimeout <= std::chrono::seconds(1),
          "default pollTimeout exceeds the 1s bound: " +
              std::to_string(options.pollTimeout.count()) + "ms");
    check(options.authDeadline == std::chrono::seconds(30),
          "default authDeadline drifted from 30s: " +
              std::to_string(options.authDeadline.count()) + "ms");
}

// Full loopback login through the worker: the evidenced gateway batch, the
// 0x2002 redirect, the Blowfish game login, the first s2c frame, and the
// documented init prefix all happen on the worker thread; the UI thread only
// waits for the established status event.
void test_online_worker_login_emits_established() {
    LoopbackServer gateway;
    LoopbackServer game;
    OnlineChatWorker worker(synthCredentials(), "127.0.0.1", gateway.port());
    check(worker.start(), "worker start failed");

    driveWorkerLoginThroughGraphKey(&worker, &gateway);
    // The 0x200b flag frame stays internal: after the redirect the next UI
    // event must still be the established status.
    gateway.sendStep(makeFrame([] {
        std::vector<std::uint8_t> p(26, 0);
        p[0] = 0x0b; p[1] = 0x20;
        p[2] = 'N'; p[3] = 'N'; p[4] = 'Y';
        return p;
    }()), 0, 0);
    gateway.sendStep(makeRedirectFrame(kSynthSession, 127, 0, 0, 1,
                                       game.port()), 0, 0);
    const auto rc4Key = queueWorkerGameLogin(&game);
    awaitWorkerEstablished(&worker);

    dhsh::Rc4Session fixtureRc4(rc4Key.data(), rc4Key.size());
    expectWorkerInitSequence(&game, &fixtureRc4);

    check(worker.status() == OnlineChatWorker::Status::established,
          "worker status not established");
    check(worker.ignoredCommandCount() == 1,
          "first s2c 0x300b frame not counted as ignored");

    worker.stop();
    check(worker.status() == OnlineChatWorker::Status::stopped,
          "stop did not move the worker to stopped");
    check(game.stepError().empty(), "game fixture error: " + game.stepError());
}

// The worker's roleList event carries exactly the UI display fields -- role
// id and GBK name -- and nothing else: no gateway payload, no diagnostics.
void test_online_worker_role_list_event_contents() {
    LoopbackServer gateway;
    OnlineChatWorker worker(synthCredentials(), "127.0.0.1", gateway.port());
    check(worker.start(), "worker start failed");

    gateway.recvExactStep(16, 0);
    validateHelloOrEmpty(gateway.awaitReceived(5s), 0x1005, "worker 0x1005");
    gateway.recvExactStep(226, 0);
    validateAuthRequest(gateway.awaitReceived(5s));
    gateway.recvExactStep(2 + 0x0e + kSynthEnvReport.size(), 0);
    validateEnvReport(gateway.awaitReceived(5s));

    gateway.sendStep(makeRoleListFrame(synthRoleSet()), 0, 0);
    OnlineChatWorker::Event event{};
    check(worker.waitEvent(&event, 10s), "no roleList event before deadline");
    check(event.type == OnlineChatWorker::Event::Type::roleList,
          "worker event is not a role list");
    check(event.roles.size() == 2, "roleList role count mismatch");
    check(event.roles[0].id == kSynthRoleIdA &&
          event.roles[0].nameGbk == kSynthRoleNameGbk,
          "role A display fields mismatch");
    check(event.roles[1].id == kSynthRoleIdB &&
          event.roles[1].nameGbk == kSynthRoleNameGbkB,
          "role B display fields mismatch");
    // The event is display data only: no diagnostic text, channel or counts.
    check(event.message.empty() && event.channel == 0 &&
          event.textLengthBytes == 0,
          "roleList event carried non-display fields");

    worker.stop();
}

// Worker login commands fail closed: selections are only accepted while
// connecting, an unknown role id, a duplicate selection, and a graph-key
// submit before the challenge are all refused with a sanitized loginRejected
// event, and none of them touch the wire. The flow still completes normally
// afterwards.
void test_online_worker_login_commands_fail_closed() {
    LoopbackServer gateway;
    OnlineChatWorker worker(synthCredentials(), "127.0.0.1", gateway.port());

    // API-level rejection is synchronous and independent of the worker
    // thread: before start() the worker is idle, so the command is refused
    // on the spot with a loginRejected event.
    check(!worker.selectRole(kSynthRoleIdA),
          "selectRole accepted the idle state");
    awaitWorkerLoginRejected(&worker, kSynthRoleNameGbk);

    check(worker.start(), "worker start failed");

    gateway.recvExactStep(16, 0);
    validateHelloOrEmpty(gateway.awaitReceived(5s), 0x1005, "worker 0x1005");
    gateway.recvExactStep(226, 0);
    validateAuthRequest(gateway.awaitReceived(5s));
    gateway.recvExactStep(2 + 0x0e + kSynthEnvReport.size(), 0);
    validateEnvReport(gateway.awaitReceived(5s));

    // The received 0x1005 proves run() already set status_ to connecting, so
    // this command is queued for the worker thread; with no role list yet
    // the SESSION refuses the never-listed id -- exactly one loginRejected
    // event and a wire that stays silent.
    check(worker.selectRole(kSynthRoleIdA),
          "worker rejected the role-selection command");
    gateway.recvQuietStep(300);
    awaitWorkerLoginRejected(&worker, kSynthRoleNameGbk);
    check(gateway.awaitReceived(5s).empty(),
          "unknown-id selection touched the wire");

    // The server sends an initial 0x2005 before the role records: it precedes
    // the selection, so the worker must stay silent -- no challenge event.
    gateway.sendStep(makeGraphKeyFrame(), 0, 0);
    {
        OnlineChatWorker::Event event{};
        check(!worker.waitEvent(&event, 300ms),
              "worker surfaced a challenge for the pre-selection 0x2005");
    }

    gateway.sendStep(makeRoleListFrame(synthRoleSet()), 0, 0);
    awaitWorkerRoleList(&worker, 2);

    // Unknown role id: queued, refused by the session, wire stays silent.
    check(worker.selectRole(0x0000deadU), "queue rejected the unknown-id selection");
    gateway.recvQuietStep(300);
    awaitWorkerLoginRejected(&worker, kSynthRoleNameGbk);
    check(gateway.awaitReceived(5s).empty(),
          "unknown-id selection touched the wire");

    // Valid selection sends the 0x100b follow-up.
    check(worker.selectRole(kSynthRoleIdA), "worker rejected the role selection");
    gateway.recvExactStep(16, 0);
    validateHelloOrEmpty(gateway.awaitReceived(5s), 0x100b, "worker 0x100b");

    // A second selection is a duplicate submit: refused, nothing on the wire.
    check(worker.selectRole(kSynthRoleIdB), "queue rejected the duplicate selection");
    gateway.recvQuietStep(300);
    awaitWorkerLoginRejected(&worker, kSynthRoleNameGbkB);
    check(gateway.awaitReceived(5s).empty(),
          "duplicate selection sent a second 0x100b");

    // Graph-key submit before the 0x2005 challenge: refused, wire silent.
    check(worker.submitGraphKey(kSynthRoleCode),
          "queue rejected the early graph-key submit");
    gateway.recvQuietStep(300);
    awaitWorkerLoginRejected(&worker, kSynthRoleCode);
    check(gateway.awaitReceived(5s).empty(),
          "early graph-key submit touched the wire");

    // The challenge that follows the selection unlocks the normal path.
    gateway.sendStep(makeGraphKeyFrame(), 0, 0);
    awaitWorkerGraphKeyChallenge(&worker);

    // A repeat 0x2005 stays inert: no second challenge event.
    gateway.sendStep(makeGraphKeyFrame(), 0, 0);
    {
        OnlineChatWorker::Event event{};
        check(!worker.waitEvent(&event, 300ms),
              "repeat 0x2005 re-emitted the challenge");
    }

    check(worker.submitGraphKey(kSynthRoleCode),
          "worker rejected the graph-key submit");
    gateway.recvExactStep(43, 0);
    validateRoleSelect(gateway.awaitReceived(5s), kSynthRoleNameGbk,
                       kSynthRoleCode);

    worker.stop();
    check(gateway.stepError().empty(), "gateway fixture error: " + gateway.stepError());
}

// Decrypted 0x303f frames become chatMessage events with decoded UTF-8; other
// game commands are dropped after incrementing the ignored counter.
void test_online_worker_303f_becomes_chat_event() {
    LoopbackServer gateway;
    LoopbackServer game;
    OnlineChatWorker worker(synthCredentials(), "127.0.0.1", gateway.port());
    check(worker.start(), "worker start failed");
    driveWorkerLoginThroughGraphKey(&worker, &gateway);
    gateway.sendStep(makeRedirectFrame(kSynthSession, 127, 0, 0, 1,
                                       game.port()), 0, 0);
    const auto rc4Key = queueWorkerGameLogin(&game);
    awaitWorkerEstablished(&worker);
    dhsh::Rc4Session fixtureRc4(rc4Key.data(), rc4Key.size());
    expectWorkerInitSequence(&game, &fixtureRc4);

    // Synthetic GBK sender/text (sender = U+6D4B U+8BD5 U+8005, text =
    // U+4F60 U+597D U+FF0C U+4E16 U+754C); the UTF-8 expectations below pin
    // the decode the worker must surface in the event.
    game.sendStep(makeS2cChatFrame(7, "\xb2\xe2\xca\xd4\xd5\xdf",
                                   "\xc4\xe3\xba\xc3\xa3\xac\xca\xc0\xbd\xe7"), 0, 0);
    // Rich-text shape: color switch + line break exercise the span plumbing.
    game.sendStep(makeS2cChatFrame(3, "SYNTH_NPC", "#bluehello\nworld"), 0, 0);
    // A non-0x303f game frame must be ignored, not surfaced.
    game.sendStep(makeS2cGameFrame(0x300b, {0x01, 0x00, 0x00, 0x00}), 0, 0);

    OnlineChatWorker::Event event{};
    check(worker.waitEvent(&event, 5s), "no chat event for the CJK frame");
    check(event.type == OnlineChatWorker::Event::Type::chatMessage,
          "first event is not a chat message");
    check(event.channel == 7, "chat event channel mismatch");
    check(event.chat.channel == 7, "decoded chat channel mismatch");
    check(event.chat.sender == "\xe6\xb5\x8b\xe8\xaf\x95\xe8\x80\x85",
          "decoded chat sender mismatch");
    check(event.chat.spans.size() == 1 && event.chat.spans[0].text ==
              "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c",
          "decoded chat text mismatch");

    check(worker.waitEvent(&event, 5s), "no chat event for the rich-text frame");
    check(event.type == OnlineChatWorker::Event::Type::chatMessage &&
          event.channel == 3, "rich-text chat event mismatch");
    check(event.chat.sender == "SYNTH_NPC", "rich-text sender mismatch");
    check(event.chat.spans.size() == 3, "rich-text span count mismatch");
    check(event.chat.spans[0].kind == dhsh::ChatSpanKind::text &&
          event.chat.spans[0].text == "hello" &&
          event.chat.spans[0].color == "blue", "rich-text span 0 mismatch");
    check(event.chat.spans[1].kind == dhsh::ChatSpanKind::line_break,
          "rich-text span 1 not a line break");
    check(event.chat.spans[2].kind == dhsh::ChatSpanKind::text &&
          event.chat.spans[2].text == "world", "rich-text span 2 mismatch");

    awaitIgnoredCount(&worker, 2);  // login 0x300b + the extra one above
    worker.stop();
    check(game.stepError().empty(), "game fixture error: " + game.stepError());
}

// A UI send queues on the UI thread and lands on the wire as a real 0x4050
// frame (RC4 + trailer + sequence continuity after the init prefix), and the
// worker never echoes it back as an event.
void test_online_worker_ui_send_hits_wire_no_echo() {
    LoopbackServer gateway;
    LoopbackServer game;
    OnlineChatWorker worker(synthCredentials(), "127.0.0.1", gateway.port());
    check(worker.start(), "worker start failed");
    driveWorkerLoginThroughGraphKey(&worker, &gateway);
    gateway.sendStep(makeRedirectFrame(kSynthSession, 127, 0, 0, 1,
                                       game.port()), 0, 0);
    const auto rc4Key = queueWorkerGameLogin(&game);
    awaitWorkerEstablished(&worker);
    dhsh::Rc4Session fixtureRc4(rc4Key.data(), rc4Key.size());
    expectWorkerInitSequence(&game, &fixtureRc4);

    game.recvExactStep(2 + (5 + 0x13) + 4, 0);
    check(worker.sendChat(9, "hello"), "worker rejected a valid send");
    const auto wire = game.awaitReceived(5s);
    const auto plain = decryptGameFrame(wire, &fixtureRc4, 0x4050, /*seq=*/17);
    validateChatPayload(plain, /*channel=*/9, "hello", /*seq=*/17);

    // No echo, no pseudo-success event: the queue stays empty after a send.
    OnlineChatWorker::Event event{};
    check(!worker.waitEvent(&event, 300ms),
          "worker echoed the send or reported a spurious event");

    worker.stop();
    check(game.stepError().empty(), "game fixture error: " + game.stepError());
}

// Sends before establishment return false immediately and queue a sendRejected
// event carrying the channel and byte length only -- never the text.
void test_online_worker_send_rejected_before_established() {
    OnlineChatWorker worker(synthCredentials(), "127.0.0.1", 9);
    check(!worker.sendChat(5, "hello"), "sendChat accepted the idle state");
    OnlineChatWorker::Event event{};
    check(worker.tryPopEvent(&event), "rejection produced no event");
    check(event.type == OnlineChatWorker::Event::Type::sendRejected,
          "rejection event type mismatch");
    check(event.channel == 5 && event.textLengthBytes == 5,
          "rejection event fields mismatch");
    check(event.message.find("hello") == std::string::npos,
          "rejection event leaked the text");

    // A connect failure (port 9 is the loopback discard) surfaces as failed;
    // stop() must not overwrite the failed status, and sends stay rejected.
    check(worker.start(), "worker start failed");
    check(worker.waitEvent(&event, 10s), "no worker event before deadline");
    check(event.type == OnlineChatWorker::Event::Type::statusChanged &&
          event.status == OnlineChatWorker::Status::failed,
          "connect failure did not produce the failed status");
    check(worker.status() == OnlineChatWorker::Status::failed,
          "worker status not failed after connect failure");
    check(!worker.sendChat(5, "hello"), "sendChat accepted the failed state");
    check(worker.tryPopEvent(&event) &&
          event.type == OnlineChatWorker::Event::Type::sendRejected,
          "failed-state rejection produced no event");
    worker.stop();
    check(worker.status() == OnlineChatWorker::Status::failed,
          "stop overwrote the failed status");
}

// A flooded outgoing queue rejects sends with false + a length-only event; the
// poll timeout bounds how long stop() waits for the worker to notice.
void test_online_worker_outgoing_queue_full_rejects() {
    LoopbackServer gateway;
    LoopbackServer game;
    OnlineChatWorker::Options options;
    // Keep the worker asleep in poll while the UI floods, but stay below the
    // 5s fixture receive deadline used by the login drive helpers (the flood
    // itself completes in milliseconds, far inside one poll wait).
    options.pollTimeout = 2s;
    options.sendTimeout = 500ms;
    OnlineChatWorker worker(synthCredentials(), "127.0.0.1", gateway.port(),
                            options);
    check(worker.start(), "worker start failed");
    driveWorkerLoginThroughGraphKey(&worker, &gateway);
    gateway.sendStep(makeRedirectFrame(kSynthSession, 127, 0, 0, 1,
                                       game.port()), 0, 0);
    const auto rc4Key = queueWorkerGameLogin(&game);
    awaitWorkerEstablished(&worker);
    dhsh::Rc4Session fixtureRc4(rc4Key.data(), rc4Key.size());
    expectWorkerInitSequence(&game, &fixtureRc4);

    const std::string big(4000, 'a');
    bool rejected = false;
    for (int i = 0; i < 4096 && !rejected; ++i) {
        rejected = !worker.sendChat(5, big);
    }
    check(rejected, "outgoing queue never filled under flood");

    OnlineChatWorker::Event event{};
    check(worker.waitEvent(&event, 5s), "no rejection event for the full queue");
    check(event.type == OnlineChatWorker::Event::Type::sendRejected &&
          event.channel == 5 && event.textLengthBytes == 4000,
          "full-queue rejection event fields mismatch");
    check(event.message.find("aaaa") == std::string::npos,
          "full-queue rejection leaked the text");

    worker.stop();
    check(game.stepError().empty(), "game fixture error: " + game.stepError());
}

// The login-command queue is bounded by Options::maxLoginCommands: commands
// past the bound are refused immediately with a queue-full loginRejected
// event and never reach the session or any log.
void test_online_worker_login_command_queue_full_rejects() {
    LoopbackServer gateway;
    OnlineChatWorker::Options options;
    // Keep the worker asleep in poll while the UI floods (the flood itself
    // completes in microseconds, far inside one poll wait).
    options.pollTimeout = 2s;
    const std::size_t bound = options.maxLoginCommands;
    OnlineChatWorker worker(synthCredentials(), "127.0.0.1", gateway.port(),
                            options);
    check(worker.start(), "worker start failed");

    // Sync on the start batch: the received 0x1005 proves run() already set
    // status_ to connecting, so the commands below are queued rather than
    // state-rejected.
    gateway.recvExactStep(16, 0);
    validateHelloOrEmpty(gateway.awaitReceived(5s), 0x1005, "worker 0x1005");
    gateway.recvExactStep(226, 0);
    validateAuthRequest(gateway.awaitReceived(5s));
    gateway.recvExactStep(2 + 0x0e + kSynthEnvReport.size(), 0);
    validateEnvReport(gateway.awaitReceived(5s));

    // One unknown-id selection: the session refuses it (no role list yet) and
    // the rejection event marks the point where the worker has drained the
    // queue and gone back to sleep in poll. The queue is untouched from here
    // until that poll returns, so the flood below cannot be drained early.
    check(worker.selectRole(kSynthRoleIdA), "worker rejected the queued command");
    awaitWorkerLoginRejected(&worker, kSynthRoleNameGbk);

    // Fill the queue exactly to the bound; every command is accepted.
    for (std::size_t i = 1; i <= bound; ++i) {
        check(worker.selectRole(static_cast<std::uint32_t>(0x00001000 + i)),
              "queue rejected a command below the bound");
    }
    // The next command exceeds the bound: refused on the spot with a
    // queue-full event; the submitted id never reaches any event.
    check(!worker.selectRole(0x00002000U), "queue-full command accepted");
    OnlineChatWorker::Event event{};
    check(worker.waitEvent(&event, 5s), "no queue-full rejection event");
    check(event.type == OnlineChatWorker::Event::Type::loginRejected,
          "queue-full event type mismatch");
    check(event.message.find("queue full") != std::string::npos,
          "queue-full rejection lost its diagnostic: " + event.message);

    worker.stop();
}

// Rejected gateway auth fails the worker without any fallback: the game
// fixture is never contacted and later sends stay rejected.
void test_online_worker_bad_credentials_fail_no_fallback() {
    LoopbackServer gateway;
    LoopbackServer game;
    OnlineChatWorker worker(synthCredentials(), "127.0.0.1", gateway.port());
    check(worker.start(), "worker start failed");

    // Error-shaped 0x2000, then an orderly close -- mirrors the GameSession
    // rejection test above: the malformed role list fails at protocol_error.
    expectGatewayStartBatch(&gateway);
    gateway.sendStep(makeFrame([] {
        std::vector<std::uint8_t> p(20, 0);
        p[0] = 0x00; p[1] = 0x20;
        p[2] = 0x01;
        return p;
    }()), 0, 0);
    gateway.shutdownStep();

    OnlineChatWorker::Event event{};
    check(worker.waitEvent(&event, 10s), "no worker event before deadline");
    check(event.type == OnlineChatWorker::Event::Type::statusChanged &&
          event.status == OnlineChatWorker::Status::failed,
          "auth rejection did not produce the failed status");
    check(event.error == SessionError::protocol_error,
          "auth rejection error mapping mismatch");
    check(worker.status() == OnlineChatWorker::Status::failed,
          "worker status not failed after auth rejection");
    check(event.message.find(kSynthAccount) == std::string::npos &&
          event.message.find(kSynthPassword) == std::string::npos,
          "failure event leaked credential material");
    check(game.waitAccepting(300ms),
          "game fixture was contacted after rejected auth");

    check(!worker.sendChat(5, "hello"), "sendChat accepted the failed state");
    check(worker.tryPopEvent(&event) &&
          event.type == OnlineChatWorker::Event::Type::sendRejected,
          "post-failure rejection produced no event");
    worker.stop();
    check(worker.status() == OnlineChatWorker::Status::failed,
          "stop overwrote the failed status");
}

// A malformed 0x303f produces a diagnostic with the payload byte count only;
// the sender/text content never reaches the event message.
void test_online_worker_bad_303f_diagnostic_sanitized() {
    LoopbackServer gateway;
    LoopbackServer game;
    OnlineChatWorker worker(synthCredentials(), "127.0.0.1", gateway.port());
    check(worker.start(), "worker start failed");
    driveWorkerLoginThroughGraphKey(&worker, &gateway);
    gateway.sendStep(makeRedirectFrame(kSynthSession, 127, 0, 0, 1,
                                       game.port()), 0, 0);
    const auto rc4Key = queueWorkerGameLogin(&game);
    awaitWorkerEstablished(&worker);
    dhsh::Rc4Session fixtureRc4(rc4Key.data(), rc4Key.size());
    expectWorkerInitSequence(&game, &fixtureRc4);

    // Text region (DHR1 [144,148) = wire [146,150)) = 'a', NUL, 'b', NUL: the
    // interior NUL makes the decoder reject the DHR1 payload. The diagnostic
    // must carry the decoded-buffer byte count only, never this content.
    std::vector<std::uint8_t> dhr1(148, 0);
    writeLe32(dhr1.data() + 4, 5);
    const std::string leaker = "LEAKER";
    std::memcpy(dhr1.data() + 26, leaker.data(), leaker.size());
    dhr1[144] = 'a';
    dhr1[146] = 'b';
    std::vector<std::uint8_t> plain(2 + dhr1.size(), 0);
    plain[0] = 0x3f;
    plain[1] = 0x30;
    std::copy(dhr1.begin(), dhr1.end(), plain.begin() + 2);
    game.sendStep(encryptS2cFrame(std::move(plain)), 0, 0);

    OnlineChatWorker::Event event{};
    check(worker.waitEvent(&event, 5s), "no diagnostic event for the bad frame");
    check(event.type == OnlineChatWorker::Event::Type::diagnostic,
          "bad-frame event is not a diagnostic");
    check(event.message.find("0x303f") != std::string::npos,
          "diagnostic lost the command context");
    check(event.message.find("148") != std::string::npos,
          "diagnostic lost the decoded payload byte count");
    check(event.message.find("LEAKER") == std::string::npos,
          "diagnostic leaked payload content");
    check(worker.ignoredCommandCount() == 1,
          "bad 0x303f frame changed the ignored counter");

    // No chatMessage event follows the diagnostic.
    check(!worker.waitEvent(&event, 300ms),
          "bad frame still produced a chat event");
    worker.stop();
    check(game.stepError().empty(), "game fixture error: " + game.stepError());
}

// A server-side close after establishment fails the worker with the session's
// disconnected mapping instead of spinning or hanging.
void test_online_worker_server_disconnect_fails() {
    LoopbackServer gateway;
    LoopbackServer game;
    OnlineChatWorker worker(synthCredentials(), "127.0.0.1", gateway.port());
    check(worker.start(), "worker start failed");
    driveWorkerLoginThroughGraphKey(&worker, &gateway);
    gateway.sendStep(makeRedirectFrame(kSynthSession, 127, 0, 0, 1,
                                       game.port()), 0, 0);
    game.recvExactStep(113, 0);
    game.sendStep(makeS2cGameFrame(0x300b, {0x01, 0x00, 0x00, 0x00}), 0, 0);
    game.shutdownStep();
    validateGameLogin(game.awaitReceived(5s), kSynthSession, 1);

    awaitWorkerEstablished(&worker);
    OnlineChatWorker::Event event{};
    check(worker.waitEvent(&event, 10s), "no event after server close");
    check(event.type == OnlineChatWorker::Event::Type::statusChanged &&
          event.status == OnlineChatWorker::Status::failed,
          "server close did not produce the failed status");
    check(event.error == SessionError::disconnected,
          "server close error mapping mismatch");
    check(worker.status() == OnlineChatWorker::Status::failed,
          "worker status not failed after server close");
    worker.stop();
}

// Lifecycle robustness: double start refused, start after stop refused, stop
// idempotent (and never overwriting a terminal failed status), destructor
// safe with and without stop, never-started destructor safe.
void test_online_worker_start_stop_lifecycle() {
    {
        // Port 9 (loopback discard) refuses the connection, so this worker
        // ends failed; repeated stop must stay a safe no-op and must not
        // overwrite the terminal failed status.
        OnlineChatWorker worker(synthCredentials(), "127.0.0.1", 9);
        check(worker.start(), "first start refused");
        check(!worker.start(), "double start accepted");
        worker.stop();
        worker.stop();
        check(worker.status() == OnlineChatWorker::Status::failed,
              "idempotent stop overwrote the failed status");
        check(!worker.start(), "start after stop accepted");
    }
    {
        // A silent gateway keeps the worker connecting; stop must tear the
        // session down, join the thread and report stopped -- not failed.
        LoopbackServer silentGateway;
        OnlineChatWorker worker(synthCredentials(), "127.0.0.1",
                                silentGateway.port());
        check(worker.start(), "start refused against the silent gateway");
        worker.stop();
        check(worker.status() == OnlineChatWorker::Status::stopped,
              "stop of a connecting session did not report stopped");
        check(!worker.start(), "start after stop accepted");
    }
    {
        OnlineChatWorker worker(synthCredentials(), "127.0.0.1", 9);
        check(worker.start(), "start refused in destructor-only scope");
        // Destructor without an explicit stop must join the thread itself.
    }
    {
        OnlineChatWorker worker(synthCredentials(), "127.0.0.1", 9);
        // Never started: the destructor must be a no-op.
    }
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
        test_header_only_read_timeout_closes_stream();
        test_failed_write_closes_stream();
        test_read_timeout_then_recover();
        test_reconnect_resets_receive_state();
        test_repeated_connections_and_wsa_lifecycle();
        test_fixture_s2c_encrypt_matches_capture();
        test_md5_known_answers();
        test_session_full_login_loopback();
        test_session_default_security_code();
        test_session_start_sends_only_three_frames();
        test_session_role_list_single_frame_two_roles();
        test_session_role_list_accumulates_across_frames();
        test_session_role_list_rejects_malformed_frames();
        test_session_select_role_sends_100b_after_explicit_choice();
        test_session_early_2005_does_not_unlock_challenge();
        test_session_graph_key_gates_1002_until_2005();
        test_session_rejects_invalid_graph_key();
        test_session_1002_slot_boundary();
        test_session_graph_key_challenge_decodes_image();
        test_session_invalid_graph_key_challenge_fails_closed();
        test_session_gateway_auth_rejected_stops();
        test_session_unknown_gateway_command_stops();
        test_session_illegal_redirect_stops();
        test_session_gateway_timeout_stops();
        test_session_gateway_partial_frame_timeout_stops();
        test_session_gateway_disconnect_stops();
        test_session_gateway_desync_diagnostic_is_sanitized();
        test_session_game_disconnect_stops();
        test_session_game_partial_frame_timeout_stops();
        test_session_bad_usage_stops();
        test_session_send_chat_wire_format_and_seq();
        test_session_send_chat_rejects_invalid_input();
        test_session_send_chat_size_boundary();
        test_session_send_chat_cjk_boundary();
        test_session_send_chat_requires_established();
        test_session_send_chat_write_failure_fails_closed();
        test_online_worker_login_emits_established();
        test_online_worker_role_list_event_contents();
        test_online_worker_login_commands_fail_closed();
        test_online_worker_default_options_cover_server_response();
        test_online_worker_303f_becomes_chat_event();
        test_online_worker_ui_send_hits_wire_no_echo();
        test_online_worker_send_rejected_before_established();
        test_online_worker_outgoing_queue_full_rejects();
        test_online_worker_login_command_queue_full_rejects();
        test_online_worker_bad_credentials_fail_no_fallback();
        test_online_worker_bad_303f_diagnostic_sanitized();
        test_online_worker_server_disconnect_fails();
        test_online_worker_start_stop_lifecycle();
        std::cout << "game_session tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "game_session test failure: " << error.what() << '\n';
        return 1;
    }
}
