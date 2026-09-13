// Thermal Governor — real loopback TCP transport tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every case runs real sockets between two threads. Synchronisation is
// explicit: the lockstep protocol below means neither side ever needs a
// sleep, a timeout or a watchdog, and every thread is joined.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "framework.hpp"

#include "thermal_governor/error.hpp"
#include "thermal_governor/protocol.hpp"
#include "thermal_governor/transport.hpp"

using thermal_governor::FrameLimits;
using thermal_governor::MessageKind;
using thermal_governor::ThermalErrorCode;
using thermal_governor::TcpListener;
using thermal_governor::TcpSocket;
using thermal_governor::encode_frame;

namespace {

/// Joins the wrapped thread on every exit path, including the early return
/// taken by a failing check, so no case ever leaks a running thread.
class ThreadGuard {
public:
    explicit ThreadGuard(std::thread worker) : worker_(std::move(worker)) {}
    ~ThreadGuard() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    ThreadGuard(const ThreadGuard&) = delete;
    ThreadGuard& operator=(const ThreadGuard&) = delete;
    ThreadGuard(ThreadGuard&&) = delete;
    ThreadGuard& operator=(ThreadGuard&&) = delete;

    void join() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    std::thread worker_;
};

/// Deterministic payload pattern, so reassembly can be compared exactly.
[[nodiscard]] std::vector<std::uint8_t> pattern_payload(std::size_t length, std::uint32_t seed) {
    std::vector<std::uint8_t> payload(length);
    std::uint32_t state = seed | 1U;
    for (std::size_t index = 0; index < length; ++index) {
        state = state * 1664525U + 1013904223U;
        payload[index] = static_cast<std::uint8_t>((state >> 24) & 0xFFU);
    }
    return payload;
}

/// Per-side result. Written by one thread and read only after the join, which
/// is itself the synchronisation point.
struct Outcome {
    bool failed = false;
    bool connected = false;
    bool close_reported = false;
    std::size_t frames = 0;
    std::string detail;
};

/// Bind a loopback listener on an ephemeral port.
[[nodiscard]] TcpListener make_listener(bool& ok) {
    auto bound = TcpListener::bind_loopback(0);
    if (!bound.has_value()) {
        ok = false;
        return TcpListener{};
    }
    ok = true;
    return std::move(bound.value());
}

constexpr std::uint32_t kFrameCount = 200;

[[nodiscard]] std::vector<std::uint8_t> frame_payload(std::uint32_t index) {
    return pattern_payload(static_cast<std::size_t>(index % 8U) * 17U, index);
}

}  // namespace

