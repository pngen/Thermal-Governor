// Thermal Governor — governance integration tests.
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

}  // namespace

TG_CASE(governance, register_and_evaluate) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 65.0));
    auto evaluation = fixture->governor->evaluate_domain(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(evaluation);
    TG_CHECK_EQ(evaluation.value().state, ThermalState::NORMAL);
    TG_CHECK_EQ(evaluation.value().domain_generation.value(), 1U);
    TG_CHECK(evaluation.value().headroom.effective_headroom.value() > 0.0);
}

TG_CASE(governance, duplicate_identical_evidence_is_idempotent) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    auto first = fixture->governor->evaluate_domain(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(first);

    ++fixture->telemetry;
    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{fixture->telemetry}};
    evidence.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{1}},
                                              DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    evidence.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    evidence.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    evidence.temperature = DegreesCelsius{64.0};
    evidence.source = MeasurementSource::SYNTHETIC_MODEL;
    evidence.provenance = Provenance::SYNTHETIC;
    evidence.measurement_sequence = fixture->telemetry;
    evidence.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{fixture->telemetry}};
    evidence.coordinator_epoch = fixture->governor->epoch();
    evidence.measured_at = fixture->clock->now();

    TG_OK(fixture->governor->publish_temperature(evidence));
    auto duplicate = fixture->governor->publish_temperature(evidence);
    TG_OK(duplicate);
    TG_CHECK(!duplicate.value().accepted);
    TG_CHECK_EQ(duplicate.value().comparison, EvidenceComparison::IDENTICAL_DUPLICATE);
}

TG_CASE(governance, conflicting_duplicate_is_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    auto receipt = fixture->publish(1, 64.0);
    TG_OK(receipt);

    // Same evidence identity and sequence, divergent measurement.
    ThermalEvidence conflicting;
    conflicting.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{fixture->telemetry}};
    conflicting.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{1}},
                                                 DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    conflicting.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    conflicting.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    conflicting.temperature = DegreesCelsius{99.0};
    conflicting.source = MeasurementSource::SYNTHETIC_MODEL;
    conflicting.provenance = Provenance::SYNTHETIC;
    conflicting.measurement_sequence = fixture->telemetry;
    conflicting.telemetry_generation =
        TelemetryGeneration{StrongId<TelemetryGenerationTag>{fixture->telemetry}};
    conflicting.coordinator_epoch = fixture->governor->epoch();
    conflicting.measured_at = fixture->clock->now();

    TG_ERROR_CODE(fixture->governor->publish_temperature(conflicting),
                  ThermalErrorCode::DUPLICATE_CONFLICT);
}

TG_CASE(governance, stale_coordinator_epoch_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 64.0));

    ThermalEvidence stale;
    stale = ThermalEvidence{};
    stale.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{999}};
    stale.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{1}},
                                           DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    stale.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    stale.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    stale.temperature = DegreesCelsius{50.0};
    stale.source = MeasurementSource::SYNTHETIC_MODEL;
    stale.provenance = Provenance::SYNTHETIC;
    stale.measurement_sequence = 999;
    stale.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{999}};
    stale.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{7}};
    stale.measured_at = fixture->clock->now();

    TG_ERROR_CODE(fixture->governor->publish_temperature(stale), ThermalErrorCode::STALE_EPOCH);
}

TG_CASE(governance, stale_device_generation_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 64.0, 1, 1, 1));
    // Advance the device generation, then send evidence bound to the old one.
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
    TG_ERROR_CODE(fixture->publish(1, 60.0, 1, 1, 1),
                  ThermalErrorCode::STALE_DEVICE_GENERATION);
}

TG_CASE(governance, stale_domain_generation_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 64.0));

    ThermalDomainDefinition definition;
    definition.id = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    definition.generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{2}};
    definition.type = ThermalDomainType::ACCELERATOR;
    definition.label = Label{"domain-1"};
    definition.provenance = Provenance::SYNTHETIC;
    definition.evidence_source = "synthetic test device";
    definition.policy = ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}};
    DomainMember member;
    member.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{1}},
                                            DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    definition.members.push_back(member);
    TG_STATUS_OK(fixture->governor->update_thermal_domain(definition));

    TG_ERROR_CODE(fixture->publish(1, 60.0, 1, 1, 1),
                  ThermalErrorCode::STALE_DOMAIN_GENERATION);
}

TG_CASE(governance, stale_telemetry_generation_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 64.0));
    TG_OK(fixture->publish(1, 65.0));

    ThermalEvidence older;
    older.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{1}};
    older.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{1}},
                                           DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    older.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    older.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    older.temperature = DegreesCelsius{10.0};
    older.source = MeasurementSource::SYNTHETIC_MODEL;
    older.provenance = Provenance::SYNTHETIC;
    older.measurement_sequence = 1;
    older.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{1}};
    older.coordinator_epoch = fixture->governor->epoch();
    older.measured_at = fixture->clock->now();

    TG_ERROR_CODE(fixture->governor->publish_temperature(older),
                  ThermalErrorCode::STALE_TELEMETRY);
}

TG_CASE(governance, provenance_overstatement_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    // Claiming a modelled measurement as REAL must be rejected outright.
    ThermalEvidence overstated;
    overstated.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{1}};
    overstated.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{1}},
                                                DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    overstated.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    overstated.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    overstated.temperature = DegreesCelsius{40.0};
    overstated.source = MeasurementSource::SYNTHETIC_MODEL;
    overstated.provenance = Provenance::REAL;
    overstated.measurement_sequence = 1;
    overstated.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{1}};
    overstated.coordinator_epoch = fixture->governor->epoch();
    overstated.measured_at = fixture->clock->now();
    TG_ERROR_CODE(fixture->governor->publish_temperature(overstated),
                  ThermalErrorCode::INVALID_ARGUMENT);
}

