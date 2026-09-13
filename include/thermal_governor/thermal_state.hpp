// Thermal Governor — thermal state model.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_THERMAL_STATE_HPP
#define THERMAL_GOVERNOR_THERMAL_STATE_HPP

#include <cstdint>
#include <string_view>

namespace thermal_governor {

/// Explicit thermal state.
///
/// DERATED and THROTTLING are deliberately distinct: a resource may be
/// proactively DERATED before hardware throttling occurs, and THROTTLING
/// represents actual throttle evidence rather than an inference from
/// temperature alone.
enum class ThermalState : std::uint8_t {
    NORMAL = 0,
    WARM,
    NEAR_LIMIT,
    DERATED,
    THROTTLING,
    CRITICAL,
    REVALIDATION_REQUIRED,
    UNKNOWN,
    UNSUPPORTED,
};

inline constexpr std::uint32_t kThermalStateCount = 9;

[[nodiscard]] constexpr std::string_view to_string(ThermalState s) noexcept {
    switch (s) {
        case ThermalState::NORMAL: return "NORMAL";
        case ThermalState::WARM: return "WARM";
        case ThermalState::NEAR_LIMIT: return "NEAR_LIMIT";
        case ThermalState::DERATED: return "DERATED";
        case ThermalState::THROTTLING: return "THROTTLING";
        case ThermalState::CRITICAL: return "CRITICAL";
        case ThermalState::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
        case ThermalState::UNKNOWN: return "UNKNOWN";
        case ThermalState::UNSUPPORTED: return "UNSUPPORTED";
    }
    return "UNRECOGNISED_THERMAL_STATE";
}

/// Severity rank used for monotonicity checks.
///
/// UNKNOWN and UNSUPPORTED rank above the healthy states: neither is ever
/// allowed to silently downgrade into NORMAL.
[[nodiscard]] constexpr std::uint32_t severity(ThermalState s) noexcept {
    switch (s) {
        case ThermalState::NORMAL: return 0;
        case ThermalState::WARM: return 1;
        case ThermalState::NEAR_LIMIT: return 2;
        case ThermalState::DERATED: return 3;
        case ThermalState::THROTTLING: return 4;
        case ThermalState::CRITICAL: return 5;
        case ThermalState::REVALIDATION_REQUIRED: return 6;
        case ThermalState::UNKNOWN: return 7;
        case ThermalState::UNSUPPORTED: return 8;
    }
    return 9;
}

/// True when the state captures a restriction on execution authority.
[[nodiscard]] constexpr bool is_restrictive(ThermalState s) noexcept {
    return s == ThermalState::DERATED || s == ThermalState::THROTTLING ||
           s == ThermalState::CRITICAL;
}

/// True when the state represents absence of trustworthy thermal knowledge.
[[nodiscard]] constexpr bool is_unresolved(ThermalState s) noexcept {
    return s == ThermalState::UNKNOWN || s == ThermalState::UNSUPPORTED ||
           s == ThermalState::REVALIDATION_REQUIRED;
}

/// True when recovery hysteresis applies: leaving this state requires
/// positive fresh evidence rather than mere absence of a violation.
[[nodiscard]] constexpr bool requires_recovery_gate(ThermalState s) noexcept {
    return s == ThermalState::DERATED || s == ThermalState::THROTTLING ||
           s == ThermalState::CRITICAL || s == ThermalState::REVALIDATION_REQUIRED;
}

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_THERMAL_STATE_HPP
