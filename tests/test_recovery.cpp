// Thermal Governor — recovery hysteresis and gating tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "framework.hpp"
#include "governor_fixture.hpp"

namespace {

using namespace thermal_governor;
using tg::GovernorFixture;
using tg::GovernorFixture;

void prepare(tg::Context& tg_ctx, GovernorFixture& fixture) {
    TG_STATUS_OK(fixture.add_device(1));
    TG_STATUS_OK(fixture.add_accelerator_domain(1, 1, 1));
}

[[nodiscard]] RecoveryAssessment assess(GovernorFixture& fixture) {
    auto assessment = fixture.governor->evaluate_recovery(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    if (!assessment.has_value()) {
        RecoveryAssessment empty;
        return empty;
    }
    return assessment.value();
}

}  // namespace

TG_CASE(recovery, forbidden_above_recovery_threshold) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    const RecoveryAssessment assessment = assess(*fixture);
    TG_CHECK(!assessment.allowed);
    TG_CHECK(!assessment.temperature_below_threshold);
    TG_CHECK(assessment.blocking_reasons.contains(ThermalReasonCode::RECOVERY_THRESHOLD_EXCEEDED));
}

TG_CASE(recovery, sample_count_blocks_recovery) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    TG_OK(fixture->publish_after(1000, 1, 70.0));
    TG_OK(fixture->publish_after(1000, 1, 70.0));
    const RecoveryAssessment assessment = assess(*fixture);
    TG_CHECK(!assessment.allowed);
    TG_CHECK(assessment.blocking_reasons.contains(
        ThermalReasonCode::RECOVERY_SAMPLES_INSUFFICIENT));
    TG_OK(fixture->publish_after(1000, 1, 70.0));
    TG_CHECK(assess(*fixture).allowed);
}

TG_CASE(recovery, evidence_span_blocks_recovery) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    TG_OK(fixture->publish(1, 70.0));
    TG_OK(fixture->publish(1, 70.0));
    TG_OK(fixture->publish(1, 70.0));
    const RecoveryAssessment assessment = assess(*fixture);
    TG_CHECK(!assessment.allowed);
    TG_CHECK(assessment.blocking_reasons.contains(
        ThermalReasonCode::RECOVERY_EVIDENCE_SPAN_INSUFFICIENT));
}

TG_CASE(recovery, thermal_throttle_blocks_recovery) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    TG_STATUS_OK(fixture->add_device(1, 1, CapabilityState::SUPPORTED_SYNTHETIC,
                                     CapabilityState::SUPPORTED_SYNTHETIC));
    TG_STATUS_OK(fixture->add_accelerator_domain(1, 1, 1));
    TG_OK(fixture->publish(1, 85.0, 1, 1, 1, WorkerId{}, WorkerBootId{},
                           ThrottleObservation::from_raw(bits(ThrottleReasonBit::HW_THERMAL_SLOWDOWN))));
    // Cool, but still reporting thermal throttle: recovery stays forbidden.
    TG_OK(fixture->publish(1, 70.0, 1, 1, 1, WorkerId{}, WorkerBootId{},
                           ThrottleObservation::from_raw(bits(ThrottleReasonBit::HW_THERMAL_SLOWDOWN))));
    TG_OK(fixture->publish_after(1000, 1, 70.0));
    TG_OK(fixture->publish_after(1000, 1, 70.0));
    const RecoveryAssessment assessment = assess(*fixture);
    TG_CHECK(!assessment.allowed);
    TG_CHECK(assessment.blocking_reasons.contains(
        ThermalReasonCode::RECOVERY_BLOCKED_BY_THROTTLE_EVIDENCE));
}

TG_CASE(recovery, explicit_authorization_requirement) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    auto policy = fixture->governor->get_policy(ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}});
    TG_OK(policy);
    ThermalPolicy updated = policy.value();
    updated.generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{2}};
    updated.recovery.require_explicit_authorization = true;
    TG_OK(fixture->governor->set_policy(updated));

    TG_OK(fixture->publish(1, 85.0));
    TG_OK(fixture->publish_after(1000, 1, 70.0));
    TG_OK(fixture->publish_after(1000, 1, 70.0));
    TG_OK(fixture->publish_after(1000, 1, 70.0));
    RecoveryAssessment assessment = assess(*fixture);
    TG_CHECK(!assessment.allowed);
    TG_CHECK(assessment.blocking_reasons.contains(
        ThermalReasonCode::RECOVERY_REQUIRES_EXPLICIT_AUTHORIZATION));

    TG_STATUS_OK(fixture->governor->authorize_recovery(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, "operator approved"));
    assessment = assess(*fixture);
    TG_CHECK(assessment.allowed);
}

TG_CASE(recovery, generation_binding_blocks_recovery) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    TG_OK(fixture->publish_after(1000, 1, 70.0));
    TG_OK(fixture->publish_after(1000, 1, 70.0));
    // Advance the device generation: every earlier witness is invalidated.
    DeviceRegistration updated;
    updated.device = DeviceId{StrongId<DeviceIdTag>{1}};
    updated.generation = DeviceGeneration{StrongId<DeviceGenerationTag>{2}};
    updated.node = NodeId{StrongId<NodeIdTag>{1}};
    updated.node_generation = NodeGeneration{StrongId<NodeGenerationTag>{1}};
    updated.rack = RackId{StrongId<RackIdTag>{1}};
    updated.rack_generation = RackGeneration{StrongId<RackGenerationTag>{1}};
    updated.label = Label{"device-1"};
    updated.provenance = Provenance::SYNTHETIC;
    updated.capabilities.set(ThermalCapability::TEMPERATURE, CapabilityState::SUPPORTED_SYNTHETIC);
    TG_STATUS_OK(fixture->governor->register_device(updated));
    fixture->advance(1000);
    TG_OK(fixture->publish(1, 70.0, 1, 1, 2));

    // Recovery must not be granted on witnesses from a superseded generation.
    const RecoveryAssessment assessment = assess(*fixture);
    TG_CHECK(assessment.qualifying_samples <= 1);
}

TG_CASE(recovery, recovery_threshold_boundary) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    // Recovery gate is recovery + margin = 73.0 degrees.
    TG_OK(fixture->publish_after(1000, 1, 73.0));
    TG_OK(fixture->publish_after(1000, 1, 73.0));
    TG_OK(fixture->publish_after(1000, 1, 73.0));
    TG_CHECK(!assess(*fixture).allowed);
    TG_OK(fixture->publish_after(0, 1, 72.999));
    TG_OK(fixture->publish_after(1000, 1, 72.999));
    TG_OK(fixture->publish_after(1000, 1, 72.999));
    TG_CHECK(assess(*fixture).allowed);
}

TG_CASE(recovery, assessment_render_is_stable) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    const RecoveryAssessment first = assess(*fixture);
    const RecoveryAssessment second = assess(*fixture);
    TG_CHECK_EQ(first.render(), second.render());
}
