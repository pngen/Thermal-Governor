// Thermal Governor — action authority and lifecycle tests.
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

[[nodiscard]] ActionId authorize(GovernorFixture& fixture, MitigationIntent intent) {
    MitigationIntentRecord record;
    record.intent = intent;
    auto action = fixture.governor->authorize_mitigation(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "test");
    if (!action.has_value()) {
        return ActionId{};
    }
    return action.value();
}

}  // namespace

TG_CASE(actions, acknowledged_is_not_effective) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));

    const ActionId id = authorize(*fixture, MitigationIntent::REQUEST_CLOCK_REDUCTION);
    TG_CHECK(id.is_valid());
    TG_CHECK_EQ(fixture->governor->get_action(id).value().lifecycle, ActionLifecycle::AUTHORIZED);

    TG_OK(fixture->governor->record_action_dispatch(id));
    TG_CHECK_EQ(fixture->governor->get_action(id).value().lifecycle, ActionLifecycle::DISPATCHED);

    const auto action = fixture->governor->get_action(id).value();
    ActionAck ack;
    ack.action_id = id;
    ack.action_generation = action.generation;
    ack.coordinator_epoch = fixture->governor->epoch();
    ack.worker_boot = action.authority.worker_boot;
    ack.device_generation = action.authority.device_generation;
    ack.domain_generation = action.authority.domain_generation;
    ack.accepted = true;
    ack.detail = "backend accepted";
    TG_OK(fixture->governor->record_action_ack(ack));
    TG_CHECK_EQ(fixture->governor->get_action(id).value().lifecycle,
                ActionLifecycle::ACKNOWLEDGED);

    ActionResult result;
    result.action_id = id;
    result.action_generation = action.generation;
    result.coordinator_epoch = fixture->governor->epoch();
    result.worker_boot = action.authority.worker_boot;
    result.device_generation = action.authority.device_generation;
    result.domain_generation = action.authority.domain_generation;
    result.policy_generation = action.authority.policy_generation;
    result.telemetry_generation = action.authority.telemetry_generation;
    result.backend_succeeded = true;
    result.detail = "backend reported success";
    TG_OK(fixture->governor->record_action_result(result));
    // A backend success is a claim, not proof of thermal effect.
    TG_CHECK_EQ(fixture->governor->get_action(id).value().lifecycle, ActionLifecycle::VERIFYING);
}

TG_CASE(actions, verification_requires_fresh_evidence) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));

    const ActionId id = authorize(*fixture, MitigationIntent::REQUEST_CLOCK_REDUCTION);
    TG_CHECK(id.is_valid());
    TG_OK(fixture->governor->record_action_dispatch(id));

    const auto action = fixture->governor->get_action(id).value();
    ActionAck ack;
    ack.action_id = id;
    ack.action_generation = action.generation;
    ack.coordinator_epoch = fixture->governor->epoch();
    ack.worker_boot = action.authority.worker_boot;
    ack.device_generation = action.authority.device_generation;
    ack.domain_generation = action.authority.domain_generation;
    ack.accepted = true;
    TG_OK(fixture->governor->record_action_ack(ack));

    // An observation taken at the dispatch instant cannot prove effect: the
    // clock has not advanced, so nothing was measured after the dispatch.
    ++fixture->telemetry;
    ThermalEvidence same_instant;
    same_instant.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{fixture->telemetry}};
    same_instant.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{1}},
                                                  DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    same_instant.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    same_instant.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    same_instant.temperature = DegreesCelsius{70.0};
    same_instant.source = MeasurementSource::SYNTHETIC_MODEL;
    same_instant.provenance = Provenance::SYNTHETIC;
    same_instant.measurement_sequence = fixture->telemetry;
    same_instant.telemetry_generation =
        TelemetryGeneration{StrongId<TelemetryGenerationTag>{fixture->telemetry}};
    same_instant.coordinator_epoch = fixture->governor->epoch();
    same_instant.measured_at = fixture->clock->now();
    TG_ERROR_CODE(fixture->governor->verify_action(id, same_instant),
                  ThermalErrorCode::EVIDENCE_STALE);
}