TG_CASE(transport, exchange_many_frames_both_directions) {
    bool ok = false;
    TcpListener listener = make_listener(ok);
    TG_CHECK(ok);
    TG_CHECK(listener.valid());
    TG_CHECK(listener.bound_port() != 0);
    const std::uint16_t port = listener.bound_port();

    Outcome client;
    ThreadGuard guard{std::thread([&client, port]() {
        auto connected = TcpSocket::connect_to("127.0.0.1", port);
        if (!connected.has_value()) {
            client.failed = true;
            client.detail = "connect failed: " + connected.error().detail;
            return;
        }
        TcpSocket socket = std::move(connected.value());
        client.connected = true;

        for (std::uint32_t index = 0; index < kFrameCount; ++index) {
            const std::vector<std::uint8_t> payload = frame_payload(index);
            const auto sent = socket.send_frame(MessageKind::PUBLISH_EVIDENCE, index,
                                                payload.data(), payload.size());
            if (!sent.ok()) {
                client.failed = true;
                client.detail = "client send failed at frame " + std::to_string(index);
                socket.close();
                return;
            }
            const auto reply = socket.recv_frame();
            if (!reply.has_value()) {
                client.failed = true;
                client.detail = "client receive failed at frame " + std::to_string(index);
                socket.close();
                return;
            }
            if (reply.value().kind != MessageKind::PUBLISH_ACK || reply.value().flags != index ||
                reply.value().payload != payload) {
                client.failed = true;
                client.detail = "client observed a mismatched reply at frame " +
                                std::to_string(index);
                socket.close();
                return;
            }
            ++client.frames;
        }
        socket.shutdown_send();
    })};

    Outcome server;
    auto accepted = listener.accept_one();
    if (!accepted.has_value()) {
        TG_FAIL("accept failed: " + accepted.error().detail);
    }
    TcpSocket server_socket = std::move(accepted.value());

    for (std::uint32_t index = 0; index < kFrameCount; ++index) {
        const auto frame = server_socket.recv_frame();
        if (!frame.has_value()) {
            server.failed = true;
            server.detail = "server receive failed at frame " + std::to_string(index);
            break;
        }
        const std::vector<std::uint8_t> expected = frame_payload(index);
        if (frame.value().kind != MessageKind::PUBLISH_EVIDENCE ||
            frame.value().flags != index || frame.value().payload != expected) {
            server.failed = true;
            server.detail = "server observed a mismatched frame at index " +
                            std::to_string(index);
            break;
        }
        ++server.frames;
        const auto sent = server_socket.send_frame(MessageKind::PUBLISH_ACK, index,
                                                   expected.data(), expected.size());
        if (!sent.ok()) {
            server.failed = true;
            server.detail = "server send failed at frame " + std::to_string(index);
            break;
        }
    }

    if (server.failed) {
        // Release the peer so the join below always completes.
        server_socket.close();
    } else {
        const auto after = server_socket.recv_frame();
        if (after.has_value()) {
            server.failed = true;
            server.detail = "server read a frame after the client stopped sending";
        } else if (after.error().code != ThermalErrorCode::TRANSPORT_CLOSED &&
                   after.error().code != ThermalErrorCode::FRAME_TRUNCATED) {
            server.failed = true;
            server.detail = "unexpected peer close code: " +
                            std::string(thermal_governor::to_string(after.error().code));
        } else {
            server.close_reported = true;
        }
    }

    guard.join();

    if (server.failed) {
        TG_FAIL("server: " + server.detail);
    }
    if (client.failed) {
        TG_FAIL("client: " + client.detail);
    }
    TG_CHECK(client.connected);
    TG_CHECK_EQ(client.frames, static_cast<std::size_t>(kFrameCount));
    TG_CHECK_EQ(server.frames, static_cast<std::size_t>(kFrameCount));
    TG_CHECK(server.close_reported);
}

