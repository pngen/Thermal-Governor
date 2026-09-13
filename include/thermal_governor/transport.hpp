// Thermal Governor — framed TCP transport between independent processes.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_TRANSPORT_HPP
#define THERMAL_GOVERNOR_TRANSPORT_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "thermal_governor/error.hpp"
#include "thermal_governor/protocol.hpp"

namespace thermal_governor {

/// Owns the platform socket runtime lifetime (Winsock on Windows).
///
/// Startup is reference-counted and every live socket or listener holds
/// exactly one reference for its whole lifetime, so the runtime is torn
/// down only once the last one closes.
class SocketRuntime {
public:
    [[nodiscard]] static Status ensure() noexcept;
    [[nodiscard]] static void release() noexcept;
};

/// A connected TCP stream socket.
///
/// A send is never assumed to equal a receive: every transfer loops until
/// the requested extent is satisfied or the peer closes.
class TcpSocket {
public:
    TcpSocket() noexcept = default;
    ~TcpSocket();

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    [[nodiscard]] static Result<TcpSocket> connect_to(const std::string& host,
                                                      std::uint16_t port);

    [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidHandle; }

    [[nodiscard]] Status send_all(const std::uint8_t* data, std::size_t length);
    /// Returns the number of bytes read; 0 means the peer closed cleanly.
    [[nodiscard]] Result<std::size_t> recv_some(std::uint8_t* buffer, std::size_t length);

    /// Send one complete frame, handling partial writes.
    [[nodiscard]] Status send_frame(MessageKind kind, std::uint32_t flags,
                                    const std::uint8_t* payload, std::size_t payload_length);

    /// Receive exactly one frame, handling partial reads.
    [[nodiscard]] Result<Frame> recv_frame(const FrameLimits& limits = {});

    /// Half-close the outbound direction.
    void shutdown_send() noexcept;

    /// Close both directions without releasing the handle.
    ///
    /// This is the thread-safe way to unblock a reader that is already
    /// blocked in a receive on this socket, and it is how a server retires
    /// an idle session without leaking the session thread.
    void shutdown_both() noexcept;
    void close() noexcept;

    [[nodiscard]] std::string peer_description() const;

    [[nodiscard]] std::intptr_t native_handle() const noexcept { return handle_; }

    void set_no_delay(bool enabled) noexcept;

private:
    friend class TcpListener;
    static constexpr std::intptr_t kInvalidHandle = -1;

    explicit TcpSocket(std::intptr_t handle) noexcept : handle_(handle) {}

    std::intptr_t handle_ = kInvalidHandle;
};

/// A listening TCP socket bound to loopback.
class TcpListener {
public:
    TcpListener() noexcept = default;
    ~TcpListener();

    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    /// Bind to 127.0.0.1. Port 0 selects an ephemeral port, readable via
    /// bound_port().
    [[nodiscard]] static Result<TcpListener> bind_loopback(std::uint16_t port,
                                                           int backlog = 64);

    /// Wait for one connection, honouring a cooperative stop flag.
    ///
    /// The wait is implemented with a bounded select() poll so that
    /// shutdown is responsive without forced termination. It is not a test
    /// timeout and never aborts a running case.
    [[nodiscard]] Result<TcpSocket> accept_one(const std::atomic<bool>* stop_flag = nullptr);

    [[nodiscard]] std::uint16_t bound_port() const noexcept { return bound_port_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidHandle; }
    void close() noexcept;

private:
    static constexpr std::intptr_t kInvalidHandle = -1;
    std::intptr_t handle_ = kInvalidHandle;
    std::uint16_t bound_port_ = 0;
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_TRANSPORT_HPP
