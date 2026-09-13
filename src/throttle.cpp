// Thermal Governor — throttle classification.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/throttle.hpp"

#include <vector>

namespace thermal_governor {
namespace {

struct ReasonName {
    ThrottleReasonBit bit;
    const char* name;
};

constexpr ReasonName kReasonNames[] = {
    {ThrottleReasonBit::GPU_IDLE, "GPU_IDLE"},
    {ThrottleReasonBit::APPLICATIONS_CLOCKS_SETTING, "APPLICATIONS_CLOCKS_SETTING"},
    {ThrottleReasonBit::SW_POWER_CAP, "SW_POWER_CAP"},
    {ThrottleReasonBit::HW_SLOWDOWN, "HW_SLOWDOWN"},
    {ThrottleReasonBit::SYNC_BOOST, "SYNC_BOOST"},
    {ThrottleReasonBit::SW_THERMAL_SLOWDOWN, "SW_THERMAL_SLOWDOWN"},
    {ThrottleReasonBit::HW_THERMAL_SLOWDOWN, "HW_THERMAL_SLOWDOWN"},
    {ThrottleReasonBit::HW_POWER_BRAKE_SLOWDOWN, "HW_POWER_BRAKE_SLOWDOWN"},
    {ThrottleReasonBit::DISPLAY_CLOCK_SETTING, "DISPLAY_CLOCK_SETTING"},
    {ThrottleReasonBit::THERMAL_SENSOR_SLOWDOWN, "THERMAL_SENSOR_SLOWDOWN"},
    {ThrottleReasonBit::BOARD_LIMIT, "BOARD_LIMIT"},
    {ThrottleReasonBit::RELIABILITY, "RELIABILITY"},
    {ThrottleReasonBit::UNKNOWN_REASON, "UNKNOWN_REASON"},
};

}  // namespace

ThrottleObservation ThrottleObservation::from_raw(std::uint64_t raw) noexcept {
    ThrottleObservation observation;
    observation.raw_reasons = raw;

    const bool thermal = (raw & kThermalThrottleMask) != 0U;
    const bool non_thermal = (raw & kNonThermalThrottleMask) != 0U;

    if (thermal) {
        // Thermal slowdown dominates any co-occurring non-thermal reason:
        // the thermal condition is the one Thermal Governor governs.
        observation.classification = ThrottleClass::THERMAL_THROTTLE_OBSERVED;
    } else if (non_thermal) {
        observation.classification = ThrottleClass::NON_THERMAL_THROTTLE_OBSERVED;
    } else {
        observation.classification = ThrottleClass::NO_THROTTLE_OBSERVED;
    }
    return observation;
}

std::string ThrottleObservation::render_reasons() const {
    std::string out;
    for (const auto& entry : kReasonNames) {
        if ((raw_reasons & bits(entry.bit)) == 0U) {
            continue;
        }
        if (!out.empty()) {
            out += ",";
        }
        out += entry.name;
    }
    if (out.empty()) {
        out = "NONE";
    }
    return out;
}

}  // namespace thermal_governor