TG_CASE(transport, one_mebibyte_payload_is_reassembled_exactly) {
    constexpr std::size_t kLargeBytes = 1024U * 1024U;
    const std::vector<std::uint8_t> large = pattern_payload(kLargeBytes, 0x5AU);
    const std::vector<std::uint8_t> small = pattern_payload(37, 0x11U);

    bool ok = false;
    TcpListener listener = make_listener(ok);
    TG_CHECK(ok);
    const std::uint16_t port = listener.bound_port();

    Outcome client;
    ThreadGuard guard{std::thread([&client, &large, &small, port]() {
        auto connected = TcpSocket::connect_to("127.0.0.1", port);
        if (!connected.has_value()) {
            client.failed = true;
            client.detail = "connect failed: " + connected.error().detail;
            return;
        }
        TcpSocket socket = std::move(connected.value());
        client.connected = true;

        const auto sent = socket.send_frame(MessageKind::PUBLISH_EVIDENCE, 1, large.data(),
                                            large.size());
        if (!sent.ok()) {
            client.failed = true;
            client.detail = "large send failed";
            socket.close();
            return;
        }
        const auto echoed = socket.recv_frame();
        if (!echoed.has_value()) {
            client.failed = true;
            client.detail = "large receive failed";
            socket.close();
            return;
        }
        if (echoed.value().kind != MessageKind::EVALUATE_RESPONSE ||
            echoed.value().payload.size() != large.size() ||
            echoed.value().payload != large) {
            client.failed = true;
            client.detail = "large payload was not reassembled exactly";
            socket.close();
            return;
        }
        ++client.frames;

        const auto small_sent = socket.send_frame(MessageKind::HEARTBEAT, 2, small.data(),
                                                  small.size());
        if (!small_sent.ok()) {
            client.failed = true;
            client.detail = "small send failed";
            socket.close();
            return;
        }
        const auto small_echo = socket.recv_frame();
        if (!small_echo.has_value() || small_echo.value().payload != small) {
            client.failed = true;
            client.detail = "small payload was not reassembled exactly";
            socket.close();
            return;
        }
        ++client.frames;
        socket.shutdown_send();
    })};

    Outcome server;
    auto accepted = listener.accept_one();
    if (!accepted.has_value()) {
        TG_FAIL("accept failed: " + accepted.error().detail);
    }
    TcpSocket server_socket = std::move(accepted.value());

    bool done = true;
    const auto received = server_socket.recv_frame();
    if (!received.has_value()) {
        done = false;
        server.failed = true;
        server.detail = "server large receive failed";
    } else if (received.value().kind != MessageKind::PUBLISH_EVIDENCE ||
               received.value().payload.size() != large.size() ||
               received.value().payload != large) {
        done = false;
        server.failed = true;
        server.detail = "server did not reassemble the large payload exactly";
    } else {
        ++server.frames;
        const auto sent = server_socket.send_frame(MessageKind::EVALUATE_RESPONSE, 1, large.data(),
                                                   large.size());
        if (!sent.ok()) {
            done = false;
            server.failed = true;
            server.detail = "server large reply failed";
        }
    }

    if (done) {
        const auto second = server_socket.recv_frame();
        if (!second.has_value()) {
            done = false;
            server.failed = true;
            server.detail = "server small receive failed";
        } else if (second.value().kind != MessageKind::HEARTBEAT ||
                   second.value().payload != small) {
            done = false;
            server.failed = true;
            server.detail = "server did not reassemble the small payload exactly";
        } else {
            ++server.frames;
            const auto sent = server_socket.send_frame(MessageKind::HEARTBEAT, 2, small.data(),
                                                       small.size());
            if (!sent.ok()) {
                done = false;
                server.failed = true;
                server.detail = "server small reply failed";
            }
        }
    }

    if (!done) {
        server_socket.close();
    } else {
        const auto after = server_socket.recv_frame();
        if (after.has_value()) {
            server.failed = true;
            server.detail = "server read a frame after the client stopped sending";
        } else if (after.error().code != ThermalErrorCode::TRANSPORT_CLOSED &&
                   after.error().code != ThermalErrorCode::FRAME_TRUNCATED) {
            server.failed = true;
            server.detail = "unexpected peer close code: " +
                            std::string(thermal_governor::to_string(after.error().code));
        } else {
            server.close_reported = true;
        }
    }

    guard.join();

    if (server.failed) {
        TG_FAIL("server: " + server.detail);
    }
    if (client.failed) {
        TG_FAIL("client: " + client.detail);
    }
    TG_CHECK_EQ(client.frames, static_cast<std::size_t>(2));
    TG_CHECK_EQ(server.frames, static_cast<std::size_t>(2));
    TG_CHECK(server.close_reported);
}

