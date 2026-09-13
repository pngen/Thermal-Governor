// Thermal Governor — frame encoding and validation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/protocol.hpp"

#include <cstring>

#include "thermal_governor/persistence.hpp"
#include "thermal_governor/version.hpp"

namespace thermal_governor {
namespace {

void put_u16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
}

void put_u32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFU);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
}

[[nodiscard]] std::uint16_t get_u16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(in[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[1]) << 8);
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* in) {
    return static_cast<std::uint32_t>(in[0]) |
           (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) |
           (static_cast<std::uint32_t>(in[3]) << 24);
}

}  // namespace

bool is_known_message_kind(std::uint16_t raw) noexcept {
    switch (static_cast<MessageKind>(raw)) {
        case MessageKind::INVALID:
            return false;
        case MessageKind::HELLO:
        case MessageKind::HELLO_ACK:
        case MessageKind::REGISTER_WORKER:
        case MessageKind::REGISTER_ACK:
        case MessageKind::PUBLISH_EVIDENCE:
        case MessageKind::PUBLISH_ACK:
        case MessageKind::EVALUATE_REQUEST:
        case MessageKind::EVALUATE_RESPONSE:
        case MessageKind::DISPATCH_INTENT:
        case MessageKind::INTENT_ACK:
        case MessageKind::INTENT_RESULT:
        case MessageKind::SNAPSHOT_REQUEST:
        case MessageKind::SNAPSHOT_RESPONSE:
        case MessageKind::HEARTBEAT:
        case MessageKind::SHUTDOWN:
        case MessageKind::ERROR_RESPONSE:
        case MessageKind::RECOVERY_AUTHORIZE:
        case MessageKind::VERIFICATION_REPORT:
        case MessageKind::QUERY_REQUEST:
        case MessageKind::QUERY_RESPONSE:
        case MessageKind::DISCONNECT:
        case MessageKind::RESPONSE_END:
            return true;
    }
    return false;
}

Result<std::vector<std::uint8_t>> encode_frame(MessageKind kind, std::uint32_t flags,
                                               const std::uint8_t* payload,
                                               std::size_t payload_length) {
    if (!is_known_message_kind(static_cast<std::uint16_t>(kind))) {
        return ThermalError{ThermalErrorCode::INVALID_ARGUMENT, "cannot encode an unknown frame kind"};
    }
    if (payload_length > 0xFFFFFFFFULL) {
        return ThermalError{ThermalErrorCode::FRAME_TOO_LARGE, "payload length exceeds u32"};
    }
    if (payload_length != 0 && payload == nullptr) {
        return ThermalError{ThermalErrorCode::INVALID_ARGUMENT, "payload pointer is null"};
    }

    std::vector<std::uint8_t> frame(kFrameOverhead + payload_length, 0);

    // Header bytes [0, 16) are covered by header_crc; the checksum field at
    // [16, 20) is deliberately excluded so a checksum never covers itself.
    put_u32(frame.data() + 0, kFrameMagic);
    put_u16(frame.data() + 4, kProtocolVersion);
    put_u16(frame.data() + 6, static_cast<std::uint16_t>(kind));
    put_u32(frame.data() + 8, flags);
    put_u32(frame.data() + 12, static_cast<std::uint32_t>(payload_length));
    put_u32(frame.data() + 16, crc32c(frame.data(), 16));

    if (payload_length != 0) {
        std::memcpy(frame.data() + kFrameHeaderSize, payload, payload_length);
    }
    // Payload checksum covers exactly the payload bytes.
    const std::uint32_t payload_crc =
        crc32c(frame.data() + kFrameHeaderSize, payload_length, 0);
    put_u32(frame.data() + kFrameHeaderSize + payload_length, payload_crc);

    return frame;
}

Result<FrameHeader> decode_frame_header(const std::uint8_t* header, std::size_t length,
                                        const FrameLimits& limits) {
    if (header == nullptr) {
        return ThermalError{ThermalErrorCode::INVALID_ARGUMENT, "header pointer is null"};
    }
    if (length < kFrameHeaderSize) {
        return ThermalError{ThermalErrorCode::FRAME_TRUNCATED, "header is shorter than 20 bytes"};
    }
    if (get_u32(header) != kFrameMagic) {
        return ThermalError{ThermalErrorCode::FRAME_CORRUPT, "frame magic mismatch"};
    }
    const std::uint32_t stored_crc = get_u32(header + 16);
    const std::uint32_t computed_crc = crc32c(header, 16);
    if (stored_crc != computed_crc) {
        return ThermalError{ThermalErrorCode::FRAME_CORRUPT, "frame header checksum mismatch"};
    }

    FrameHeader out;
    out.version = get_u16(header + 4);
    if (out.version != kProtocolVersion) {
        return ThermalError{ThermalErrorCode::PROTOCOL_UNSUPPORTED,
                            "unsupported protocol version"};
    }
    const std::uint16_t raw_kind = get_u16(header + 6);
    if (!is_known_message_kind(raw_kind)) {
        return ThermalError{ThermalErrorCode::FRAME_CORRUPT, "unrecognised message kind"};
    }
    out.kind = static_cast<MessageKind>(raw_kind);
    out.flags = get_u32(header + 8);
    out.payload_length = get_u32(header + 12);
    if (static_cast<std::size_t>(out.payload_length) > limits.max_payload_bytes) {
        return ThermalError{ThermalErrorCode::FRAME_TOO_LARGE,
                            "declared payload length exceeds the configured bound"};
    }
    return out;
}

Result<Frame> decode_frame(const std::uint8_t* bytes, std::size_t length,
                           const FrameLimits& limits) {
    if (bytes == nullptr) {
        return ThermalError{ThermalErrorCode::INVALID_ARGUMENT, "frame pointer is null"};
    }
    if (length < kFrameOverhead) {
        return ThermalError{ThermalErrorCode::FRAME_TRUNCATED, "frame shorter than the overhead"};
    }
    auto header = decode_frame_header(bytes, length, limits);
    if (!header.has_value()) {
        return header.error();
    }

    const std::size_t expected =
        kFrameOverhead + static_cast<std::size_t>(header.value().payload_length);
    if (length < expected) {
        return ThermalError{ThermalErrorCode::FRAME_TRUNCATED, "frame payload truncated"};
    }
    if (length > expected) {
        return ThermalError{ThermalErrorCode::FRAME_CORRUPT,
                            "trailing bytes after the declared frame extent"};
    }

    const std::uint8_t* payload = bytes + kFrameHeaderSize;
    const std::uint32_t stored_crc = get_u32(payload + header.value().payload_length);
    const std::uint32_t computed_crc = crc32c(payload, header.value().payload_length, 0);
    if (stored_crc != computed_crc) {
        return ThermalError{ThermalErrorCode::FRAME_CORRUPT, "frame payload checksum mismatch"};
    }

    Frame frame;
    frame.kind = header.value().kind;
    frame.flags = header.value().flags;
    frame.payload.assign(payload, payload + header.value().payload_length);
    return frame;
}

}  // namespace thermal_governor
