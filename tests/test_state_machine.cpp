// Thermal Governor — deterministic thermal state machine tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "framework.hpp"
#include "governor_fixture.hpp"

namespace {

using namespace thermal_governor;
using tg::GovernorFixture;
using tg::GovernorFixture;

[[nodiscard]] ThermalState state_of(GovernorFixture& fixture, std::uint64_t domain = 1) {
    auto evaluation = fixture.governor->evaluate_domain(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{domain}});
    if (!evaluation.has_value()) {
        return ThermalState::UNSUPPORTED;
    }
    return evaluation.value().state;
}

[[nodiscard]] ThermalDecision decision_of(GovernorFixture& fixture, std::uint64_t domain = 1) {
    auto evaluation = fixture.governor->evaluate_domain(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{domain}});
    if (!evaluation.has_value()) {
        return ThermalDecision::UNSUPPORTED;
    }
    return evaluation.value().decision;
}

void prepare(tg::Context& tg_ctx, GovernorFixture& fixture) {
    TG_STATUS_OK(fixture.add_device(1));
    TG_STATUS_OK(fixture.add_accelerator_domain(1, 1, 1));
}

}  // namespace

// The default policy used throughout: warning 75, near-limit start 77,
// derating 80, critical 88, recovery 72, recovery margin 1.
TG_CASE(state_machine, normal_below_warning) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 60.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::NORMAL);
    TG_CHECK_EQ(decision_of(*fixture), ThermalDecision::ALLOW);
}

TG_CASE(state_machine, warm_at_warning_boundary) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 74.999));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::NORMAL);
    TG_OK(fixture->publish(1, 75.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::WARM);
    TG_OK(fixture->publish(1, 75.001));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::WARM);
}

TG_CASE(state_machine, near_limit_band_boundary) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 76.999));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::WARM);
    TG_OK(fixture->publish(1, 77.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::NEAR_LIMIT);
    TG_OK(fixture->publish(1, 77.001));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::NEAR_LIMIT);
}

TG_CASE(state_machine, derating_boundary) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 79.999));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::NEAR_LIMIT);
    TG_OK(fixture->publish(1, 80.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
    TG_CHECK_EQ(decision_of(*fixture), ThermalDecision::ALLOW_DERATED);
    TG_OK(fixture->publish(1, 80.001));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
}

TG_CASE(state_machine, critical_boundary) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 87.999));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
    TG_OK(fixture->publish(1, 88.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::CRITICAL);
    TG_CHECK_EQ(decision_of(*fixture), ThermalDecision::DENY);
}

TG_CASE(state_machine, critical_never_allows) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 120.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::CRITICAL);
    TG_CHECK(decision_of(*fixture) != ThermalDecision::ALLOW);
    TG_CHECK(decision_of(*fixture) != ThermalDecision::ALLOW_DERATED);
}

TG_CASE(state_machine, throttling_distinct_from_derating) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    TG_STATUS_OK(fixture->add_device(1, 1, CapabilityState::SUPPORTED_SYNTHETIC,
                                     CapabilityState::SUPPORTED_SYNTHETIC));
    TG_STATUS_OK(fixture->add_accelerator_domain(1, 1, 1));
    // A cool but genuinely throttled device is THROTTLING, not DERATED.
    TG_OK(fixture->publish(1, 60.0, 1, 1, 1, WorkerId{}, WorkerBootId{},
                           ThrottleObservation::from_raw(bits(ThrottleReasonBit::HW_THERMAL_SLOWDOWN))));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::THROTTLING);
    // A hot but unthrottled device is DERATED, not THROTTLING.
    TG_OK(fixture->publish(1, 82.0, 1, 1, 1, WorkerId{}, WorkerBootId{}, ThrottleObservation::from_raw(0)));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
}

TG_CASE(state_machine, non_thermal_throttle_is_not_thermal_throttling) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    TG_STATUS_OK(fixture->add_device(1, 1, CapabilityState::SUPPORTED_SYNTHETIC,
                                     CapabilityState::SUPPORTED_SYNTHETIC));
    TG_STATUS_OK(fixture->add_accelerator_domain(1, 1, 1));
    TG_OK(fixture->publish(1, 55.0, 1, 1, 1, WorkerId{}, WorkerBootId{},
                           ThrottleObservation::from_raw(bits(ThrottleReasonBit::SW_POWER_CAP))));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::NORMAL);
    auto evaluation = fixture->governor->evaluate_domain(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(evaluation);
    TG_CHECK_EQ(evaluation.value().throttle_class, ThrottleClass::NON_THERMAL_THROTTLE_OBSERVED);
}

TG_CASE(state_machine, hysteresis_retains_derated) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 80.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
    // 79 is below the derating threshold but far above the recovery gate:
    // the state must be retained, not relaxed.
    TG_OK(fixture->publish(1, 79.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
    TG_OK(fixture->publish(1, 78.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
    auto evaluation = fixture->governor->evaluate_domain(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(evaluation);
    TG_CHECK(evaluation.value().retained_by_hysteresis);
    TG_CHECK(evaluation.value().reasons.contains(ThermalReasonCode::STATE_RETAINED_BY_HYSTERESIS));
}

TG_CASE(state_machine, recovery_after_required_samples) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
    TG_OK(fixture->publish_after(0, 1, 70.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
    TG_OK(fixture->publish_after(1000, 1, 70.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
    TG_OK(fixture->publish_after(1000, 1, 70.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::NORMAL);
    TG_CHECK_EQ(decision_of(*fixture), ThermalDecision::ALLOW);
}

TG_CASE(state_machine, one_cool_sample_cannot_bypass_hysteresis) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    TG_OK(fixture->publish_after(5000, 1, 60.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
}

TG_CASE(state_machine, absent_evidence_is_not_safe) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    const ThermalState state = state_of(*fixture);
    TG_CHECK(state == ThermalState::REVALIDATION_REQUIRED || state == ThermalState::UNKNOWN);
    TG_CHECK(decision_of(*fixture) != ThermalDecision::ALLOW);
}

TG_CASE(state_machine, unknown_capability_stays_unknown) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    TG_STATUS_OK(fixture->add_device(1, 1, CapabilityState::UNKNOWN));
    TG_STATUS_OK(fixture->add_accelerator_domain(1, 1, 1));
    TG_OK(fixture->publish(1, 40.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::UNKNOWN);
    TG_CHECK_EQ(decision_of(*fixture), ThermalDecision::UNKNOWN);
}

TG_CASE(state_machine, unsupported_capability_stays_unsupported) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    TG_STATUS_OK(fixture->add_device(1, 1, CapabilityState::UNSUPPORTED));
    TG_STATUS_OK(fixture->add_accelerator_domain(1, 1, 1));
    TG_OK(fixture->publish(1, 40.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::UNSUPPORTED);
    TG_CHECK_EQ(decision_of(*fixture), ThermalDecision::UNSUPPORTED);
}

TG_CASE(state_machine, retained_state_never_relaxes_without_recovery) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 95.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::CRITICAL);
    // Falling below the critical threshold does not restore full authority.
    TG_OK(fixture->publish(1, 85.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
    TG_OK(fixture->publish(1, 76.0));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::DERATED);
}

TG_CASE(state_machine, derating_never_expands_capability) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 82.0));
    auto high = fixture->governor->evaluate_domain(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(high);
    TG_CHECK(!does_not_expand(DeratingLevel::FULL_CAPABILITY, high.value().derating));
    TG_CHECK(does_not_expand(high.value().derating, DeratingLevel::FULL_CAPABILITY));
}
