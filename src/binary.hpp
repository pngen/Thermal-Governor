// Thermal Governor — shared bounds-checked binary encoding primitives.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_SRC_BINARY_HPP
#define THERMAL_GOVERNOR_SRC_BINARY_HPP

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace thermal_governor::detail {

/// Append-only little-endian writer with length-prefixed strings.
class BinaryWriter {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u16(std::uint16_t value) {
        bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
    }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }
    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    }
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }
    void real(double value) {
        std::uint64_t bits_value = 0;
        static_assert(sizeof(bits_value) == sizeof(value), "double must be 64-bit");
        std::memcpy(&bits_value, &value, sizeof(value));
        u64(bits_value);
    }
    void text(const std::string& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void bytes(const std::vector<std::uint8_t>& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    template <class T, class Encode>
    void optional(const std::optional<T>& value, Encode encode) {
        u8(value.has_value() ? 1U : 0U);
        if (value.has_value()) {
            encode(*value);
        }
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

/// Bounds-checked little-endian reader.
///
/// Every failure latches, so decoding can be written linearly and checked
/// once at the end. A latched reader never returns a plausible value: the
/// caller must test ok() (or exhausted()).
class BinaryReader {
public:
    BinaryReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool exhausted() const noexcept { return position_ == size_; }

    bool need(std::size_t count) {
        if (!ok_) {
            return false;
        }
        if (count > size_ - position_) {
            ok_ = false;
            return false;
        }
        return true;
    }

    std::uint8_t u8() {
        if (!need(1)) {
            return 0;
        }
        return data_[position_++];
    }
    std::uint16_t u16() {
        if (!need(2)) {
            return 0;
        }
        const std::uint16_t value =
            static_cast<std::uint16_t>(data_[position_]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(data_[position_ + 1]) << 8);
        position_ += 2;
        return value;
    }
    std::uint32_t u32() {
        if (!need(4)) {
            return 0;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(data_[position_ + static_cast<std::size_t>(i)])
                     << (8 * i);
        }
        position_ += 4;
        return value;
    }
    std::uint64_t u64() {
        if (!need(8)) {
            return 0;
        }
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(data_[position_ + static_cast<std::size_t>(i)])
                     << (8 * i);
        }
        position_ += 8;
        return value;
    }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    double real() {
        const std::uint64_t bits_value = u64();
        double value = 0.0;
        std::memcpy(&value, &bits_value, sizeof(value));
        return value;
    }
    std::string text(std::size_t limit) {
        const std::uint32_t length = u32();
        if (!ok_ || static_cast<std::size_t>(length) > limit || !need(length)) {
            ok_ = false;
            return {};
        }
        std::string value(reinterpret_cast<const char*>(data_ + position_), length);
        position_ += length;
        return value;
    }
    std::vector<std::uint8_t> bytes(std::size_t limit) {
        const std::uint32_t length = u32();
        if (!ok_ || static_cast<std::size_t>(length) > limit || !need(length)) {
            ok_ = false;
            return {};
        }
        std::vector<std::uint8_t> value(data_ + position_, data_ + position_ + length);
        position_ += length;
        return value;
    }
    /// Unprefixed raw bytes at the current position.
    std::vector<std::uint8_t> raw_bytes(std::size_t count) {
        if (!need(count)) {
            ok_ = false;
            return {};
        }
        std::vector<std::uint8_t> value(data_ + position_, data_ + position_ + count);
        position_ += count;
        return value;
    }
    bool presence() {
        const std::uint8_t flag = u8();
        if (flag > 1U) {
            ok_ = false;
            return false;
        }
        return flag == 1U;
    }

    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    void fail() { ok_ = false; }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t position_ = 0;
    bool ok_ = true;
};

}  // namespace thermal_governor::detail

#endif  // THERMAL_GOVERNOR_SRC_BINARY_HPP