TG_CASE(governance, hard_constraint_dominates_favourable_headroom) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    // A very low thermal intensity workload does not rescue a critical state.
    TG_OK(fixture->publish(1, 95.0));
    ThermalAdmissionRequest request;
    request.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    request.workload = WorkloadId{StrongId<WorkloadIdTag>{1}};
    request.profile = WorkloadThermalProfile::LOW_THERMAL_INTENSITY;
    auto admission = fixture->governor->evaluate_admission(request);
    TG_OK(admission);
    TG_CHECK_EQ(admission.value().decision, AdmissionDecision::DENY);
}

TG_CASE(governance, admission_derates_on_marginal_headroom) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    // 85 degrees: raw headroom 3, effective headroom 0 after margins.
    TG_OK(fixture->publish(1, 85.0));
    ThermalAdmissionRequest request;
    request.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    request.workload = WorkloadId{StrongId<WorkloadIdTag>{1}};
    request.profile = WorkloadThermalProfile::HIGH_THERMAL_INTENSITY;
    auto admission = fixture->governor->evaluate_admission(request);
    TG_OK(admission);
    TG_CHECK_EQ(admission.value().decision, AdmissionDecision::ADMIT_DERATED);
    TG_CHECK(admission.value().permitted_concurrency.value() < 100.0);
}

TG_CASE(governance, admission_denies_excluded_workload_profile) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 40.0));
    ThermalAdmissionRequest request;
    request.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    request.workload = WorkloadId{StrongId<WorkloadIdTag>{1}};
    request.profile = static_cast<WorkloadThermalProfile>(200);
    auto admission = fixture->governor->evaluate_admission(request);
    TG_OK(admission);
    TG_CHECK_EQ(admission.value().decision, AdmissionDecision::DENY);
    TG_CHECK(admission.value().reasons.contains(ThermalReasonCode::WORKLOAD_PROFILE_EXCLUDED));
}

TG_CASE(governance, stale_policy_generation_in_admission_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 40.0));
    ThermalAdmissionRequest request;
    request.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    request.workload = WorkloadId{StrongId<WorkloadIdTag>{1}};
    request.profile = WorkloadThermalProfile::MODERATE_THERMAL_INTENSITY;
    request.expected_policy_generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{99}};
    TG_ERROR_CODE(fixture->governor->evaluate_admission(request),
                  ThermalErrorCode::STALE_POLICY);
}

TG_CASE(governance, vendor_limit_governs_only_when_real) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    TG_STATUS_OK(fixture->add_device(1, 1, CapabilityState::SUPPORTED_SYNTHETIC,
                                     CapabilityState::UNSUPPORTED,
                                     CapabilityState::SUPPORTED_SYNTHETIC));
    TG_STATUS_OK(fixture->add_accelerator_domain(1, 1, 1));
    // A SYNTHETIC limit must never tighten the governing ceiling.
    TG_OK(fixture->publish(1, 40.0, 1, 1, 1, WorkerId{}, WorkerBootId{}, std::nullopt,
                           Provenance::SYNTHETIC, std::nullopt, 45.0));
    auto headroom = fixture->governor->query_headroom(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(headroom);
    TG_CHECK_EQ(headroom.value().governing_limit.value(), 88.0);
}

TG_CASE(governance, envelope_is_generation_bound) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 70.0));
    auto envelope = fixture->governor->query_envelope(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(envelope);
    TG_CHECK_EQ(envelope.value().domain_generation.value(), 1U);
    TG_CHECK_EQ(envelope.value().coordinator_epoch.value(), fixture->governor->epoch().value());
    TG_CHECK(envelope.value().policy_generation.value() >= 1U);
    TG_CHECK(envelope.value().telemetry_generation.value() >= 1U);
    TG_CHECK(envelope.value().permits_full_capability());
}

TG_CASE(governance, explanation_is_byte_identical_for_equal_inputs) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 79.0));
    auto first = fixture->governor->explain(ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(first);
    auto second = fixture->governor->explain(ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(second);
    TG_CHECK_EQ(first.value().render(), second.value().render());
    TG_CHECK(first.value().render().find("EffectiveHeadroom") != std::string::npos);
}

TG_CASE(governance, unknown_domain_is_a_typed_error) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    TG_ERROR_CODE(fixture->governor->evaluate_domain(
                      ThermalDomainId{StrongId<ThermalDomainIdTag>{42}}),
                  ThermalErrorCode::UNKNOWN_DOMAIN);
    TG_ERROR_CODE(fixture->governor->query_envelope(
                      ThermalDomainId{StrongId<ThermalDomainIdTag>{42}}),
                  ThermalErrorCode::UNKNOWN_DOMAIN);
}

TG_CASE(governance, domain_provenance_must_be_explicit) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    TG_STATUS_OK(fixture->add_device(1));
    ThermalDomainDefinition definition;
    definition.id = ThermalDomainId{StrongId<ThermalDomainIdTag>{5}};
    definition.generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    definition.type = ThermalDomainType::RACK;
    definition.provenance = Provenance::UNKNOWN;
    TG_STATUS_ERROR_CODE(fixture->governor->register_thermal_domain(definition),
                         ThermalErrorCode::INVALID_ARGUMENT);
    definition.provenance = Provenance::SYNTHETIC;
    TG_STATUS_OK(fixture->governor->register_thermal_domain(definition));
}