TG_CASE(actions, verification_after_fresh_evidence) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));

    const ActionId id = authorize(*fixture, MitigationIntent::REQUEST_CLOCK_REDUCTION);
    TG_CHECK(id.is_valid());
    TG_OK(fixture->governor->record_action_dispatch(id));

    const auto action = fixture->governor->get_action(id).value();
    ActionAck ack;
    ack.action_id = id;
    ack.action_generation = action.generation;
    ack.coordinator_epoch = fixture->governor->epoch();
    ack.worker_boot = action.authority.worker_boot;
    ack.device_generation = action.authority.device_generation;
    ack.domain_generation = action.authority.domain_generation;
    ack.accepted = true;
    TG_OK(fixture->governor->record_action_ack(ack));

    ActionResult result;
    result.action_id = id;
    result.action_generation = action.generation;
    result.coordinator_epoch = fixture->governor->epoch();
    result.worker_boot = action.authority.worker_boot;
    result.device_generation = action.authority.device_generation;
    result.domain_generation = action.authority.domain_generation;
    result.policy_generation = action.authority.policy_generation;
    result.telemetry_generation = action.authority.telemetry_generation;
    result.backend_succeeded = true;
    TG_OK(fixture->governor->record_action_result(result));

    fixture->advance(1000);
    ++fixture->telemetry;
    ThermalEvidence fresh;
    fresh.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{fixture->telemetry}};
    fresh.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{1}},
                                           DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    fresh.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    fresh.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    fresh.temperature = DegreesCelsius{62.0};
    fresh.source = MeasurementSource::SYNTHETIC_MODEL;
    fresh.provenance = Provenance::SYNTHETIC;
    fresh.measurement_sequence = fixture->telemetry;
    fresh.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{fixture->telemetry}};
    fresh.coordinator_epoch = fixture->governor->epoch();
    fresh.measured_at = fixture->clock->now();

    auto verification = fixture->governor->verify_action(id, fresh);
    TG_OK(verification);
    TG_EXPECT(verification.value().outcome == VerificationOutcome::THERMAL_STATE_IMPROVED ||
              verification.value().outcome == VerificationOutcome::DERATING_EFFECTIVE ||
              verification.value().outcome == VerificationOutcome::DERATING_PARTIALLY_EFFECTIVE);
    TG_CHECK_EQ(verification.value().temperature_after.value(), 62.0);
    TG_CHECK(verification.value().delta.value() < 0.0);
    TG_CHECK(is_terminal(fixture->governor->get_action(id).value().lifecycle));
}

TG_CASE(actions, stale_action_generation_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    const ActionId id = authorize(*fixture, MitigationIntent::REQUEST_CLOCK_REDUCTION);
    TG_CHECK(id.is_valid());
    TG_OK(fixture->governor->record_action_dispatch(id));

    const auto action = fixture->governor->get_action(id).value();
    ActionAck ack;
    ack.action_id = id;
    ack.action_generation = ActionGeneration{StrongId<ActionGenerationTag>{action.generation.value() + 5}};
    ack.coordinator_epoch = fixture->governor->epoch();
    ack.domain_generation = action.authority.domain_generation;
    ack.accepted = true;
    TG_ERROR_CODE(fixture->governor->record_action_ack(ack), ThermalErrorCode::STALE_ACTION);
}

TG_CASE(actions, stale_epoch_ack_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    const ActionId id = authorize(*fixture, MitigationIntent::REQUEST_CLOCK_REDUCTION);
    TG_OK(fixture->governor->record_action_dispatch(id));

    const auto action = fixture->governor->get_action(id).value();
    ActionAck ack;
    ack.action_id = id;
    ack.action_generation = action.generation;
    ack.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{99}};
    ack.domain_generation = action.authority.domain_generation;
    ack.accepted = true;
    TG_ERROR_CODE(fixture->governor->record_action_ack(ack), ThermalErrorCode::STALE_EPOCH);
}

