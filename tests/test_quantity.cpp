// Thermal Governor — quantity, delta and threshold boundary tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <limits>
#include <string>

#include "framework.hpp"

#include "thermal_governor/headroom.hpp"
#include "thermal_governor/policy.hpp"
#include "thermal_governor/quantity.hpp"

using thermal_governor::DegreesCelsius;
using thermal_governor::HeadroomBreakdown;
using thermal_governor::HeadroomInput;
using thermal_governor::Percent;
using thermal_governor::TemperatureDelta;
using thermal_governor::ThermalErrorCode;
using thermal_governor::ThermalThresholds;
using thermal_governor::compute_headroom;

namespace {

/// Boundary step. Every value built from it is exactly representable as a
/// binary double, so every assertion below is an exact comparison.
constexpr double kEpsilon = 0.25;
constexpr double kAbsoluteZero = -273.15;
constexpr double kMaxPlausible = 1500.0;

[[nodiscard]] HeadroomInput boundary_input(double current, double critical, double recovery,
                                           double recovery_margin) {
    HeadroomInput input;
    input.current_temperature = DegreesCelsius{current};
    input.policy_critical = DegreesCelsius{critical};
    input.recovery_threshold = DegreesCelsius{recovery};
    input.recovery_margin = TemperatureDelta{recovery_margin};
    return input;
}

[[nodiscard]] ThermalThresholds boundary_thresholds() {
    ThermalThresholds thresholds;
    thresholds.warning = DegreesCelsius{75.0};
    thresholds.derating = DegreesCelsius{80.0};
    thresholds.critical = DegreesCelsius{88.0};
    thresholds.recovery = DegreesCelsius{72.0};
    thresholds.near_limit_band = TemperatureDelta{2.25};
    return thresholds;
}

}  // namespace

TG_CASE(quantity, try_from_rejects_non_finite) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    TG_PHASE("non-finite temperatures");
    TG_CHECK(!DegreesCelsius::try_from(nan).has_value());
    TG_CHECK(!DegreesCelsius::try_from(inf).has_value());
    TG_CHECK(!DegreesCelsius::try_from(-inf).has_value());

    TG_PHASE("non-finite deltas");
    TG_CHECK(!TemperatureDelta::try_from(nan).has_value());
    TG_CHECK(!TemperatureDelta::try_from(inf).has_value());
    TG_CHECK(!TemperatureDelta::try_from(-inf).has_value());

    TG_PHASE("non-finite percentages");
    TG_CHECK(!Percent::try_from(nan).has_value());
    TG_CHECK(!Percent::try_from(inf).has_value());
    TG_CHECK(!Percent::try_from(-inf).has_value());
}

TG_CASE(quantity, try_from_rejects_implausible_magnitudes) {
    TG_PHASE("temperature below absolute zero");
    TG_CHECK(!DegreesCelsius::try_from(kAbsoluteZero - kEpsilon).has_value());
    TG_CHECK(!DegreesCelsius::try_from(-1000.0).has_value());

    TG_PHASE("temperature above the plausibility bound");
    TG_CHECK(!DegreesCelsius::try_from(kMaxPlausible + kEpsilon).has_value());
    TG_CHECK(!DegreesCelsius::try_from(2000.0).has_value());

    TG_PHASE("delta magnitude bound");
    TG_CHECK(!TemperatureDelta::try_from(2000.0 + kEpsilon).has_value());
    TG_CHECK(!TemperatureDelta::try_from(-2000.0 - kEpsilon).has_value());
    TG_CHECK(!TemperatureDelta::try_from(1.0e9).has_value());
}

TG_CASE(quantity, try_from_accepts_exact_boundaries) {
    TG_PHASE("absolute zero is inclusive");
    const auto cold = DegreesCelsius::try_from(kAbsoluteZero);
    TG_CHECK(cold.has_value());
    TG_CHECK_EQ(cold->value(), kAbsoluteZero);
    TG_CHECK(cold->is_valid());

    TG_PHASE("plausibility bound is inclusive");
    const auto hot = DegreesCelsius::try_from(kMaxPlausible);
    TG_CHECK(hot.has_value());
    TG_CHECK_EQ(hot->value(), kMaxPlausible);
    TG_CHECK(hot->is_valid());

    TG_PHASE("delta bounds are inclusive");
    TG_CHECK(TemperatureDelta::try_from(2000.0).has_value());
    TG_CHECK(TemperatureDelta::try_from(-2000.0).has_value());
    TG_CHECK(TemperatureDelta::try_from(0.0).has_value());

    TG_PHASE("unvalidated construction reports invalid without clamping");
    TG_CHECK(!DegreesCelsius{kAbsoluteZero - kEpsilon}.is_valid());
    TG_CHECK(!DegreesCelsius{kMaxPlausible + kEpsilon}.is_valid());
    TG_CHECK(DegreesCelsius{}.is_valid());
}

