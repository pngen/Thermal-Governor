// Thermal Governor — explicit headroom computation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/headroom.hpp"

#include "format.hpp"

#include <algorithm>

namespace thermal_governor {

TemperatureDelta uncertainty_from_confidence(const std::optional<double>& confidence,
                                             TemperatureDelta configured) noexcept {
    if (!confidence.has_value()) {
        return configured;
    }
    const double c = std::clamp(*confidence, 0.0, 1.0);
    return TemperatureDelta{configured.value() * (1.0 - c)};
}

HeadroomBreakdown compute_headroom(const HeadroomInput& input) noexcept {
    HeadroomBreakdown out;

    out.policy_critical = input.policy_critical;
    out.current_temperature = input.current_temperature;
    out.policy_safety_margin = input.policy_safety_margin;
    out.uncertainty_margin = input.uncertainty_margin;
    out.recovery_threshold = input.recovery_threshold;
    out.recovery_margin = input.recovery_margin;

    DegreesCelsius governing = input.policy_critical;
    if (input.vendor_limit.has_value() && input.vendor_limit->is_valid()) {
        // Only a REAL vendor limit may tighten the governing ceiling. A
        // synthetic limit cannot override policy.
        if (input.vendor_limit_provenance == Provenance::REAL &&
            input.vendor_limit->value() < governing.value()) {
            governing = *input.vendor_limit;
        }
        out.vendor_limit = *input.vendor_limit;
    }
    out.governing_limit = governing;

    out.raw_headroom = DegreesCelsius{governing.value()} - input.current_temperature;
    out.effective_headroom =
        TemperatureDelta{out.raw_headroom.value() - input.policy_safety_margin.value() -
                         input.uncertainty_margin.value()};
    out.distance_above_recovery_gate =
        input.current_temperature -
        DegreesCelsius{input.recovery_threshold.value() + input.recovery_margin.value()};

    return out;
}

std::string render_headroom(const HeadroomBreakdown& breakdown) {
    std::string out;
    out += "PolicyMaximumLegalTemperature: " +
           detail::temperature(breakdown.policy_critical.value()) + "\n";
    if (breakdown.vendor_limit.has_value()) {
        out += "VendorTemperatureLimit: " +
               detail::temperature(breakdown.vendor_limit->value()) + "\n";
    } else {
        out += "VendorTemperatureLimit: <unsupported>\n";
    }
    out += "GoverningLimit: " + detail::temperature(breakdown.governing_limit.value()) + "\n";
    out += "CurrentTemperature: " + detail::temperature(breakdown.current_temperature.value()) + "\n";
    out += "RawHeadroom: " + detail::number(breakdown.raw_headroom.value()) + "\n";
    out += "PolicySafetyMargin: " + detail::number(breakdown.policy_safety_margin.value()) + "\n";
    out += "UncertaintyMargin: " + detail::number(breakdown.uncertainty_margin.value()) + "\n";
    out += "EffectiveHeadroom: " + detail::number(breakdown.effective_headroom.value()) + "\n";
    out += "RecoveryThreshold: " + detail::temperature(breakdown.recovery_threshold.value()) + "\n";
    out += "RecoveryMargin: " + detail::number(breakdown.recovery_margin.value()) + "\n";
    out += "DistanceAboveRecoveryGate: " +
           detail::number(breakdown.distance_above_recovery_gate.value()) + "\n";
    return out;
}

}  // namespace thermal_governor
