// Thermal Governor — framed protocol tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "framework.hpp"

#include "thermal_governor/error.hpp"
#include "thermal_governor/persistence.hpp"
#include "thermal_governor/protocol.hpp"
#include "thermal_governor/version.hpp"

using thermal_governor::FrameLimits;
using thermal_governor::MessageKind;
using thermal_governor::ThermalErrorCode;
using thermal_governor::crc32c;
using thermal_governor::decode_frame;
using thermal_governor::decode_frame_header;
using thermal_governor::encode_frame;
using thermal_governor::is_known_message_kind;
using thermal_governor::kFrameHeaderSize;
using thermal_governor::kFrameOverhead;
using thermal_governor::kProtocolVersion;

namespace {

/// Offsets inside the fixed 20 byte header.
constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kKindOffset = 6;
constexpr std::size_t kFlagsOffset = 8;
constexpr std::size_t kLengthOffset = 12;
constexpr std::size_t kHeaderChecksumOffset = 16;

void put_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::uint32_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU);
    }
}

[[nodiscard]] std::uint32_t get_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::uint32_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (8U * index);
    }
    return value;
}

/// Repair the header checksum after a deliberate header mutation so the
/// mutation under test is the one that fails, not the checksum.
void repair_header_checksum(std::vector<std::uint8_t>& frame) {
    put_u32(frame, kHeaderChecksumOffset, crc32c(frame.data(), kHeaderChecksumOffset));
}

[[nodiscard]] std::vector<std::uint8_t> payload_of(std::size_t length, std::uint8_t seed) {
    std::vector<std::uint8_t> payload(length);
    for (std::size_t index = 0; index < length; ++index) {
        payload[index] = static_cast<std::uint8_t>((seed + index * 7U) & 0xFFU);
    }
    return payload;
}

[[nodiscard]] std::vector<std::uint8_t> frame_of(MessageKind kind, std::uint32_t flags,
                                                 const std::vector<std::uint8_t>& payload) {
    const auto encoded = encode_frame(kind, flags, payload.empty() ? nullptr : payload.data(),
                                      payload.size());
    return encoded.has_value() ? encoded.value() : std::vector<std::uint8_t>{};
}

}  // namespace

TG_CASE(protocol, round_trip_every_message_kind) {
    const std::vector<std::uint8_t> payload = payload_of(64, 3);
    TG_CHECK(!payload.empty());

    for (std::uint16_t raw = 1; raw <= 22; ++raw) {
        TG_CHECK(is_known_message_kind(raw));
        const auto kind = static_cast<MessageKind>(raw);

        const auto encoded = encode_frame(kind, 0x12345678U, payload.data(), payload.size());
        TG_CHECK(encoded.has_value());
        TG_CHECK_EQ(encoded.value().size(), kFrameOverhead + payload.size());

        const auto decoded = decode_frame(encoded.value().data(), encoded.value().size());
        TG_CHECK(decoded.has_value());
        TG_CHECK(decoded.value().kind == kind);
        TG_CHECK_EQ(decoded.value().flags, 0x12345678U);
        TG_CHECK(decoded.value().payload == payload);
    }

    TG_PHASE("the invalid kind and unknown kinds are never known");
    TG_CHECK(!is_known_message_kind(std::uint16_t{0}));
    TG_CHECK(!is_known_message_kind(std::uint16_t{23}));
    TG_CHECK(!is_known_message_kind(std::uint16_t{0xFFFF}));
}

TG_CASE(protocol, round_trip_empty_payload) {
    const auto encoded = encode_frame(MessageKind::HEARTBEAT, 0, nullptr, 0);
    TG_CHECK(encoded.has_value());
    TG_CHECK_EQ(encoded.value().size(), kFrameOverhead);

    const auto decoded = decode_frame(encoded.value().data(), encoded.value().size());
    TG_CHECK(decoded.has_value());
    TG_CHECK(decoded.value().kind == MessageKind::HEARTBEAT);
    TG_CHECK(decoded.value().payload.empty());
    TG_CHECK_EQ(decoded.value().flags, 0U);
}

TG_CASE(protocol, encode_rejects_unknown_kind_and_null_payload) {
    const std::vector<std::uint8_t> payload = payload_of(4, 1);
    TG_ERROR_CODE(encode_frame(MessageKind::INVALID, 0, payload.data(), payload.size()),
                  ThermalErrorCode::INVALID_ARGUMENT);
    TG_ERROR_CODE(encode_frame(static_cast<MessageKind>(99), 0, payload.data(), payload.size()),
                  ThermalErrorCode::INVALID_ARGUMENT);
    TG_ERROR_CODE(encode_frame(MessageKind::HELLO, 0, nullptr, 8),
                  ThermalErrorCode::INVALID_ARGUMENT);
    TG_OK(encode_frame(MessageKind::HELLO, 0, nullptr, 0));
}

