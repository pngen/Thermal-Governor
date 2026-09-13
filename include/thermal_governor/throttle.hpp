// Thermal Governor — throttle evidence semantics.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_THROTTLE_HPP
#define THERMAL_GOVERNOR_THROTTLE_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace thermal_governor {

/// Bit flags mirroring the vendor-neutral set of clock-throttle reasons.
enum class ThrottleReasonBit : std::uint64_t {
    NONE = 0,
    GPU_IDLE = 1ULL << 0,
    APPLICATIONS_CLOCKS_SETTING = 1ULL << 1,
    SW_POWER_CAP = 1ULL << 2,
    HW_SLOWDOWN = 1ULL << 3,
    SYNC_BOOST = 1ULL << 4,
    SW_THERMAL_SLOWDOWN = 1ULL << 5,
    HW_THERMAL_SLOWDOWN = 1ULL << 6,
    HW_POWER_BRAKE_SLOWDOWN = 1ULL << 7,
    DISPLAY_CLOCK_SETTING = 1ULL << 8,
    THERMAL_SENSOR_SLOWDOWN = 1ULL << 9,
    BOARD_LIMIT = 1ULL << 10,
    RELIABILITY = 1ULL << 11,
    UNKNOWN_REASON = 1ULL << 63,
};

[[nodiscard]] constexpr std::uint64_t bits(ThrottleReasonBit b) noexcept {
    return static_cast<std::uint64_t>(b);
}

inline constexpr std::uint64_t kThermalThrottleMask =
    bits(ThrottleReasonBit::SW_THERMAL_SLOWDOWN) | bits(ThrottleReasonBit::HW_THERMAL_SLOWDOWN) |
    bits(ThrottleReasonBit::HW_SLOWDOWN) | bits(ThrottleReasonBit::THERMAL_SENSOR_SLOWDOWN);

inline constexpr std::uint64_t kNonThermalThrottleMask =
    bits(ThrottleReasonBit::SW_POWER_CAP) | bits(ThrottleReasonBit::HW_POWER_BRAKE_SLOWDOWN) |
    bits(ThrottleReasonBit::APPLICATIONS_CLOCKS_SETTING) | bits(ThrottleReasonBit::SYNC_BOOST) |
    bits(ThrottleReasonBit::DISPLAY_CLOCK_SETTING) | bits(ThrottleReasonBit::BOARD_LIMIT) |
    bits(ThrottleReasonBit::RELIABILITY);

/// Classification of the observed throttle condition.
///
/// Not every clock reduction is thermal throttling, and absence of evidence
/// is not evidence of absence.
enum class ThrottleClass : std::uint8_t {
    THROTTLE_UNKNOWN = 0,
    NO_THROTTLE_OBSERVED,
    THERMAL_THROTTLE_OBSERVED,
    NON_THERMAL_THROTTLE_OBSERVED,
    THROTTLE_UNSUPPORTED,
};

[[nodiscard]] constexpr std::string_view to_string(ThrottleClass c) noexcept {
    switch (c) {
        case ThrottleClass::THROTTLE_UNKNOWN: return "THROTTLE_UNKNOWN";
        case ThrottleClass::NO_THROTTLE_OBSERVED: return "NO_THROTTLE_OBSERVED";
        case ThrottleClass::THERMAL_THROTTLE_OBSERVED: return "THERMAL_THROTTLE_OBSERVED";
        case ThrottleClass::NON_THERMAL_THROTTLE_OBSERVED: return "NON_THERMAL_THROTTLE_OBSERVED";
        case ThrottleClass::THROTTLE_UNSUPPORTED: return "THROTTLE_UNSUPPORTED";
    }
    return "UNRECOGNISED_THROTTLE_CLASS";
}

/// Immutable throttle observation derived from a raw reason bitmask.
struct ThrottleObservation {
    std::uint64_t raw_reasons = 0;
    ThrottleClass classification = ThrottleClass::THROTTLE_UNKNOWN;

    [[nodiscard]] static ThrottleObservation from_raw(std::uint64_t raw) noexcept;

    [[nodiscard]] static ThrottleObservation unsupported() noexcept {
        ThrottleObservation o;
        o.classification = ThrottleClass::THROTTLE_UNSUPPORTED;
        return o;
    }

    [[nodiscard]] static ThrottleObservation unknown() noexcept {
        return ThrottleObservation{};
    }

    [[nodiscard]] bool thermal_active() const noexcept {
        return classification == ThrottleClass::THERMAL_THROTTLE_OBSERVED;
    }
    [[nodiscard]] bool any_throttle() const noexcept {
        return classification == ThrottleClass::THERMAL_THROTTLE_OBSERVED ||
               classification == ThrottleClass::NON_THERMAL_THROTTLE_OBSERVED;
    }

    /// Stable, ordered, comma-separated reason list.
    [[nodiscard]] std::string render_reasons() const;

    friend bool operator==(const ThrottleObservation&, const ThrottleObservation&) = default;
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_THROTTLE_HPP
