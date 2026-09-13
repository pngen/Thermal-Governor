// Thermal Governor — independent capability model.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_CAPABILITY_HPP
#define THERMAL_GOVERNOR_CAPABILITY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "thermal_governor/identity.hpp"
#include "thermal_governor/provenance.hpp"

namespace thermal_governor {

/// Each thermal capability is resolved independently. One working telemetry
/// source never implies that another works.
enum class ThermalCapability : std::uint8_t {
    DEVICE_IDENTITY = 0,
    TEMPERATURE,
    TEMPERATURE_LIMIT,
    TEMPERATURE_THRESHOLD_SHUTDOWN,
    TEMPERATURE_THRESHOLD_SLOWDOWN,
    THROTTLE_REASONS,
    CURRENT_CLOCK,
    MAX_CLOCK,
    FAN_TELEMETRY,
    COOLING_TELEMETRY,
    THERMAL_DOMAIN_METADATA,
    NODE_THERMAL_METADATA,
    RACK_THERMAL_METADATA,
    ACTUAL_THROTTLE_ENFORCEMENT,
    CLOCK_REDUCTION_ENFORCEMENT,
    COUNT,
};

inline constexpr std::size_t kThermalCapabilityCount =
    static_cast<std::size_t>(ThermalCapability::COUNT);

[[nodiscard]] constexpr std::string_view capability_name(ThermalCapability c) noexcept {
    switch (c) {
        case ThermalCapability::DEVICE_IDENTITY: return "DEVICE_IDENTITY";
        case ThermalCapability::TEMPERATURE: return "TEMPERATURE";
        case ThermalCapability::TEMPERATURE_LIMIT: return "TEMPERATURE_LIMIT";
        case ThermalCapability::TEMPERATURE_THRESHOLD_SHUTDOWN: return "TEMPERATURE_THRESHOLD_SHUTDOWN";
        case ThermalCapability::TEMPERATURE_THRESHOLD_SLOWDOWN: return "TEMPERATURE_THRESHOLD_SLOWDOWN";
        case ThermalCapability::THROTTLE_REASONS: return "THROTTLE_REASONS";
        case ThermalCapability::CURRENT_CLOCK: return "CURRENT_CLOCK";
        case ThermalCapability::MAX_CLOCK: return "MAX_CLOCK";
        case ThermalCapability::FAN_TELEMETRY: return "FAN_TELEMETRY";
        case ThermalCapability::COOLING_TELEMETRY: return "COOLING_TELEMETRY";
        case ThermalCapability::THERMAL_DOMAIN_METADATA: return "THERMAL_DOMAIN_METADATA";
        case ThermalCapability::NODE_THERMAL_METADATA: return "NODE_THERMAL_METADATA";
        case ThermalCapability::RACK_THERMAL_METADATA: return "RACK_THERMAL_METADATA";
        case ThermalCapability::ACTUAL_THROTTLE_ENFORCEMENT: return "ACTUAL_THROTTLE_ENFORCEMENT";
        case ThermalCapability::CLOCK_REDUCTION_ENFORCEMENT: return "CLOCK_REDUCTION_ENFORCEMENT";
        case ThermalCapability::COUNT: break;
    }
    return "UNRECOGNISED_CAPABILITY";
}

/// Resolution state of a single capability.
///
/// SUPPORTED_SYNTHETIC is deliberately distinct from SUPPORTED_REAL: a
/// modelled capability must never be presented as a measured one.
enum class CapabilityState : std::uint8_t {
    UNKNOWN = 0,
    SUPPORTED_REAL,
    SUPPORTED_READ_ONLY,
    SUPPORTED_SYNTHETIC,
    UNSUPPORTED,
};

[[nodiscard]] constexpr std::string_view to_string(CapabilityState s) noexcept {
    switch (s) {
        case CapabilityState::UNKNOWN: return "UNKNOWN";
        case CapabilityState::SUPPORTED_REAL: return "SUPPORTED_REAL";
        case CapabilityState::SUPPORTED_READ_ONLY: return "SUPPORTED_READ_ONLY";
        case CapabilityState::SUPPORTED_SYNTHETIC: return "SUPPORTED_SYNTHETIC";
        case CapabilityState::UNSUPPORTED: return "UNSUPPORTED";
    }
    return "UNRECOGNISED_CAPABILITY_STATE";
}

[[nodiscard]] constexpr bool is_supported(CapabilityState s) noexcept {
    return s == CapabilityState::SUPPORTED_REAL || s == CapabilityState::SUPPORTED_READ_ONLY ||
           s == CapabilityState::SUPPORTED_SYNTHETIC;
}

[[nodiscard]] constexpr Provenance provenance_of(CapabilityState s) noexcept {
    switch (s) {
        case CapabilityState::SUPPORTED_REAL: return Provenance::REAL;
        case CapabilityState::SUPPORTED_READ_ONLY: return Provenance::REAL;
        case CapabilityState::SUPPORTED_SYNTHETIC: return Provenance::SYNTHETIC;
        case CapabilityState::UNSUPPORTED: return Provenance::UNSUPPORTED;
        case CapabilityState::UNKNOWN: return Provenance::UNKNOWN;
    }
    return Provenance::UNKNOWN;
}

/// Independently resolved capability set for one subject.
class CapabilitySet {
public:
    CapabilitySet() {
        states_.fill(CapabilityState::UNKNOWN);
        details_.fill(std::string{});
    }

    void set(ThermalCapability c, CapabilityState state, std::string detail = {}) {
        const auto index = static_cast<std::size_t>(c);
        if (index >= kThermalCapabilityCount) {
            return;
        }
        states_[index] = state;
        details_[index] = std::move(detail);
    }

    [[nodiscard]] CapabilityState get(ThermalCapability c) const noexcept {
        const auto index = static_cast<std::size_t>(c);
        if (index >= kThermalCapabilityCount) {
            return CapabilityState::UNKNOWN;
        }
        return states_[index];
    }

    [[nodiscard]] bool supported(ThermalCapability c) const noexcept {
        return is_supported(get(c));
    }

    [[nodiscard]] const std::string& detail(ThermalCapability c) const noexcept {
        const auto index = static_cast<std::size_t>(c);
        if (index >= kThermalCapabilityCount) {
            static const std::string empty;
            return empty;
        }
        return details_[index];
    }

    /// True when every capability in the set is still UNKNOWN.
    [[nodiscard]] bool all_unknown() const noexcept {
        for (const auto s : states_) {
            if (s != CapabilityState::UNKNOWN) {
                return false;
            }
        }
        return true;
    }

    /// Rendering is stable and ordered by capability index.
    [[nodiscard]] std::string render() const;

    CapabilityGeneration generation{};

private:
    std::array<CapabilityState, kThermalCapabilityCount> states_{};
    std::array<std::string, kThermalCapabilityCount> details_{};
};

/// Description of a thermal backend's capability set, produced before any
/// evidence is trusted.
struct BackendCapabilities {
    std::string backend_name;
    std::string backend_version;
    Provenance provenance = Provenance::UNKNOWN;
    CapabilitySet capabilities;
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_CAPABILITY_HPP