TG_CASE(protocol, decode_rejects_bad_magic) {
    std::vector<std::uint8_t> frame = frame_of(MessageKind::HELLO, 0, payload_of(8, 2));
    TG_CHECK(!frame.empty());

    frame[kMagicOffset] ^= 0xFFU;
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()), ThermalErrorCode::FRAME_CORRUPT);
    TG_ERROR_CODE(decode_frame_header(frame.data(), kFrameHeaderSize),
                  ThermalErrorCode::FRAME_CORRUPT);

    TG_PHASE("null buffers are refused outright");
    TG_ERROR_CODE(decode_frame(nullptr, 64), ThermalErrorCode::INVALID_ARGUMENT);
    TG_ERROR_CODE(decode_frame_header(nullptr, kFrameHeaderSize),
                  ThermalErrorCode::INVALID_ARGUMENT);
}

TG_CASE(protocol, decode_rejects_unsupported_version) {
    std::vector<std::uint8_t> frame = frame_of(MessageKind::HELLO, 0, payload_of(8, 2));
    TG_CHECK(!frame.empty());

    put_u16(frame, kVersionOffset, static_cast<std::uint16_t>(kProtocolVersion + 1));
    repair_header_checksum(frame);
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()),
                  ThermalErrorCode::PROTOCOL_UNSUPPORTED);
    TG_ERROR_CODE(decode_frame_header(frame.data(), kFrameHeaderSize),
                  ThermalErrorCode::PROTOCOL_UNSUPPORTED);

    put_u16(frame, kVersionOffset, std::uint16_t{0});
    repair_header_checksum(frame);
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()),
                  ThermalErrorCode::PROTOCOL_UNSUPPORTED);
}

TG_CASE(protocol, decode_rejects_unknown_message_kind) {
    std::vector<std::uint8_t> frame = frame_of(MessageKind::HELLO, 0, payload_of(8, 2));
    TG_CHECK(!frame.empty());

    put_u16(frame, kKindOffset, std::uint16_t{0});
    repair_header_checksum(frame);
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()), ThermalErrorCode::FRAME_CORRUPT);

    put_u16(frame, kKindOffset, std::uint16_t{99});
    repair_header_checksum(frame);
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()), ThermalErrorCode::FRAME_CORRUPT);
    TG_ERROR_CODE(decode_frame_header(frame.data(), kFrameHeaderSize),
                  ThermalErrorCode::FRAME_CORRUPT);
}

TG_CASE(protocol, decode_rejects_truncated_header) {
    const std::vector<std::uint8_t> frame = frame_of(MessageKind::HELLO, 0, payload_of(8, 2));
    TG_CHECK(frame.size() > kFrameOverhead);

    TG_ERROR_CODE(decode_frame(frame.data(), kFrameOverhead - 1), ThermalErrorCode::FRAME_TRUNCATED);
    TG_ERROR_CODE(decode_frame(frame.data(), 1), ThermalErrorCode::FRAME_TRUNCATED);
    TG_ERROR_CODE(decode_frame(frame.data(), 0), ThermalErrorCode::FRAME_TRUNCATED);
    TG_ERROR_CODE(decode_frame_header(frame.data(), kFrameHeaderSize - 1),
                  ThermalErrorCode::FRAME_TRUNCATED);
    TG_ERROR_CODE(decode_frame_header(frame.data(), 0), ThermalErrorCode::FRAME_TRUNCATED);

    TG_PHASE("exactly the overhead is accepted as a header");
    TG_CHECK(decode_frame_header(frame.data(), kFrameHeaderSize).has_value());
}

TG_CASE(protocol, decode_rejects_truncated_payload) {
    const std::vector<std::uint8_t> payload = payload_of(64, 4);
    const std::vector<std::uint8_t> frame = frame_of(MessageKind::PUBLISH_EVIDENCE, 5, payload);
    TG_CHECK_EQ(frame.size(), kFrameOverhead + payload.size());

    TG_ERROR_CODE(decode_frame(frame.data(), frame.size() - 1), ThermalErrorCode::FRAME_TRUNCATED);
    TG_ERROR_CODE(decode_frame(frame.data(), kFrameOverhead), ThermalErrorCode::FRAME_TRUNCATED);
    TG_ERROR_CODE(decode_frame(frame.data(), kFrameHeaderSize), ThermalErrorCode::FRAME_TRUNCATED);

    TG_PHASE("the trailer alone is not a frame");
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size() - kFrameOverhead),
                  ThermalErrorCode::FRAME_TRUNCATED);
}