TG_CASE(transport, peer_close_is_reported) {
    bool ok = false;
    TcpListener listener = make_listener(ok);
    TG_CHECK(ok);
    const std::uint16_t port = listener.bound_port();

    const std::vector<std::uint8_t> payload = pattern_payload(96, 0x22U);
    Outcome client;
    ThreadGuard guard{std::thread([&client, &payload, port]() {
        auto connected = TcpSocket::connect_to("127.0.0.1", port);
        if (!connected.has_value()) {
            client.failed = true;
            client.detail = "connect failed: " + connected.error().detail;
            return;
        }
        TcpSocket socket = std::move(connected.value());
        client.connected = true;
        const auto sent = socket.send_frame(MessageKind::HEARTBEAT, 3, payload.data(),
                                            payload.size());
        if (!sent.ok()) {
            client.failed = true;
            client.detail = "send failed";
            socket.close();
            return;
        }
        ++client.frames;
        socket.shutdown_send();
    })};

    Outcome server;
    auto accepted = listener.accept_one();
    if (!accepted.has_value()) {
        TG_FAIL("accept failed: " + accepted.error().detail);
    }
    TcpSocket server_socket = std::move(accepted.value());

    const auto received = server_socket.recv_frame();
    if (!received.has_value()) {
        server.failed = true;
        server.detail = "server did not receive the frame before the close";
    } else if (received.value().kind != MessageKind::HEARTBEAT ||
               received.value().payload != payload) {
        server.failed = true;
        server.detail = "server received a mismatched frame";
    } else {
        ++server.frames;
        const auto after = server_socket.recv_frame();
        if (after.has_value()) {
            server.failed = true;
            server.detail = "server read a frame after the peer closed";
        } else if (after.error().code != ThermalErrorCode::TRANSPORT_CLOSED &&
                   after.error().code != ThermalErrorCode::FRAME_TRUNCATED) {
            server.failed = true;
            server.detail = "unexpected peer close code: " +
                            std::string(thermal_governor::to_string(after.error().code));
        } else {
            server.close_reported = true;
        }
    }

    guard.join();

    if (server.failed) {
        TG_FAIL("server: " + server.detail);
    }
    if (client.failed) {
        TG_FAIL("client: " + client.detail);
    }
    TG_CHECK_EQ(server.frames, static_cast<std::size_t>(1));
    TG_CHECK(server.close_reported);

    TG_PHASE("a closed socket reports the closure instead of pretending to send");
    TcpSocket closed;
    TG_CHECK(!closed.valid());
    const std::vector<std::uint8_t> dummy = pattern_payload(4, 1);
    TG_STATUS_ERROR_CODE(closed.send_all(dummy.data(), dummy.size()),
                         ThermalErrorCode::TRANSPORT_CLOSED);
    TG_ERROR_CODE(closed.recv_frame(), ThermalErrorCode::TRANSPORT_CLOSED);
    TG_STATUS_ERROR_CODE(closed.send_frame(MessageKind::HEARTBEAT, 0, dummy.data(), dummy.size()),
                         ThermalErrorCode::TRANSPORT_CLOSED);
    TG_CHECK_EQ(closed.peer_description(), std::string("<closed>"));
}

