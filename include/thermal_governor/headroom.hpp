// Thermal Governor — explicit thermal headroom.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_HEADROOM_HPP
#define THERMAL_GOVERNOR_HEADROOM_HPP

#include <optional>
#include <string>

#include "thermal_governor/provenance.hpp"
#include "thermal_governor/quantity.hpp"

namespace thermal_governor {

/// Inputs to the headroom computation.
///
/// Usable headroom is never merely (thermal_limit - current_temperature).
struct HeadroomInput {
    DegreesCelsius current_temperature{};

    /// Policy-defined maximum legal temperature (the critical threshold).
    DegreesCelsius policy_critical{};

    /// Vendor-reported temperature limit, when a REAL backend exposes one.
    /// The governing ceiling is the lower of this and the policy critical.
    std::optional<DegreesCelsius> vendor_limit;
    Provenance vendor_limit_provenance = Provenance::UNKNOWN;

    /// Sensor uncertainty, in degrees, derived from reported confidence.
    TemperatureDelta uncertainty_margin{0.0};
    TemperatureDelta policy_safety_margin{0.0};

    /// Headroom above the recovery threshold that must remain before
    /// recovery may be authorised.
    TemperatureDelta recovery_margin{0.0};
    DegreesCelsius recovery_threshold{};
};

/// Full, auditable headroom breakdown.
struct HeadroomBreakdown {
    DegreesCelsius policy_critical{};
    std::optional<DegreesCelsius> vendor_limit;
    /// min(policy_critical, vendor_limit) when a REAL vendor limit exists.
    DegreesCelsius governing_limit{};
    DegreesCelsius current_temperature{};

    TemperatureDelta raw_headroom{};
    TemperatureDelta policy_safety_margin{};
    TemperatureDelta uncertainty_margin{};

    /// raw_headroom - policy_safety_margin - uncertainty_margin.
    ///
    /// Deliberately not clamped: a negative value is an honest deficit and
    /// is what admission predicates test against.
    TemperatureDelta effective_headroom{};

    DegreesCelsius recovery_threshold{};
    TemperatureDelta recovery_margin{};
    /// current_temperature - (recovery_threshold + recovery_margin)
    TemperatureDelta distance_above_recovery_gate{};

    [[nodiscard]] bool has_positive_headroom() const noexcept {
        return effective_headroom.value() > 0.0;
    }
    [[nodiscard]] bool recovery_headroom_satisfied() const noexcept {
        return distance_above_recovery_gate.value() < 0.0;
    }
    [[nodiscard]] bool meets_demand(TemperatureDelta demand) const noexcept {
        return effective_headroom.value() >= demand.value();
    }
};

/// Compute the full headroom breakdown. Pure and deterministic.
[[nodiscard]] HeadroomBreakdown compute_headroom(const HeadroomInput& input) noexcept;

/// Derive a sensor uncertainty margin from a reported confidence in [0, 1].
///
/// A confidence of 1.0 yields zero added uncertainty; a confidence of 0.0
/// yields the full configured sensor uncertainty. Absent confidence yields
/// the configured uncertainty unchanged: absence of a confidence claim is
/// not evidence of accuracy.
[[nodiscard]] TemperatureDelta uncertainty_from_confidence(
    const std::optional<double>& confidence, TemperatureDelta configured) noexcept;

/// Canonical rendering of a headroom breakdown. Field order is stable.
[[nodiscard]] std::string render_headroom(const HeadroomBreakdown& breakdown);

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_HEADROOM_HPP
