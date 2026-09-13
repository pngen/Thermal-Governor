// Thermal Governor — shared helpers for the process tools.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "session.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace thermal_governor::tools {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::string to_hex(const std::uint8_t* data, std::size_t length) {
    std::string out;
    out.reserve(length * 2);
    for (std::size_t i = 0; i < length; ++i) {
        out.push_back(kHexDigits[(data[i] >> 4) & 0x0FU]);
        out.push_back(kHexDigits[data[i] & 0x0FU]);
    }
    return out;
}

std::string to_hex(const std::vector<std::uint8_t>& data) {
    return to_hex(data.data(), data.size());
}

bool from_hex(const std::string& text, std::vector<std::uint8_t>& out) {
    if (text.size() % 2 != 0) {
        return false;
    }
    out.clear();
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int high = hex_value(text[i]);
        const int low = hex_value(text[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return true;
}

Status send_bytes(TcpSocket& socket, MessageKind kind, std::uint32_t flags,
                  const std::vector<std::uint8_t>& payload) {
    return socket.send_frame(kind, flags, payload.data(), payload.size());
}

Status send_end(TcpSocket& socket) { return socket.send_frame(MessageKind::RESPONSE_END, 0, nullptr, 0); }

Status read_group(TcpSocket& socket, std::vector<Frame>& out_frames, const FrameLimits& limits) {
    for (;;) {
        auto frame = socket.recv_frame(limits);
        if (!frame.has_value()) {
            return Status(frame.error());
        }
        if (frame.value().kind == MessageKind::RESPONSE_END) {
            return Status::success();
        }
        out_frames.push_back(std::move(frame.value()));
    }
}

bool write_ready_file(const std::string& path, const std::string& content) {
    if (path.empty()) {
        return true;
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream << content;
    stream.flush();
    return stream.good();
}

bool parse_u64(const std::string& text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0') {
        return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
}

bool parse_double(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == nullptr || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == separator) {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    out.push_back(current);
    return out;
}

}  // namespace thermal_governor::tools