TG_CASE(quantity, make_reports_temperature_invalid) {
    TG_OK(DegreesCelsius::make(kAbsoluteZero));
    TG_OK(DegreesCelsius::make(kMaxPlausible));
    TG_ERROR_CODE(DegreesCelsius::make(kAbsoluteZero - kEpsilon),
                  ThermalErrorCode::TEMPERATURE_INVALID);
    TG_ERROR_CODE(DegreesCelsius::make(kMaxPlausible + kEpsilon),
                  ThermalErrorCode::TEMPERATURE_INVALID);
    TG_ERROR_CODE(DegreesCelsius::make(std::numeric_limits<double>::quiet_NaN()),
                  ThermalErrorCode::TEMPERATURE_INVALID);
}

TG_CASE(quantity, delta_arithmetic_is_exact) {
    const TemperatureDelta quarter{0.25};
    const TemperatureDelta half{0.5};
    const TemperatureDelta other{2.25};

    TG_CHECK_EQ((quarter + quarter).value(), 0.5);
    TG_CHECK_EQ((quarter + quarter + quarter + quarter).value(), 1.0);
    TG_CHECK_EQ((other + half).value(), 2.75);
    TG_CHECK_EQ((other + (-half)).value(), 1.75);
    TG_CHECK_EQ((-other).value(), -2.25);
    TG_CHECK_EQ((-(-other)).value(), 2.25);
    TG_CHECK_EQ((TemperatureDelta{0.0} + other).value(), 2.25);

    TG_CHECK(other + half == TemperatureDelta{2.75});
    TG_CHECK(other > half);
    TG_CHECK(other >= other);
    TG_CHECK(half < other);
    TG_CHECK(TemperatureDelta{} == TemperatureDelta{0.0});
}

TG_CASE(quantity, absolute_temperature_arithmetic_is_exact) {
    const DegreesCelsius base{80.0};
    const TemperatureDelta rise{0.25};
    const TemperatureDelta fall{1.5};

    const DegreesCelsius heated = base + rise;
    const DegreesCelsius cooled = base - fall;
    TG_CHECK_EQ(heated.value(), 80.25);
    TG_CHECK_EQ(cooled.value(), 78.5);
    TG_CHECK(heated == DegreesCelsius{80.25});
    TG_CHECK(heated > cooled);

    const TemperatureDelta difference = heated - cooled;
    TG_CHECK_EQ(difference.value(), 1.75);
    TG_CHECK_EQ((cooled + difference).value(), 80.25);
    // Routed through the validating factory so the operands are genuine
    // runtime values; a wholly constant expression would be folded and the
    // assertion would be checking a compile-time constant.
    const TemperatureDelta drop = TemperatureDelta::try_from(-0.5).value();
    TG_CHECK_EQ((DegreesCelsius{0.0} + drop).value(), -0.5);
}

TG_CASE(quantity, percent_try_from_range) {
    TG_CHECK(Percent::try_from(0.0).has_value());
    TG_CHECK(Percent::try_from(100.0).has_value());

    const auto half = Percent::try_from(12.5);
    TG_CHECK(half.has_value());
    TG_CHECK_EQ(half->value(), 12.5);

    TG_CHECK(!Percent::try_from(-0.25).has_value());
    TG_CHECK(!Percent::try_from(-1.0).has_value());
    TG_CHECK(!Percent::try_from(100.25).has_value());
    TG_CHECK(!Percent::try_from(1000.0).has_value());

    TG_CHECK(Percent{} == Percent{0.0});
    TG_CHECK(Percent{50.0} < Percent{75.0});
}

