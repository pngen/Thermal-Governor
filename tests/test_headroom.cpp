// Thermal Governor — explicit headroom tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <optional>
#include <string>

#include "framework.hpp"

#include "thermal_governor/headroom.hpp"
#include "thermal_governor/provenance.hpp"
#include "thermal_governor/quantity.hpp"

using thermal_governor::DegreesCelsius;
using thermal_governor::HeadroomBreakdown;
using thermal_governor::HeadroomInput;
using thermal_governor::Provenance;
using thermal_governor::TemperatureDelta;
using thermal_governor::compute_headroom;
using thermal_governor::render_headroom;
using thermal_governor::uncertainty_from_confidence;

namespace {

[[nodiscard]] HeadroomInput base_input() {
    HeadroomInput input;
    input.current_temperature = DegreesCelsius{80.0};
    input.policy_critical = DegreesCelsius{88.0};
    input.recovery_threshold = DegreesCelsius{72.0};
    input.recovery_margin = TemperatureDelta{1.0};
    input.policy_safety_margin = TemperatureDelta{2.0};
    input.uncertainty_margin = TemperatureDelta{1.5};
    return input;
}

}  // namespace

TG_CASE(headroom, raw_and_effective_are_exact) {
    const HeadroomBreakdown breakdown = compute_headroom(base_input());

    TG_CHECK_EQ(breakdown.governing_limit.value(), 88.0);
    TG_CHECK_EQ(breakdown.current_temperature.value(), 80.0);
    TG_CHECK_EQ(breakdown.raw_headroom.value(), 8.0);
    TG_CHECK_EQ(breakdown.policy_safety_margin.value(), 2.0);
    TG_CHECK_EQ(breakdown.uncertainty_margin.value(), 1.5);
    TG_CHECK_EQ(breakdown.effective_headroom.value(), 4.5);
    TG_CHECK(breakdown.has_positive_headroom());

    HeadroomInput exact = base_input();
    exact.current_temperature = DegreesCelsius{80.25};
    exact.policy_safety_margin = TemperatureDelta{0.25};
    exact.uncertainty_margin = TemperatureDelta{0.5};
    const HeadroomBreakdown quartered = compute_headroom(exact);
    TG_CHECK_EQ(quartered.raw_headroom.value(), 7.75);
    TG_CHECK_EQ(quartered.effective_headroom.value(), 7.0);
}

TG_CASE(headroom, real_vendor_limit_below_critical_governs) {
    HeadroomInput input = base_input();
    input.vendor_limit = DegreesCelsius{85.0};
    input.vendor_limit_provenance = Provenance::REAL;

    const HeadroomBreakdown breakdown = compute_headroom(input);
    TG_CHECK(breakdown.vendor_limit.has_value());
    TG_CHECK_EQ(breakdown.vendor_limit->value(), 85.0);
    TG_CHECK_EQ(breakdown.policy_critical.value(), 88.0);
    TG_CHECK_EQ(breakdown.governing_limit.value(), 85.0);
    TG_CHECK_EQ(breakdown.raw_headroom.value(), 5.0);
    TG_CHECK_EQ(breakdown.effective_headroom.value(), 1.5);
}

TG_CASE(headroom, real_vendor_limit_above_critical_is_reported_but_does_not_govern) {
    HeadroomInput input = base_input();
    input.vendor_limit = DegreesCelsius{95.0};
    input.vendor_limit_provenance = Provenance::REAL;

    const HeadroomBreakdown breakdown = compute_headroom(input);
    TG_CHECK(breakdown.vendor_limit.has_value());
    TG_CHECK_EQ(breakdown.vendor_limit->value(), 95.0);
    TG_CHECK_EQ(breakdown.governing_limit.value(), 88.0);
    TG_CHECK_EQ(breakdown.raw_headroom.value(), 8.0);
}

TG_CASE(headroom, synthetic_vendor_limit_never_governs) {
    const Provenance non_real[] = {Provenance::UNKNOWN, Provenance::SYNTHETIC,
                                   Provenance::UNSUPPORTED};
    for (const Provenance provenance : non_real) {
        HeadroomInput input = base_input();
        input.vendor_limit = DegreesCelsius{70.0};
        input.vendor_limit_provenance = provenance;
        const HeadroomBreakdown breakdown = compute_headroom(input);
        TG_CHECK(breakdown.vendor_limit.has_value());
        TG_CHECK_EQ(breakdown.vendor_limit->value(), 70.0);
        TG_CHECK_EQ(breakdown.governing_limit.value(), 88.0);
        TG_CHECK_EQ(breakdown.raw_headroom.value(), 8.0);
    }
}

TG_CASE(headroom, invalid_vendor_limit_is_ignored) {
    HeadroomInput input = base_input();
    input.vendor_limit = DegreesCelsius{-400.0};
    input.vendor_limit_provenance = Provenance::REAL;

    const HeadroomBreakdown breakdown = compute_headroom(input);
    TG_CHECK(!breakdown.vendor_limit.has_value());
    TG_CHECK_EQ(breakdown.governing_limit.value(), 88.0);

    HeadroomInput absent = base_input();
    const HeadroomBreakdown unsupported = compute_headroom(absent);
    TG_CHECK(!unsupported.vendor_limit.has_value());
    TG_CHECK_EQ(unsupported.governing_limit.value(), 88.0);
}

