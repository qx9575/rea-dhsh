// Transport + GameSession core tests for Task 11 Steps 2/3. Every test talks
// only to loopback fixture servers (127.0.0.1, ephemeral ports) owned by this
// test process: one plays the plaintext login gateway, one the encrypted game
// server. No production server is ever contacted. All credentials are
// synthetic SYNTH_* values; the real 2026-11-23 capture material stays in
// .rea/ and is never embedded here.

#include "network/GameCrypto.h"
#include "network/GameSession.h"
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

// ---------------------------------------------------------------------------
// GameSession core tests (Task 11 Step 3). The fixture pair below simulates
// both ends evidenced in the 2026-11-23 capture: a plaintext login gateway
// (0x1005/0x1000/0x1500/0x100b/0x1002 c2s, 0x2002 redirect s2c) and the
// encrypted game server (0x4004 Blowfish login, RC4 + trailer afterwards,
// table-cipher s2c). All values are SYNTH_* fiction on 127.0.0.1.

using dhsh::GameSession;
using dhsh::GatewayAuthMaterial;
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

SessionCredentials synthCredentials() {
    SessionCredentials credentials;
    credentials.account = kSynthAccount;
    credentials.password = kSynthPassword;
    credentials.zoneNameGbk = kSynthZoneGbk;
    credentials.versionToken = kSynthVersion;
    credentials.macAddress = kSynthMac;
    credentials.environmentReportGbk = kSynthEnvReport;
    return credentials;
}