TG_CASE(quantity, warning_boundary_triple) {
    const ThermalThresholds thresholds = boundary_thresholds();
    const double warning = thresholds.warning.value();

    TG_PHASE("warning threshold triple");
    TG_CHECK(DegreesCelsius{warning - kEpsilon} < thresholds.warning);
    TG_CHECK(DegreesCelsius{warning} == thresholds.warning);
    TG_CHECK(DegreesCelsius{warning + kEpsilon} > thresholds.warning);

    TG_PHASE("near-limit start triple");
    const double start = thresholds.near_limit_start().value();
    TG_CHECK_EQ(start, 77.75);
    TG_CHECK(DegreesCelsius{start - kEpsilon} < thresholds.near_limit_start());
    TG_CHECK(DegreesCelsius{start} == thresholds.near_limit_start());
    TG_CHECK(DegreesCelsius{start + kEpsilon} > thresholds.near_limit_start());

    TG_PHASE("warning sits inside the band");
    TG_CHECK(thresholds.warning < thresholds.derating);
    TG_CHECK(thresholds.warning <= thresholds.near_limit_start());
    TG_CHECK_EQ(thresholds.hysteresis_width().value(), 8.0);
}

TG_CASE(quantity, derating_boundary_triple) {
    const ThermalThresholds thresholds = boundary_thresholds();
    const double derating = thresholds.derating.value();

    TG_PHASE("derating threshold triple");
    TG_CHECK(DegreesCelsius{derating - kEpsilon} < thresholds.derating);
    TG_CHECK(DegreesCelsius{derating} == thresholds.derating);
    TG_CHECK(DegreesCelsius{derating + kEpsilon} > thresholds.derating);

    TG_PHASE("derating is between near-limit start and critical");
    TG_CHECK(thresholds.near_limit_start() < thresholds.derating);
    TG_CHECK(thresholds.derating < thresholds.critical);
    TG_CHECK_EQ((thresholds.derating - thresholds.near_limit_start()).value(), 2.25);
}

TG_CASE(quantity, critical_boundary_triple) {
    const ThermalThresholds thresholds = boundary_thresholds();
    const double critical = thresholds.critical.value();
    const double recovery = thresholds.recovery.value();

    const HeadroomBreakdown below =
        compute_headroom(boundary_input(critical - kEpsilon, critical, recovery, 0.0));
    const HeadroomBreakdown at =
        compute_headroom(boundary_input(critical, critical, recovery, 0.0));
    const HeadroomBreakdown above =
        compute_headroom(boundary_input(critical + kEpsilon, critical, recovery, 0.0));

    TG_CHECK_EQ(below.governing_limit.value(), critical);
    TG_CHECK_EQ(below.raw_headroom.value(), kEpsilon);
    TG_CHECK_EQ(at.raw_headroom.value(), 0.0);
    TG_CHECK_EQ(above.raw_headroom.value(), -kEpsilon);

    TG_CHECK(below.has_positive_headroom());
    TG_CHECK(!at.has_positive_headroom());
    TG_CHECK(!above.has_positive_headroom());
    TG_CHECK(above.effective_headroom.value() < 0.0);
}

TG_CASE(quantity, recovery_boundary_triple) {
    const ThermalThresholds thresholds = boundary_thresholds();
    const double recovery = thresholds.recovery.value();
    const double margin = 1.25;
    const double gate = recovery + margin;

    const HeadroomBreakdown below =
        compute_headroom(boundary_input(gate - kEpsilon, thresholds.critical.value(), recovery, margin));
    const HeadroomBreakdown at =
        compute_headroom(boundary_input(gate, thresholds.critical.value(), recovery, margin));
    const HeadroomBreakdown above =
        compute_headroom(boundary_input(gate + kEpsilon, thresholds.critical.value(), recovery, margin));

    TG_CHECK_EQ(below.distance_above_recovery_gate.value(), -kEpsilon);
    TG_CHECK_EQ(at.distance_above_recovery_gate.value(), 0.0);
    TG_CHECK_EQ(above.distance_above_recovery_gate.value(), kEpsilon);

    TG_CHECK(below.recovery_headroom_satisfied());
    TG_CHECK(!at.recovery_headroom_satisfied());
    TG_CHECK(!above.recovery_headroom_satisfied());

    TG_PHASE("recovery threshold ordering");
    TG_CHECK(thresholds.recovery < thresholds.derating);
    TG_CHECK_EQ((thresholds.derating - thresholds.recovery).value(), 8.0);
}
