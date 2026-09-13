// Thermal Governor — framed TCP transport.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/transport.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace thermal_governor {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalid = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalid = -1;
#endif

std::mutex g_socket_mutex;
int g_socket_users = 0;
bool g_socket_ready = false;

[[nodiscard]] NativeSocket to_native(std::intptr_t handle) {
    return static_cast<NativeSocket>(handle);
}

[[nodiscard]] std::intptr_t to_handle(NativeSocket socket) {
    return static_cast<std::intptr_t>(socket);
}

void close_native(NativeSocket socket) {
    if (socket == kInvalid) {
        return;
    }
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

[[nodiscard]] ThermalError last_socket_error(const char* what) {
#ifdef _WIN32
    const int code = ::WSAGetLastError();
    return ThermalError{ThermalErrorCode::TRANSPORT_IO,
                        std::string(what) + " (winsock error " + std::to_string(code) + ")"};
#else
    return ThermalError{ThermalErrorCode::TRANSPORT_IO,
                        std::string(what) + " (errno " + std::to_string(errno) + ")"};
#endif
}

}  // namespace

// --- SocketRuntime --------------------------------------------------------

Status SocketRuntime::ensure() noexcept {
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    if (g_socket_users == 0) {
#ifdef _WIN32
        WSADATA data{};
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) {
            return Status::failure(ThermalErrorCode::TRANSPORT_IO,
                                   "WSAStartup failed with code " + std::to_string(rc));
        }
#endif
        g_socket_ready = true;
    }
    ++g_socket_users;
    return Status::success();
}

void SocketRuntime::release() noexcept {
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    if (g_socket_users == 0) {
        return;
    }
    --g_socket_users;
    if (g_socket_users == 0 && g_socket_ready) {
#ifdef _WIN32
        ::WSACleanup();
#endif
        g_socket_ready = false;
    }
}

// --- TcpSocket ------------------------------------------------------------

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalidHandle;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalidHandle;
    }
    return *this;
}

Result<TcpSocket> TcpSocket::connect_to(const std::string& host, std::uint16_t port) {
    auto startup = SocketRuntime::ensure();
    if (!startup.ok()) {
        return startup.error();
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* results = nullptr;
    const std::string port_text = std::to_string(port);
    const int rc = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
    if (rc != 0 || results == nullptr) {
        SocketRuntime::release();
        return ThermalError{ThermalErrorCode::TRANSPORT_IO, "name resolution failed for " + host};
    }

    NativeSocket socket = kInvalid;
    for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
        socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (socket == kInvalid) {
            continue;
        }
        if (::connect(socket, candidate->ai_addr,
                      static_cast<int>(candidate->ai_addrlen)) == 0) {
            break;
        }
        close_native(socket);
        socket = kInvalid;
    }
    ::freeaddrinfo(results);

    if (socket == kInvalid) {
        SocketRuntime::release();
        return ThermalError{ThermalErrorCode::TRANSPORT_IO,
                            "connection refused by " + host + ":" + port_text};
    }

    TcpSocket out(to_handle(socket));
    out.set_no_delay(true);
    return out;
}

void TcpSocket::set_no_delay(bool enabled) noexcept {
    if (handle_ == kInvalidHandle) {
        return;
    }
    const int value = enabled ? 1 : 0;
    ::setsockopt(to_native(handle_), IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&value), sizeof(value));
}

Status TcpSocket::send_all(const std::uint8_t* data, std::size_t length) {
    if (handle_ == kInvalidHandle) {
        return Status::failure(ThermalErrorCode::TRANSPORT_CLOSED, "socket is not connected");
    }
    if (length == 0) {
        return Status::success();
    }
    if (data == nullptr) {
        return Status::failure(ThermalErrorCode::INVALID_ARGUMENT, "null send buffer");
    }

    std::size_t sent = 0;
    while (sent < length) {
        const std::size_t remaining = length - sent;
        const int chunk = static_cast<int>(remaining > 1U << 20 ? 1U << 20 : remaining);
        const int rc = ::send(to_native(handle_), reinterpret_cast<const char*>(data + sent),
                              chunk, 0);
        if (rc <= 0) {
            return Status(last_socket_error("send failed"));
        }
        sent += static_cast<std::size_t>(rc);
    }
    return Status::success();
}

Result<std::size_t> TcpSocket::recv_some(std::uint8_t* buffer, std::size_t length) {
    if (handle_ == kInvalidHandle) {
        return ThermalError{ThermalErrorCode::TRANSPORT_CLOSED, "socket is not connected"};
    }
    if (length == 0) {
        return std::size_t{0};
    }
    const int chunk = static_cast<int>(length > 1U << 20 ? 1U << 20 : length);
    const int rc = ::recv(to_native(handle_), reinterpret_cast<char*>(buffer), chunk, 0);
    if (rc == 0) {
        return std::size_t{0};
    }
    if (rc < 0) {
        return last_socket_error("receive failed");
    }
    return static_cast<std::size_t>(rc);
}

Status TcpSocket::send_frame(MessageKind kind, std::uint32_t flags, const std::uint8_t* payload,
                             std::size_t payload_length) {
    auto encoded = encode_frame(kind, flags, payload, payload_length);
    if (!encoded.has_value()) {
        return Status(encoded.error());
    }
    return send_all(encoded.value().data(), encoded.value().size());
}