TG_CASE(protocol, decode_rejects_trailing_bytes) {
    const std::vector<std::uint8_t> payload = payload_of(16, 6);
    std::vector<std::uint8_t> frame = frame_of(MessageKind::HEARTBEAT, 0, payload);
    TG_CHECK(!frame.empty());
    TG_OK(decode_frame(frame.data(), frame.size()));

    TG_PHASE("one unrelated trailing byte is rejected");
    std::vector<std::uint8_t> trailing = frame;
    trailing.push_back(0x5AU);
    TG_ERROR_CODE(decode_frame(trailing.data(), trailing.size()),
                  ThermalErrorCode::FRAME_CORRUPT);

    TG_PHASE("a whole second frame appended is rejected as trailing data");
    std::vector<std::uint8_t> second = frame;
    second.insert(second.end(), frame.begin(), frame.end());
    TG_ERROR_CODE(decode_frame(second.data(), second.size()), ThermalErrorCode::FRAME_CORRUPT);

    TG_PHASE("exactly one frame is accepted");
    TG_CHECK(decode_frame(frame.data(), frame.size()).has_value());
}

TG_CASE(protocol, decode_rejects_oversized_declared_payload) {
    std::vector<std::uint8_t> frame = frame_of(MessageKind::PUBLISH_EVIDENCE, 0, payload_of(8, 7));
    TG_CHECK(!frame.empty());

    TG_PHASE("a declared payload beyond the default bound is refused");
    put_u32(frame, kLengthOffset, 5U * 1024U * 1024U);
    repair_header_checksum(frame);
    TG_ERROR_CODE(decode_frame_header(frame.data(), kFrameHeaderSize),
                  ThermalErrorCode::FRAME_TOO_LARGE);
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()), ThermalErrorCode::FRAME_TOO_LARGE);

    TG_PHASE("a tighter configured bound is honoured");
    FrameLimits limits;
    limits.max_payload_bytes = 16;
    const std::vector<std::uint8_t> small = frame_of(MessageKind::PUBLISH_EVIDENCE, 0, payload_of(64, 8));
    TG_CHECK(!small.empty());
    TG_ERROR_CODE(decode_frame(small.data(), small.size(), limits),
                  ThermalErrorCode::FRAME_TOO_LARGE);
    TG_ERROR_CODE(decode_frame_header(small.data(), kFrameHeaderSize, limits),
                  ThermalErrorCode::FRAME_TOO_LARGE);

    TG_PHASE("the same frame passes under the default bound");
    TG_OK(decode_frame(small.data(), small.size()));
}

TG_CASE(protocol, decode_rejects_corrupted_header_checksum) {
    TG_PHASE("a flipped byte inside the checksum field itself");
    std::vector<std::uint8_t> frame = frame_of(MessageKind::HELLO, 7, payload_of(8, 9));
    TG_CHECK(!frame.empty());
    frame[kHeaderChecksumOffset] ^= 0xFFU;
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()), ThermalErrorCode::FRAME_CORRUPT);
    TG_ERROR_CODE(decode_frame_header(frame.data(), kFrameHeaderSize),
                  ThermalErrorCode::FRAME_CORRUPT);

    TG_PHASE("a flipped byte in the covered header region");
    frame = frame_of(MessageKind::HELLO, 7, payload_of(8, 9));
    frame[kFlagsOffset] ^= 0x01U;
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()), ThermalErrorCode::FRAME_CORRUPT);

    frame = frame_of(MessageKind::HELLO, 7, payload_of(8, 9));
    frame[kLengthOffset] ^= 0x01U;
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()), ThermalErrorCode::FRAME_CORRUPT);

    TG_PHASE("repairing the checksum restores decodability");
    frame = frame_of(MessageKind::HELLO, 7, payload_of(8, 9));
    frame[kFlagsOffset] ^= 0x01U;
    repair_header_checksum(frame);
    TG_OK(decode_frame(frame.data(), frame.size()));
}

