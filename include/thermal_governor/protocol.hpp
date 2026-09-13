// Thermal Governor — framed coordinator/worker protocol.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_PROTOCOL_HPP
#define THERMAL_GOVERNOR_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "thermal_governor/error.hpp"
#include "thermal_governor/identity.hpp"

namespace thermal_governor {

/// Frame magic: ASCII "TGOV" little-endian on the wire.
inline constexpr std::uint32_t kFrameMagic = 0x564F4754U;

/// Fixed header size in bytes.
inline constexpr std::size_t kFrameHeaderSize = 20;
/// Trailing payload checksum size in bytes.
inline constexpr std::size_t kFrameTrailerSize = 4;
/// Total non-payload overhead per frame.
inline constexpr std::size_t kFrameOverhead = kFrameHeaderSize + kFrameTrailerSize;

/// Message kinds carried in the frame header.
enum class MessageKind : std::uint16_t {
    INVALID = 0,
    HELLO = 1,
    HELLO_ACK = 2,
    REGISTER_WORKER = 3,
    REGISTER_ACK = 4,
    PUBLISH_EVIDENCE = 5,
    PUBLISH_ACK = 6,
    EVALUATE_REQUEST = 7,
    EVALUATE_RESPONSE = 8,
    DISPATCH_INTENT = 9,
    INTENT_ACK = 10,
    INTENT_RESULT = 11,
    SNAPSHOT_REQUEST = 12,
    SNAPSHOT_RESPONSE = 13,
    HEARTBEAT = 14,
    SHUTDOWN = 15,
    ERROR_RESPONSE = 16,
    RECOVERY_AUTHORIZE = 17,
    VERIFICATION_REPORT = 18,
    QUERY_REQUEST = 19,
    QUERY_RESPONSE = 20,
    DISCONNECT = 21,
    /// Terminates a multi-frame response group. A reader consumes frames
    /// until it observes this kind, so no reader ever assumes that one send
    /// equals one receive.
    RESPONSE_END = 22,
};

[[nodiscard]] constexpr std::string_view to_string(MessageKind k) noexcept {
    switch (k) {
        case MessageKind::INVALID: return "INVALID";
        case MessageKind::HELLO: return "HELLO";
        case MessageKind::HELLO_ACK: return "HELLO_ACK";
        case MessageKind::REGISTER_WORKER: return "REGISTER_WORKER";
        case MessageKind::REGISTER_ACK: return "REGISTER_ACK";
        case MessageKind::PUBLISH_EVIDENCE: return "PUBLISH_EVIDENCE";
        case MessageKind::PUBLISH_ACK: return "PUBLISH_ACK";
        case MessageKind::EVALUATE_REQUEST: return "EVALUATE_REQUEST";
        case MessageKind::EVALUATE_RESPONSE: return "EVALUATE_RESPONSE";
        case MessageKind::DISPATCH_INTENT: return "DISPATCH_INTENT";
        case MessageKind::INTENT_ACK: return "INTENT_ACK";
        case MessageKind::INTENT_RESULT: return "INTENT_RESULT";
        case MessageKind::SNAPSHOT_REQUEST: return "SNAPSHOT_REQUEST";
        case MessageKind::SNAPSHOT_RESPONSE: return "SNAPSHOT_RESPONSE";
        case MessageKind::HEARTBEAT: return "HEARTBEAT";
        case MessageKind::SHUTDOWN: return "SHUTDOWN";
        case MessageKind::ERROR_RESPONSE: return "ERROR_RESPONSE";
        case MessageKind::RECOVERY_AUTHORIZE: return "RECOVERY_AUTHORIZE";
        case MessageKind::VERIFICATION_REPORT: return "VERIFICATION_REPORT";
        case MessageKind::QUERY_REQUEST: return "QUERY_REQUEST";
        case MessageKind::QUERY_RESPONSE: return "QUERY_RESPONSE";
        case MessageKind::DISCONNECT: return "DISCONNECT";
        case MessageKind::RESPONSE_END: return "RESPONSE_END";
    }
    return "UNRECOGNISED_MESSAGE_KIND";
}

[[nodiscard]] bool is_known_message_kind(std::uint16_t raw) noexcept;

/// Bounds applied before any payload allocation.
struct FrameLimits {
    std::size_t max_payload_bytes = 4U * 1024U * 1024U;
};

/// A validated, decoded frame.
struct Frame {
    MessageKind kind = MessageKind::INVALID;
    std::uint32_t flags = 0;
    std::vector<std::uint8_t> payload;
};

/// Encode a frame.
///
/// Checksum coverage is exact and audited:
///   header_crc  = CRC32C(header bytes [0, 16))
///   payload_crc = CRC32C(payload bytes [0, payload_length))
/// Neither checksum covers itself, and the header checksum does not cover
/// the payload (which has its own).
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_frame(MessageKind kind,
                                                             std::uint32_t flags,
                                                             const std::uint8_t* payload,
                                                             std::size_t payload_length);

/// Validate a complete in-memory byte sequence as exactly one frame.
/// Rejects bad magic, unsupported version, truncation, oversized payload,
/// checksum mismatch and impossible lengths.
[[nodiscard]] Result<Frame> decode_frame(const std::uint8_t* bytes, std::size_t length,
                                         const FrameLimits& limits = {});

/// Decode only the fixed header, returning the declared payload length.
/// Used by streaming readers so they can size the next read before
/// allocating.
struct FrameHeader {
    std::uint16_t version = 0;
    MessageKind kind = MessageKind::INVALID;
    std::uint32_t flags = 0;
    std::uint32_t payload_length = 0;
};

[[nodiscard]] Result<FrameHeader> decode_frame_header(const std::uint8_t* header,
                                                      std::size_t length,
                                                      const FrameLimits& limits = {});

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_PROTOCOL_HPP