TG_CASE(headroom, uncertainty_from_confidence_behaviour) {
    const TemperatureDelta configured{1.5};

    TG_PHASE("absent confidence keeps the configured uncertainty");
    TG_CHECK_EQ(uncertainty_from_confidence(std::nullopt, configured).value(), 1.5);
    TG_CHECK_EQ(uncertainty_from_confidence(std::optional<double>{}, configured).value(), 1.5);

    TG_PHASE("confidence of zero keeps the full configured uncertainty");
    TG_CHECK_EQ(uncertainty_from_confidence(0.0, configured).value(), 1.5);

    TG_PHASE("confidence of one removes the added uncertainty");
    TG_CHECK_EQ(uncertainty_from_confidence(1.0, configured).value(), 0.0);

    TG_PHASE("partial confidence scales exactly");
    TG_CHECK_EQ(uncertainty_from_confidence(0.5, configured).value(), 0.75);
    TG_CHECK_EQ(uncertainty_from_confidence(0.25, configured).value(), 1.125);
    TG_CHECK_EQ(uncertainty_from_confidence(0.75, configured).value(), 0.375);

    TG_PHASE("out-of-range confidence is clamped, never extrapolated");
    TG_CHECK_EQ(uncertainty_from_confidence(2.0, configured).value(), 0.0);
    TG_CHECK_EQ(uncertainty_from_confidence(-1.0, configured).value(), 1.5);
}

TG_CASE(headroom, effective_headroom_is_not_clamped) {
    HeadroomInput input = base_input();
    input.current_temperature = DegreesCelsius{95.5};
    input.policy_safety_margin = TemperatureDelta{2.0};
    input.uncertainty_margin = TemperatureDelta{1.5};

    const HeadroomBreakdown breakdown = compute_headroom(input);
    TG_CHECK_EQ(breakdown.raw_headroom.value(), -7.5);
    TG_CHECK_EQ(breakdown.effective_headroom.value(), -11.0);
    TG_CHECK(!breakdown.has_positive_headroom());
    TG_CHECK(breakdown.effective_headroom.value() < 0.0);
    TG_CHECK(breakdown.raw_headroom.value() < 0.0);
    TG_CHECK(breakdown.meets_demand(TemperatureDelta{-11.0}));
    TG_CHECK(!breakdown.meets_demand(TemperatureDelta{-10.75}));
    TG_CHECK(!breakdown.meets_demand(TemperatureDelta{0.0}));
}

TG_CASE(headroom, recovery_gate_distance_sign) {
    HeadroomInput input = base_input();
    input.recovery_threshold = DegreesCelsius{72.0};
    input.recovery_margin = TemperatureDelta{1.25};

    HeadroomBreakdown below = compute_headroom(input);
    TG_CHECK_EQ(below.distance_above_recovery_gate.value(), 6.75);
    TG_CHECK(!below.recovery_headroom_satisfied());

    input.current_temperature = DegreesCelsius{73.25};
    HeadroomBreakdown at_gate = compute_headroom(input);
    TG_CHECK_EQ(at_gate.distance_above_recovery_gate.value(), 0.0);
    TG_CHECK(!at_gate.recovery_headroom_satisfied());

    input.current_temperature = DegreesCelsius{73.0};
    HeadroomBreakdown below_gate = compute_headroom(input);
    TG_CHECK_EQ(below_gate.distance_above_recovery_gate.value(), -0.25);
    TG_CHECK(below_gate.recovery_headroom_satisfied());
}

TG_CASE(headroom, meets_demand_boundaries) {
    const HeadroomBreakdown breakdown = compute_headroom(base_input());
    TG_CHECK_EQ(breakdown.effective_headroom.value(), 4.5);
    TG_CHECK(breakdown.meets_demand(TemperatureDelta{4.5}));
    TG_CHECK(breakdown.meets_demand(TemperatureDelta{4.25}));
    TG_CHECK(!breakdown.meets_demand(TemperatureDelta{4.75}));
    TG_CHECK(breakdown.meets_demand(TemperatureDelta{0.0}));
    TG_CHECK(!breakdown.meets_demand(TemperatureDelta{5.0}));
}

TG_CASE(headroom, render_headroom_is_deterministic) {
    const HeadroomInput input = base_input();
    const HeadroomBreakdown first = compute_headroom(input);
    const HeadroomBreakdown second = compute_headroom(input);

    const std::string text = render_headroom(first);
    TG_CHECK_EQ(text, render_headroom(second));
    TG_CHECK_EQ(text, render_headroom(first));
    TG_CHECK(!text.empty());

    TG_CHECK(text.find("PolicyMaximumLegalTemperature: 88") != std::string::npos);
    TG_CHECK(text.find("VendorTemperatureLimit: <unsupported>") != std::string::npos);
    TG_CHECK(text.find("GoverningLimit: 88") != std::string::npos);
    TG_CHECK(text.find("CurrentTemperature: 80") != std::string::npos);
    TG_CHECK(text.find("RawHeadroom: 8") != std::string::npos);
    TG_CHECK(text.find("EffectiveHeadroom: 4.5") != std::string::npos);
    TG_CHECK(text.find("DistanceAboveRecoveryGate: 7") != std::string::npos);

    TG_PHASE("field order is stable");
    const std::size_t policy_position = text.find("PolicyMaximumLegalTemperature:");
    const std::size_t governing_position = text.find("GoverningLimit:");
    const std::size_t effective_position = text.find("EffectiveHeadroom:");
    const std::size_t recovery_position = text.find("DistanceAboveRecoveryGate:");
    TG_CHECK(policy_position < governing_position);
    TG_CHECK(governing_position < effective_position);
    TG_CHECK(effective_position < recovery_position);

    TG_PHASE("a different input renders differently");
    HeadroomInput other = base_input();
    other.vendor_limit = DegreesCelsius{70.0};
    other.vendor_limit_provenance = Provenance::REAL;
    const std::string other_text = render_headroom(compute_headroom(other));
    TG_CHECK(other_text != text);
    TG_CHECK(other_text.find("VendorTemperatureLimit: 70") != std::string::npos);
}