TG_CASE(protocol, decode_rejects_corrupted_payload_checksum) {
    const std::vector<std::uint8_t> payload = payload_of(48, 10);

    TG_PHASE("a flipped payload byte");
    std::vector<std::uint8_t> frame = frame_of(MessageKind::PUBLISH_EVIDENCE, 3, payload);
    TG_CHECK(!frame.empty());
    frame[kFrameHeaderSize] ^= 0xFFU;
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()), ThermalErrorCode::FRAME_CORRUPT);

    TG_PHASE("a flipped byte in the trailing checksum field itself");
    frame = frame_of(MessageKind::PUBLISH_EVIDENCE, 3, payload);
    frame[kFrameHeaderSize + payload.size()] ^= 0xFFU;
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()), ThermalErrorCode::FRAME_CORRUPT);

    TG_PHASE("the untouched frame still decodes");
    frame = frame_of(MessageKind::PUBLISH_EVIDENCE, 3, payload);
    const auto decoded = decode_frame(frame.data(), frame.size());
    TG_CHECK(decoded.has_value());
    TG_CHECK(decoded.value().payload == payload);
}

TG_CASE(protocol, checksum_coverage_is_explicit) {
    const std::vector<std::uint8_t> payload = payload_of(32, 11);

    TG_PHASE("corrupting the payload leaves the header checksum valid");
    std::vector<std::uint8_t> frame = frame_of(MessageKind::VERIFICATION_REPORT, 9, payload);
    TG_CHECK(!frame.empty());
    frame[kFrameHeaderSize] ^= 0xFFU;
    TG_CHECK(decode_frame_header(frame.data(), kFrameHeaderSize).has_value());
    TG_CHECK_EQ(get_u32(frame, kHeaderChecksumOffset), crc32c(frame.data(), kHeaderChecksumOffset));
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()), ThermalErrorCode::FRAME_CORRUPT);

    TG_PHASE("the header checksum does not cover the payload checksum");
    frame = frame_of(MessageKind::VERIFICATION_REPORT, 9, payload);
    frame[kFrameHeaderSize + payload.size()] ^= 0xFFU;
    TG_CHECK(decode_frame_header(frame.data(), kFrameHeaderSize).has_value());
    TG_ERROR_CODE(decode_frame(frame.data(), frame.size()), ThermalErrorCode::FRAME_CORRUPT);

    TG_PHASE("the payload checksum does not cover the header");
    std::vector<std::uint8_t> mutated = frame_of(MessageKind::VERIFICATION_REPORT, 9, payload);
    mutated[kFlagsOffset] ^= 0x01U;
    TG_ERROR_CODE(decode_frame(mutated.data(), mutated.size()),
                  ThermalErrorCode::FRAME_CORRUPT);
    mutated[kFlagsOffset] ^= 0x01U;
    repair_header_checksum(mutated);
    const auto recovered = decode_frame(mutated.data(), mutated.size());
    TG_CHECK(recovered.has_value());
    TG_CHECK(recovered.value().payload == payload);
    TG_CHECK_EQ(recovered.value().flags, 9U);
}

TG_CASE(protocol, decode_frame_header_works_on_a_twenty_byte_prefix) {
    const std::vector<std::uint8_t> payload = payload_of(128, 12);
    const std::vector<std::uint8_t> frame = frame_of(MessageKind::EVALUATE_RESPONSE, 0xABCDU, payload);
    TG_CHECK(frame.size() > kFrameHeaderSize);

    const auto header = decode_frame_header(frame.data(), kFrameHeaderSize);
    TG_CHECK(header.has_value());
    TG_CHECK_EQ(header.value().version, kProtocolVersion);
    TG_CHECK(header.value().kind == MessageKind::EVALUATE_RESPONSE);
    TG_CHECK_EQ(header.value().flags, 0xABCDU);
    TG_CHECK_EQ(header.value().payload_length, 128U);

    TG_PHASE("only the 20 byte prefix is required");
    const std::vector<std::uint8_t> prefix(frame.begin(),
                                           frame.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderSize));
    TG_CHECK_EQ(prefix.size(), kFrameHeaderSize);
    TG_CHECK(decode_frame_header(prefix.data(), prefix.size()).has_value());

    TG_PHASE("a full frame is required to decode the payload");
    TG_ERROR_CODE(decode_frame(prefix.data(), prefix.size()), ThermalErrorCode::FRAME_TRUNCATED);

    TG_PHASE("the header alone reports the exact payload length to read next");
    const std::size_t total = kFrameOverhead + header.value().payload_length;
    TG_CHECK_EQ(total, frame.size());
}