GatewayAuthMaterial synthAuthMaterial(const SessionCredentials&) {
    GatewayAuthMaterial material;
    const char* hex = "A1B2C3D4E5F60718293A4B5C6D7E8F90";
    std::memcpy(material.passwordDigestHex.data(), hex, 32);
    material.authParam = 0x34;
    const char* stored = "SYNTH8PW";
    std::memcpy(material.storedPassword.data(), stored, 8);
    material.slot2 = {'A', '7'};
    return material;
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

std::uint8_t rol8(std::uint8_t value, unsigned shift) {
    if (shift == 0) {
        return value;
    }
    return static_cast<std::uint8_t>((value << shift) | (value >> (8 - shift)));
}

// Exact inverse of dhsh::S2cTableCipher::decryptInPlace so the fixture can
// encrypt synthetic s2c game frames onto the real wire format: the client
// reads the pos seed from the cipher payload at +0x0e (载荷-relative, cmd
// included, per net-protocol.md §3) before undoing the head rotation,
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

void expectZeros(const std::uint8_t* data, std::size_t length,
                 const std::string& message) {
    for (std::size_t i = 0; i < length; ++i) {
        check(data[i] == 0, message + " (nonzero at offset " + std::to_string(i) + ")");
    }
}

// Validates the 0x1000 gateway auth request byte layout against the captured
// field offsets; only SYNTH values are compared.
void validateAuthRequest(const std::vector<std::uint8_t>& wire) {
    check(wire.size() == 226, "0x1000 wire size mismatch");
    const auto* p = wire.data() + 2;  // skip the length header
    check(readLe16(p) == 0x1000, "0x1000 command mismatch");
    expectZeros(p + 2, 0x0c, "0x1000 header not zeroed");
    check(std::memcmp(p + 0x0e, kSynthAccount.data(), kSynthAccount.size()) == 0,
          "0x1000 account field mismatch");
    expectZeros(p + 0x0e + kSynthAccount.size(),
                0x40 - (0x0e + kSynthAccount.size()), "0x1000 account not NUL padded");
    const GatewayAuthMaterial material = synthAuthMaterial(synthCredentials());
    check(std::memcmp(p + 0x40, material.passwordDigestHex.data(), 32) == 0,
          "0x1000 password digest field mismatch");
    check(p[0x60] == 0x34 && p[0x61] == 0x00, "0x1000 +0x60 param mismatch");
    check(std::memcmp(p + 0x62, kSynthZoneGbk.data(), kSynthZoneGbk.size()) == 0,
          "0x1000 zone field mismatch");
    expectZeros(p + 0x62 + kSynthZoneGbk.size(),
                0x7b - (0x62 + kSynthZoneGbk.size()), "0x1000 zone not NUL padded");
    check(std::memcmp(p + 0x7b, "000000000000", 12) == 0, "0x1000 +0x7b marker mismatch");
    expectZeros(p + 0x87, 0xaf - 0x87, "0x1000 +0x87 region not zeroed");
    check(std::memcmp(p + 0xaf, kSynthVersion.data(), kSynthVersion.size()) == 0,
          "0x1000 version token mismatch");
    expectZeros(p + 0xaf + kSynthVersion.size(),
                0xd6 - (0xaf + kSynthVersion.size()), "0x1000 version not NUL padded");
    check(readLe32(p + 0xdb) == 1, "0x1000 +0xdb constant mismatch");
    check(std::memcmp(p + 0xd6, material.slot2.data(), 2) == 0,
          "0x1000 +0xd6 slot mismatch");
    expectZeros(p + 0xd8, 3, "0x1000 +0xd6 padding mismatch");
    expectZeros(p + 0xdf, 224 - 0xdf, "0x1000 tail not zeroed");
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
    const std::size_t payloadSize = 0x0c + kSynthEnvReport.size();
    check(wire.size() == 2 + payloadSize, "0x1500 wire size mismatch");
    const auto* p = wire.data() + 2;
    check(readLe16(p) == 0x1500, "0x1500 command mismatch");
    expectZeros(p + 2, 10, "0x1500 header not zeroed");
    check(std::memcmp(p + 0x0c, kSynthEnvReport.data(), kSynthEnvReport.size()) == 0,
          "0x1500 report text mismatch");
}

void validateCaptchaSubmit(const std::vector<std::uint8_t>& wire) {
    check(wire.size() == 43, "0x1002 wire size mismatch");
    const auto* p = wire.data() + 2;
    check(readLe16(p) == 0x1002, "0x1002 command mismatch");
    expectZeros(p + 2, 0x0c, "0x1002 header not zeroed");
    const GatewayAuthMaterial material = synthAuthMaterial(synthCredentials());
    check(std::memcmp(p + 0x0e, material.storedPassword.data(), 8) == 0,
          "0x1002 stored-password field mismatch");
    expectZeros(p + 0x0e + 8, 0x22 - (0x0e + 8), "0x1002 stored field not NUL padded");
    check(std::memcmp(p + 0x22, "AB12", 4) == 0, "0x1002 captcha text mismatch");
    check(p[0x22 + 4] == 0, "0x1002 captcha not NUL terminated");
    expectZeros(p + 0x22 + 5, 41 - (0x22 + 5), "0x1002 tail not zeroed");
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

// Full evidenced flow: gateway batch -> captcha -> 0x2002 redirect -> game
// login -> first s2c frame -> documented init sequence -> teardown.
void test_session_full_login_loopback() {
    LoopbackServer gateway;
    LoopbackServer game;

    GameSession session(synthCredentials(), synthAuthMaterial, 1, 30s);
    check(session.state() == SessionState::idle, "session not idle at start");

    gateway.recvExactStep(16, 0);
    check(session.start("127.0.0.1", gateway.port(), 2s),
          "start failed: " + session.errorMessage());
    check(session.state() == SessionState::gatewayAuthenticating,
          "session not authenticating after start");
    validateHelloOrEmpty(gateway.awaitReceived(5s), 0x1005, "0x1005");

    gateway.recvExactStep(226, 0);
    validateAuthRequest(gateway.awaitReceived(5s));

    gateway.recvExactStep(2 + 0x0c + kSynthEnvReport.size(), 0);
    validateEnvReport(gateway.awaitReceived(5s));

    gateway.recvExactStep(16, 0);
    validateHelloOrEmpty(gateway.awaitReceived(5s), 0x100b, "0x100b");

    // Gateway pushes the flag frame, then waits for the captcha submission
    // before redirecting to the game fixture.
    gateway.sendStep(makeFrame([] {
        std::vector<std::uint8_t> p(26, 0);
        p[0] = 0x0b; p[1] = 0x20;
        p[2] = 'N'; p[3] = 'N'; p[4] = 'Y';
        return p;
    }()), 0, 0);
    check(session.submitCaptcha("AB12"), "submitCaptcha failed");
    gateway.recvExactStep(43, 0);
    validateCaptchaSubmit(gateway.awaitReceived(5s));

    // Fixture answers with the redirect once the captcha frame arrived; the
    // gateway side of the fixture then goes idle.
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

// Wrong auth: the gateway answers with an error-shaped 0x2000 and closes.
// The session must fail without ever touching the game fixture.
void test_session_gateway_auth_rejected_stops() {
    LoopbackServer gateway;
    LoopbackServer game;
    GameSession session(synthCredentials(), synthAuthMaterial, 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    for (int i = 0; i < 4; ++i) {
        const std::size_t count = i == 1 ? 226
            : i == 2 ? 2 + 0x0c + kSynthEnvReport.size() : 16;
        gateway.recvExactStep(count, 0);
        gateway.awaitReceived(5s);
    }
    gateway.sendStep(makeFrame([] {
        std::vector<std::uint8_t> p(20, 0);
        p[0] = 0x00; p[1] = 0x20;
        p[2] = 0x01;
        return p;
    }()), 0, 0);
    gateway.shutdownStep();

    SessionEvent event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::gatewayFrame &&
          event.command == 0x2000, "expected 0x2000 gateway frame");
    event = awaitEvent(&session, 10s);
    check(event.type == SessionEvent::Type::error, "expected error event");
    check(event.error == SessionError::disconnected, "expected disconnected");
    check(session.state() == SessionState::failed, "session not failed");
    check(session.error() == SessionError::disconnected, "error accessor mismatch");
    check(game.waitAccepting(300ms), "game fixture was contacted after rejection");
}

// A gateway command outside the observed handshake set fails closed.
void test_session_unknown_gateway_command_stops() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), synthAuthMaterial, 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    for (int i = 0; i < 4; ++i) {
        const std::size_t count = i == 1 ? 226
            : i == 2 ? 2 + 0x0c + kSynthEnvReport.size() : 16;
        gateway.recvExactStep(count, 0);
        gateway.awaitReceived(5s);
    }
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
        GameSession session(synthCredentials(), synthAuthMaterial, 1, 30s);
        check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
        for (int i = 0; i < 4; ++i) {
            const std::size_t count = i == 1 ? 226
                : i == 2 ? 2 + 0x0c + kSynthEnvReport.size() : 16;
            gateway.recvExactStep(count, 0);
            gateway.awaitReceived(5s);
        }
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
    GameSession session(synthCredentials(), synthAuthMaterial, 1, 500ms);
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

// A gateway that closes mid-handshake fails the session.
void test_session_gateway_disconnect_stops() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), synthAuthMaterial, 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    for (int i = 0; i < 4; ++i) {
        const std::size_t count = i == 1 ? 226
            : i == 2 ? 2 + 0x0c + kSynthEnvReport.size() : 16;
        gateway.recvExactStep(count, 0);
        gateway.awaitReceived(5s);
    }
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

// A game server that drops the connection before the first s2c frame fails
// the login; no fallback.
void test_session_game_disconnect_stops() {
    LoopbackServer gateway;
    LoopbackServer game;
    GameSession session(synthCredentials(), synthAuthMaterial, 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    for (int i = 0; i < 4; ++i) {
        const std::size_t count = i == 1 ? 226
            : i == 2 ? 2 + 0x0c + kSynthEnvReport.size() : 16;
        gateway.recvExactStep(count, 0);
        gateway.awaitReceived(5s);
    }
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

// API misuse fails closed instead of guessing: double start and init
// sequence before establishment are rejected.
void test_session_bad_usage_stops() {
    LoopbackServer gateway;
    GameSession session(synthCredentials(), synthAuthMaterial, 1, 30s);
    check(session.start("127.0.0.1", gateway.port(), 2s), "start failed");
    check(!session.sendInitSequence(100ms), "init before established accepted");
    check(session.state() == SessionState::gatewayAuthenticating,
          "bad usage changed session state");
    check(!session.start("127.0.0.1", gateway.port(), 100ms),
          "double start accepted");
    check(session.state() == SessionState::failed, "double start not failed");
    check(session.error() == SessionError::bad_usage, "expected bad_usage");
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
        test_fixture_s2c_encrypt_matches_capture();
        test_session_full_login_loopback();
        test_session_gateway_auth_rejected_stops();
        test_session_unknown_gateway_command_stops();
        test_session_illegal_redirect_stops();
        test_session_gateway_timeout_stops();
        test_session_gateway_disconnect_stops();
        test_session_game_disconnect_stops();
        test_session_bad_usage_stops();
        std::cout << "game_session tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "game_session test failure: " << error.what() << '\n';
        return 1;
    }
}
