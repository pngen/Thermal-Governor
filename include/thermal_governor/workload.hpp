// Thermal Governor — workload thermal profile hints.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_WORKLOAD_HPP
#define THERMAL_GOVERNOR_WORKLOAD_HPP

#include <cstdint>
#include <optional>
#include <string_view>

#include "thermal_governor/identity.hpp"

namespace thermal_governor {

/// Optional caller-supplied thermal hint about a workload.
///
/// These are *policy inputs*, never physical truth. Thermal Governor does not
/// infer wattage or an exact temperature rise from a workload class, and it
/// does not duplicate any power-oriented workload taxonomy.
enum class WorkloadThermalProfile : std::uint8_t {
    UNKNOWN_PROFILE = 0,
    LOW_THERMAL_INTENSITY,
    MODERATE_THERMAL_INTENSITY,
    HIGH_THERMAL_INTENSITY,
    BURSTY_THERMAL_PROFILE,
    CUSTOM,
};

[[nodiscard]] constexpr std::string_view to_string(WorkloadThermalProfile p) noexcept {
    switch (p) {
        case WorkloadThermalProfile::UNKNOWN_PROFILE: return "UNKNOWN_PROFILE";
        case WorkloadThermalProfile::LOW_THERMAL_INTENSITY: return "LOW_THERMAL_INTENSITY";
        case WorkloadThermalProfile::MODERATE_THERMAL_INTENSITY: return "MODERATE_THERMAL_INTENSITY";
        case WorkloadThermalProfile::HIGH_THERMAL_INTENSITY: return "HIGH_THERMAL_INTENSITY";
        case WorkloadThermalProfile::BURSTY_THERMAL_PROFILE: return "BURSTY_THERMAL_PROFILE";
        case WorkloadThermalProfile::CUSTOM: return "CUSTOM";
    }
    return "UNRECOGNISED_WORKLOAD_PROFILE";
}

/// Thermal execution class a workload may require.
enum class ExecutionClass : std::uint8_t {
    UNRESTRICTED = 0,
    REDUCED_CONCURRENCY,
    REDUCED_CLOCK,
    BEST_EFFORT_THERMAL,
};

[[nodiscard]] constexpr std::string_view to_string(ExecutionClass c) noexcept {
    switch (c) {
        case ExecutionClass::UNRESTRICTED: return "UNRESTRICTED";
        case ExecutionClass::REDUCED_CONCURRENCY: return "REDUCED_CONCURRENCY";
        case ExecutionClass::REDUCED_CLOCK: return "REDUCED_CLOCK";
        case ExecutionClass::BEST_EFFORT_THERMAL: return "BEST_EFFORT_THERMAL";
    }
    return "UNRECOGNISED_EXECUTION_CLASS";
}

/// A workload's declared thermal characteristics.
struct WorkloadThermalProfileSpec {
    WorkloadId workload{};
    WorkloadThermalProfile profile = WorkloadThermalProfile::UNKNOWN_PROFILE;
    /// Only meaningful when profile == CUSTOM. Relative thermal intensity in
    /// [0, 1]; a policy input, not a physical measurement.
    std::optional<double> custom_intensity;

    friend bool operator==(const WorkloadThermalProfileSpec&,
                           const WorkloadThermalProfileSpec&) = default;
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_WORKLOAD_HPP