TG_CASE(transport, truncated_frame_is_reported) {
    bool ok = false;
    TcpListener listener = make_listener(ok);
    TG_CHECK(ok);
    const std::uint16_t port = listener.bound_port();

    const std::vector<std::uint8_t> payload = pattern_payload(64, 0x33U);
    const auto encoded = encode_frame(MessageKind::PUBLISH_EVIDENCE, 4, payload.data(),
                                      payload.size());
    TG_CHECK(encoded.has_value());
    const std::vector<std::uint8_t> truncated(
        encoded.value().begin(),
        encoded.value().begin() +
            static_cast<std::ptrdiff_t>(thermal_governor::kFrameHeaderSize + 8));

    Outcome client;
    ThreadGuard guard{std::thread([&client, &truncated, port]() {
        auto connected = TcpSocket::connect_to("127.0.0.1", port);
        if (!connected.has_value()) {
            client.failed = true;
            client.detail = "connect failed: " + connected.error().detail;
            return;
        }
        TcpSocket socket = std::move(connected.value());
        client.connected = true;
        const auto sent = socket.send_all(truncated.data(), truncated.size());
        if (!sent.ok()) {
            client.failed = true;
            client.detail = "partial send failed";
            socket.close();
            return;
        }
        client.frames = truncated.size();
        socket.shutdown_send();
    })};

    Outcome server;
    auto accepted = listener.accept_one();
    if (!accepted.has_value()) {
        TG_FAIL("accept failed: " + accepted.error().detail);
    }
    TcpSocket server_socket = std::move(accepted.value());

    const auto attempted = server_socket.recv_frame();
    if (attempted.has_value()) {
        server.failed = true;
        server.detail = "a truncated frame was accepted as complete";
    } else if (attempted.error().code != ThermalErrorCode::FRAME_TRUNCATED) {
        server.failed = true;
        server.detail = "an incomplete frame reported " +
                        std::string(thermal_governor::to_string(attempted.error().code)) +
                        " instead of FRAME_TRUNCATED";
    } else {
        server.close_reported = true;
    }

    guard.join();

    if (server.failed) {
        TG_FAIL("server: " + server.detail);
    }
    if (client.failed) {
        TG_FAIL("client: " + client.detail);
    }
    TG_CHECK(server.close_reported);

    TG_PHASE("a frame larger than the configured bound is refused before reading it");
    FrameLimits limits;
    limits.max_payload_bytes = 8;
    bool second_ok = false;
    TcpListener strict = make_listener(second_ok);
    TG_CHECK(second_ok);
    const std::uint16_t strict_port = strict.bound_port();

    Outcome strict_client;
    ThreadGuard second_guard{std::thread([&strict_client, strict_endpoint = strict_port]() {
        auto connected = TcpSocket::connect_to("127.0.0.1", strict_endpoint);
        if (!connected.has_value()) {
            strict_client.failed = true;
            strict_client.detail = "connect failed: " + connected.error().detail;
            return;
        }
        TcpSocket socket = std::move(connected.value());
        strict_client.connected = true;
        const std::vector<std::uint8_t> big = pattern_payload(1024, 0x44U);
        const auto sent = socket.send_frame(MessageKind::PUBLISH_EVIDENCE, 5, big.data(),
                                            big.size());
        strict_client.failed = !sent.ok();
        strict_client.detail = sent.ok() ? std::string{} : std::string("send failed");
        socket.close();
    })};

    auto strict_accepted = strict.accept_one();
    if (!strict_accepted.has_value()) {
        TG_FAIL("accept failed: " + strict_accepted.error().detail);
    }
    TcpSocket strict_socket = std::move(strict_accepted.value());
    TG_ERROR_CODE(strict_socket.recv_frame(limits), ThermalErrorCode::FRAME_TOO_LARGE);
    second_guard.join();
    if (strict_client.failed) {
        TG_FAIL("client: " + strict_client.detail);
    }
    TG_CHECK(strict_client.connected);
}

TG_CASE(transport, accept_one_honours_the_cooperative_stop_flag) {
    bool ok = false;
    TcpListener listener = make_listener(ok);
    TG_CHECK(ok);
    TG_CHECK(listener.valid());

    TG_PHASE("a stop flag that is already set");
    std::atomic<bool> stopped{true};
    TG_ERROR_CODE(listener.accept_one(&stopped), ThermalErrorCode::TRANSPORT_CLOSED);
    TG_CHECK(listener.valid());

    TG_PHASE("a stop flag set while the accept is waiting");
    std::atomic<bool> stop_late{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> reported_closed{false};
    ThreadGuard guard{std::thread([&listener, &stop_late, &entered, &reported_closed]() {
        entered.store(true);
        entered.notify_all();
        const auto accepted = listener.accept_one(&stop_late);
        reported_closed.store(!accepted.has_value() &&
                              accepted.error().code == ThermalErrorCode::TRANSPORT_CLOSED);
    })};
    entered.wait(false);
    stop_late.store(true);
    stop_late.notify_all();
    guard.join();
    TG_CHECK(reported_closed.load());
    TG_CHECK(listener.valid());

    TG_PHASE("a closed listener refuses accept, with and without a stop flag");
    listener.close();
    TG_CHECK(!listener.valid());
    TG_ERROR_CODE(listener.accept_one(), ThermalErrorCode::TRANSPORT_CLOSED);
    TG_ERROR_CODE(listener.accept_one(&stop_late), ThermalErrorCode::TRANSPORT_CLOSED);
}