TG_CASE(actions, stale_domain_generation_result_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    const ActionId id = authorize(*fixture, MitigationIntent::REQUEST_CLOCK_REDUCTION);
    TG_OK(fixture->governor->record_action_dispatch(id));
    const auto action = fixture->governor->get_action(id).value();

    ActionResult result;
    result.action_id = id;
    result.action_generation = action.generation;
    result.coordinator_epoch = fixture->governor->epoch();
    result.device_generation = action.authority.device_generation;
    result.domain_generation =
        ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{42}};
    result.policy_generation = action.authority.policy_generation;
    result.backend_succeeded = true;
    TG_ERROR_CODE(fixture->governor->record_action_result(result),
                  ThermalErrorCode::STALE_DOMAIN_GENERATION);
}

TG_CASE(actions, cancel_before_dispatch_is_honest) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    const ActionId id = authorize(*fixture, MitigationIntent::REQUEST_CLOCK_REDUCTION);
    auto cancelled = fixture->governor->cancel_action(id, "operator");
    TG_OK(cancelled);
    TG_CHECK_EQ(cancelled.value(), CancellationResult::CANCELLED_BEFORE_DISPATCH);
    TG_CHECK_EQ(fixture->governor->get_action(id).value().lifecycle, ActionLifecycle::CANCELLED);
}

TG_CASE(actions, cancel_after_dispatch_requires_verification) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    const ActionId id = authorize(*fixture, MitigationIntent::REQUEST_CLOCK_REDUCTION);
    TG_OK(fixture->governor->record_action_dispatch(id));
    auto cancelled = fixture->governor->cancel_action(id, "operator");
    TG_OK(cancelled);
    TG_CHECK_EQ(cancelled.value(), CancellationResult::VERIFICATION_REQUIRED);
}

TG_CASE(actions, policy_forbidden_intent_is_infeasible) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));

    auto policy = fixture->governor->get_policy(ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}});
    TG_OK(policy);
    ThermalPolicy restricted = policy.value();
    restricted.generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{2}};
    restricted.allowed_intents = 0;
    TG_OK(fixture->governor->set_policy(restricted));

    MitigationIntentRecord record;
    record.intent = MitigationIntent::REQUEST_CONTAINMENT;
    TG_ERROR_CODE(fixture->governor->authorize_mitigation(
                      ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "test"),
                  ThermalErrorCode::ACTION_INFEASIBLE);
}

TG_CASE(actions, critical_state_forbids_restoration) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 120.0));
    MitigationIntentRecord record;
    record.intent = MitigationIntent::REQUEST_CLOCK_RESTORE;
    TG_ERROR_CODE(fixture->governor->authorize_mitigation(
                      ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "test"),
                  ThermalErrorCode::ACTION_INFEASIBLE);
}

TG_CASE(actions, no_action_is_not_actionable) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    MitigationIntentRecord record;
    record.intent = MitigationIntent::NO_ACTION;
    TG_ERROR_CODE(fixture->governor->authorize_mitigation(
                      ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "test"),
                  ThermalErrorCode::ACTION_INFEASIBLE);
}

TG_CASE(actions, terminal_action_cannot_be_verified_again) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    const ActionId id = authorize(*fixture, MitigationIntent::REQUEST_CLOCK_REDUCTION);
    TG_OK(fixture->governor->cancel_action(id, "operator"));

    fixture->advance(1000);
    ++fixture->telemetry;
    ThermalEvidence fresh;
    fresh.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{fixture->telemetry}};
    fresh.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{1}},
                                           DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    fresh.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    fresh.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    fresh.temperature = DegreesCelsius{60.0};
    fresh.source = MeasurementSource::SYNTHETIC_MODEL;
    fresh.provenance = Provenance::SYNTHETIC;
    fresh.measurement_sequence = fixture->telemetry;
    fresh.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{fixture->telemetry}};
    fresh.coordinator_epoch = fixture->governor->epoch();
    fresh.measured_at = fixture->clock->now();
    TG_ERROR_CODE(fixture->governor->verify_action(id, fresh),
                  ThermalErrorCode::ACTION_SUPERSEDED);
}
