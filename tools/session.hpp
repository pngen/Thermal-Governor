// Thermal Governor — shared helpers for the process tools.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_TOOLS_SESSION_HPP
#define THERMAL_GOVERNOR_TOOLS_SESSION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "thermal_governor/error.hpp"
#include "thermal_governor/protocol.hpp"
#include "thermal_governor/transport.hpp"

namespace thermal_governor::tools {

/// Encode bytes as lowercase hexadecimal. Used to carry opaque wire payloads
/// inside text command arguments without inventing a second binary channel.
[[nodiscard]] std::string to_hex(const std::uint8_t* data, std::size_t length);
[[nodiscard]] std::string to_hex(const std::vector<std::uint8_t>& data);
[[nodiscard]] bool from_hex(const std::string& text, std::vector<std::uint8_t>& out);

/// Send one frame built from a byte vector.
[[nodiscard]] Status send_bytes(TcpSocket& socket, MessageKind kind, std::uint32_t flags,
                                const std::vector<std::uint8_t>& payload);

/// Send a frame group terminator.
[[nodiscard]] Status send_end(TcpSocket& socket);

/// Read frames until RESPONSE_END, appending each non-terminal frame to
/// out_frames. Handles partial reads because TcpSocket::recv_frame does.
[[nodiscard]] Status read_group(TcpSocket& socket, std::vector<Frame>& out_frames,
                                const FrameLimits& limits = {});

/// Write a small text file atomically. Used for readiness signalling so a
/// test never has to poll with an arbitrary sleep.
[[nodiscard]] bool write_ready_file(const std::string& path, const std::string& content);

/// Parse an unsigned decimal argument. Returns false on any malformed input.
[[nodiscard]] bool parse_u64(const std::string& text, std::uint64_t& out);
[[nodiscard]] bool parse_double(const std::string& text, double& out);

/// Split a comma-separated list.
[[nodiscard]] std::vector<std::string> split(const std::string& text, char separator);

}  // namespace thermal_governor::tools

#endif  // THERMAL_GOVERNOR_TOOLS_SESSION_HPP