Result<Frame> TcpSocket::recv_frame(const FrameLimits& limits) {
    std::vector<std::uint8_t> buffer;
    buffer.reserve(kFrameOverhead + 4096);

    // The header is read exactly, so a partial read can never be mistaken
    // for a complete frame.
    std::size_t header_filled = 0;
    buffer.resize(kFrameHeaderSize);
    while (header_filled < kFrameHeaderSize) {
        auto received = recv_some(buffer.data() + header_filled, kFrameHeaderSize - header_filled);
        if (!received.has_value()) {
            return received.error();
        }
        if (received.value() == 0) {
            return ThermalError{ThermalErrorCode::TRANSPORT_CLOSED,
                                "peer closed while the frame header was in flight"};
        }
        header_filled += received.value();
    }

    auto header = decode_frame_header(buffer.data(), kFrameHeaderSize, limits);
    if (!header.has_value()) {
        return header.error();
    }

    const std::size_t total =
        kFrameOverhead + static_cast<std::size_t>(header.value().payload_length);
    buffer.resize(total);
    std::size_t filled = kFrameHeaderSize;
    while (filled < total) {
        auto received = recv_some(buffer.data() + filled, total - filled);
        if (!received.has_value()) {
            return received.error();
        }
        if (received.value() == 0) {
            return ThermalError{ThermalErrorCode::FRAME_TRUNCATED,
                                "peer closed while the frame payload was in flight"};
        }
        filled += received.value();
    }

    return decode_frame(buffer.data(), buffer.size(), limits);
}

void TcpSocket::shutdown_send() noexcept {
    if (handle_ == kInvalidHandle) {
        return;
    }
#ifdef _WIN32
    ::shutdown(to_native(handle_), SD_SEND);
#else
    ::shutdown(to_native(handle_), SHUT_WR);
#endif
}

void TcpSocket::shutdown_both() noexcept {
    if (handle_ == kInvalidHandle) {
        return;
    }
#ifdef _WIN32
    ::shutdown(to_native(handle_), SD_BOTH);
#else
    ::shutdown(to_native(handle_), SHUT_RDWR);
#endif
}

void TcpSocket::close() noexcept {
    if (handle_ == kInvalidHandle) {
        return;
    }
    close_native(to_native(handle_));
    handle_ = kInvalidHandle;
    SocketRuntime::release();
}

std::string TcpSocket::peer_description() const {
    if (handle_ == kInvalidHandle) {
        return "<closed>";
    }
    sockaddr_storage storage{};
    int length = sizeof(storage);
    if (::getpeername(to_native(handle_), reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        return "<unknown>";
    }
    char host[64] = {};
    char service[16] = {};
    if (::getnameinfo(reinterpret_cast<sockaddr*>(&storage), length, host, sizeof(host), service,
                      sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "<unknown>";
    }
    return std::string(host) + ":" + service;
}

// --- TcpListener ----------------------------------------------------------

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_), bound_port_(other.bound_port_) {
    other.handle_ = kInvalidHandle;
    other.bound_port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        bound_port_ = other.bound_port_;
        other.handle_ = kInvalidHandle;
        other.bound_port_ = 0;
    }
    return *this;
}

Result<TcpListener> TcpListener::bind_loopback(std::uint16_t port, int backlog) {
    auto startup = SocketRuntime::ensure();
    if (!startup.ok()) {
        return startup.error();
    }

    NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalid) {
        SocketRuntime::release();
        return last_socket_error("listener socket creation failed");
    }

    const int reuse = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    address.sin_port = ::htons(port);

    if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        auto error = last_socket_error("listener bind failed");
        close_native(socket);
        SocketRuntime::release();
        return error;
    }
    if (::listen(socket, backlog) != 0) {
        auto error = last_socket_error("listener listen failed");
        close_native(socket);
        SocketRuntime::release();
        return error;
    }

    sockaddr_in bound{};
    int bound_length = sizeof(bound);
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &bound_length) != 0) {
        auto error = last_socket_error("listener getsockname failed");
        close_native(socket);
        SocketRuntime::release();
        return error;
    }

    TcpListener listener;
    listener.handle_ = to_handle(socket);
    listener.bound_port_ = ::ntohs(bound.sin_port);
    return listener;
}

Result<TcpSocket> TcpListener::accept_one(const std::atomic<bool>* stop_flag) {
    if (handle_ == kInvalidHandle) {
        return ThermalError{ThermalErrorCode::TRANSPORT_CLOSED, "listener is closed"};
    }

    for (;;) {
        if (stop_flag != nullptr && stop_flag->load()) {
            return ThermalError{ThermalErrorCode::TRANSPORT_CLOSED,
                                "listener stopped by the cooperative stop flag"};
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(to_native(handle_), &read_set);

        timeval wait{};
        // Bounded poll purely so a cooperative stop flag is observed
        // promptly. This is not a request timeout and never cancels a
        // running operation.
        wait.tv_sec = 0;
        wait.tv_usec = 50000;

        const int ready = ::select(0, &read_set, nullptr, nullptr, &wait);
        if (ready < 0) {
            return last_socket_error("listener select failed");
        }
        if (ready == 0) {
            continue;
        }

        sockaddr_storage peer{};
        int peer_length = sizeof(peer);
        NativeSocket accepted =
            ::accept(to_native(handle_), reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (accepted == kInvalid) {
            return last_socket_error("listener accept failed");
        }
        // Adopting a connected handle must take its own reference to the
        // socket runtime, because TcpSocket::close() releases one. Without
        // this the very first connection to close would drop the runtime to
        // zero users and tear Winsock down underneath the still-open
        // listener, invalidating it and ending the server after one client.
        auto startup = SocketRuntime::ensure();
        if (!startup.ok()) {
            close_native(accepted);
            return startup.error();
        }
        TcpSocket out(to_handle(accepted));
        out.set_no_delay(true);
        return out;
    }
}

void TcpListener::close() noexcept {
    if (handle_ == kInvalidHandle) {
        return;
    }
    close_native(to_native(handle_));
    handle_ = kInvalidHandle;
    bound_port_ = 0;
    SocketRuntime::release();
}

}  // namespace thermal_governor
