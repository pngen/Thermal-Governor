// Thermal Governor — adversarial hardening tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Each case deliberately attacks the runtime. A reproducible material defect
// found here is a defect to fix, not to document away.

#include <string>
#include <vector>

#include "framework.hpp"
#include "governor_fixture.hpp"
#include "thermal_governor/persistence.hpp"
#include "thermal_governor/protocol.hpp"

namespace {

using namespace thermal_governor;
using tg::GovernorFixture;
using tg::GovernorFixture;

void prepare(tg::Context& tg_ctx, GovernorFixture& fixture) {
    TG_STATUS_OK(fixture.add_device(1));
    TG_STATUS_OK(fixture.add_accelerator_domain(1, 1, 1));
}

/// Build an evidence record with explicit authority metadata.
[[nodiscard]] ThermalEvidence craft(const GovernorFixture& fixture, std::uint64_t device,
                                    double temperature, std::uint64_t sequence,
                                    CoordinatorEpoch epoch, std::uint64_t domain = 1) {
    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{sequence}};
    evidence.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{device}},
                                              DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    evidence.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{domain}};
    evidence.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    evidence.temperature = DegreesCelsius{temperature};
    evidence.source = MeasurementSource::SYNTHETIC_MODEL;
    evidence.provenance = Provenance::SYNTHETIC;
    evidence.measurement_sequence = sequence;
    evidence.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{sequence}};
    evidence.coordinator_epoch = epoch;
    evidence.measured_at = fixture.clock->now();
    return evidence;
}

[[nodiscard]] DurableState make_durable() {
    DurableState state;
    state.format_version = kPersistenceFormatVersion;
    state.coordinator = CoordinatorId{StrongId<CoordinatorIdTag>{1}};
    state.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{1}};
    state.policy_generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{1}};
    state.topology_generation = TopologyGeneration{StrongId<TopologyGenerationTag>{1}};
    state.policies.push_back(ThermalPolicy::make_default());
    return state;
}

}  // namespace

TG_CASE(adversarial, stale_epoch_cannot_mutate_state) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 60.0));
    auto stale = craft(*fixture, 1, 10.0, 900,
                       CoordinatorEpoch{StrongId<CoordinatorEpochTag>{fixture->governor->epoch().value() + 1}});
    TG_ERROR_CODE(fixture->governor->publish_temperature(stale), ThermalErrorCode::STALE_EPOCH);
    auto evaluation = fixture->governor->evaluate_domain(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(evaluation);
    TG_CHECK_EQ(evaluation.value().headroom.current_temperature.value(), 60.0);
}

TG_CASE(adversarial, stale_boot_cannot_mutate_state) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_STATUS_OK(fixture->governor->register_worker(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{2}},
        Label{"current"}));
    ThermalEvidence evidence = craft(*fixture, 1, 55.0, 1, fixture->governor->epoch());
    evidence.worker = WorkerId{StrongId<WorkerIdTag>{1}};
    evidence.worker_boot = WorkerBootId{StrongId<WorkerBootIdTag>{1}};
    TG_ERROR_CODE(fixture->governor->publish_temperature(evidence),
                  ThermalErrorCode::STALE_WORKER);
}

TG_CASE(adversarial, stale_device_generation_cannot_mutate_state) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 60.0));
    ThermalEvidence evidence = craft(*fixture, 1, 90.0, 500, fixture->governor->epoch());
    DeviceRegistration updated;
    updated.device = DeviceId{StrongId<DeviceIdTag>{1}};
    updated.generation = DeviceGeneration{StrongId<DeviceGenerationTag>{4}};
    updated.node = NodeId{StrongId<NodeIdTag>{1}};
    updated.node_generation = NodeGeneration{StrongId<NodeGenerationTag>{1}};
    updated.rack = RackId{StrongId<RackIdTag>{1}};
    updated.rack_generation = RackGeneration{StrongId<RackGenerationTag>{1}};
    updated.provenance = Provenance::SYNTHETIC;
    updated.capabilities.set(ThermalCapability::TEMPERATURE, CapabilityState::SUPPORTED_SYNTHETIC);
    TG_STATUS_OK(fixture->governor->register_device(updated));
    TG_ERROR_CODE(fixture->governor->publish_temperature(evidence),
                  ThermalErrorCode::STALE_DEVICE_GENERATION);
}

TG_CASE(adversarial, reordered_telemetry_is_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 60.0));
    TG_OK(fixture->publish(1, 61.0));
    TG_OK(fixture->publish(1, 62.0));
    // A record from the middle of the stream arrives late.
    ThermalEvidence late = craft(*fixture, 1, 10.0, 2,
                                 fixture->governor->epoch());
    late.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{2}};
    TG_ERROR_CODE(fixture->governor->publish_temperature(late),
                  ThermalErrorCode::STALE_TELEMETRY);
}

TG_CASE(adversarial, action_completion_after_policy_change_is_superseded) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    MitigationIntentRecord record;
    record.intent = MitigationIntent::REQUEST_CLOCK_REDUCTION;
    auto action = fixture->governor->authorize_mitigation(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "derate");
    TG_OK(action);
    TG_OK(fixture->governor->record_action_dispatch(action.value()));
    const auto dispatched = fixture->governor->get_action(action.value()).value();

    auto policy = fixture->governor->get_policy(ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}});
    TG_OK(policy);
    ThermalPolicy updated = policy.value();
    updated.generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{2}};
    TG_OK(fixture->governor->set_policy(updated));

    ActionResult result;
    result.action_id = action.value();
    result.action_generation = dispatched.generation;
    result.coordinator_epoch = fixture->governor->epoch();
    result.worker_boot = dispatched.authority.worker_boot;
    result.device_generation = dispatched.authority.device_generation;
    result.domain_generation = dispatched.authority.domain_generation;
    result.policy_generation = dispatched.authority.policy_generation;
    result.backend_succeeded = true;
    TG_ERROR_CODE(fixture->governor->record_action_result(result), ThermalErrorCode::STALE_POLICY);
    TG_CHECK_EQ(fixture->governor->get_action(action.value()).value().lifecycle,
                ActionLifecycle::SUPERSEDED);
}

TG_CASE(adversarial, action_completion_after_device_generation_change_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    MitigationIntentRecord record;
    record.intent = MitigationIntent::REQUEST_CLOCK_REDUCTION;
    auto action = fixture->governor->authorize_mitigation(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "derate");
    TG_OK(action);
    TG_OK(fixture->governor->record_action_dispatch(action.value()));
    const auto dispatched = fixture->governor->get_action(action.value()).value();

    ActionResult result;
    result.action_id = action.value();
    result.action_generation = dispatched.generation;
    result.coordinator_epoch = fixture->governor->epoch();
    result.device_generation = DeviceGeneration{StrongId<DeviceGenerationTag>{7}};
    result.domain_generation = dispatched.authority.domain_generation;
    result.policy_generation = dispatched.authority.policy_generation;
    result.backend_succeeded = true;
    TG_ERROR_CODE(fixture->governor->record_action_result(result),
                  ThermalErrorCode::STALE_DEVICE_GENERATION);
}

TG_CASE(adversarial, backend_failure_after_acknowledgement_is_reported) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    MitigationIntentRecord record;
    record.intent = MitigationIntent::REQUEST_CLOCK_REDUCTION;
    auto action = fixture->governor->authorize_mitigation(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "derate");
    TG_OK(action);
    TG_OK(fixture->governor->record_action_dispatch(action.value()));
    const auto dispatched = fixture->governor->get_action(action.value()).value();

    ActionAck ack;
    ack.action_id = action.value();
    ack.action_generation = dispatched.generation;
    ack.coordinator_epoch = fixture->governor->epoch();
    ack.worker_boot = dispatched.authority.worker_boot;
    ack.device_generation = dispatched.authority.device_generation;
    ack.domain_generation = dispatched.authority.domain_generation;
    ack.accepted = true;
    TG_OK(fixture->governor->record_action_ack(ack));

    ActionResult failure;
    failure.action_id = action.value();
    failure.action_generation = dispatched.generation;
    failure.coordinator_epoch = fixture->governor->epoch();
    failure.device_generation = dispatched.authority.device_generation;
    failure.domain_generation = dispatched.authority.domain_generation;
    failure.policy_generation = dispatched.authority.policy_generation;
    failure.backend_succeeded = false;
    failure.detail = "backend failed after acknowledging";
    TG_OK(fixture->governor->record_action_result(failure));
    TG_CHECK_EQ(fixture->governor->get_action(action.value()).value().lifecycle,
                ActionLifecycle::FAILED);
}

TG_CASE(adversarial, rejected_acknowledgement_fails_the_action) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    MitigationIntentRecord record;
    record.intent = MitigationIntent::REQUEST_CLOCK_REDUCTION;
    auto action = fixture->governor->authorize_mitigation(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "derate");
    TG_OK(action);
    TG_OK(fixture->governor->record_action_dispatch(action.value()));
    const auto dispatched = fixture->governor->get_action(action.value()).value();
    ActionAck ack;
    ack.action_id = action.value();
    ack.action_generation = dispatched.generation;
    ack.coordinator_epoch = fixture->governor->epoch();
    ack.worker_boot = dispatched.authority.worker_boot;
    ack.device_generation = dispatched.authority.device_generation;
    ack.domain_generation = dispatched.authority.domain_generation;
    ack.accepted = false;
    ack.detail = "device disappeared";
    TG_OK(fixture->governor->record_action_ack(ack));
    TG_CHECK_EQ(fixture->governor->get_action(action.value()).value().lifecycle,
                ActionLifecycle::FAILED);
}

TG_CASE(adversarial, shutdown_during_mitigation_classifies_honestly) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 85.0));
    MitigationIntentRecord first;
    first.intent = MitigationIntent::REQUEST_CLOCK_REDUCTION;
    auto dispatched = fixture->governor->authorize_mitigation(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, first, "derate");
    TG_OK(dispatched);
    TG_OK(fixture->governor->record_action_dispatch(dispatched.value()));

    MitigationIntentRecord second;
    second.intent = MitigationIntent::REQUEST_CONCURRENCY_REDUCTION;
    auto planned = fixture->governor->authorize_mitigation(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, second, "derate");
    TG_OK(planned);

    TG_STATUS_OK(fixture->governor->shutdown());
    // A never-dispatched plan is cancelled, never reported as successful.
    TG_CHECK_EQ(fixture->governor->get_action(planned.value()).value().lifecycle,
                ActionLifecycle::CANCELLED);
    // A dispatched instruction whose outcome was never proven is unknown.
    TG_CHECK_EQ(fixture->governor->get_action(dispatched.value()).value().lifecycle,
                ActionLifecycle::OUTCOME_UNKNOWN);
    // Shutdown is idempotent.
    TG_STATUS_OK(fixture->governor->shutdown());
}

TG_CASE(adversarial, repeated_derate_recover_cycles_stay_consistent) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    for (int cycle = 0; cycle < 25; ++cycle) {
        TG_OK(fixture->publish(1, 85.0));
        auto derated = fixture->governor->evaluate_domain(
            ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
        TG_OK(derated);
        TG_CHECK(derated.value().state == ThermalState::DERATED ||
                 derated.value().state == ThermalState::THROTTLING);
        for (int sample = 0; sample < 3; ++sample) {
            TG_OK(fixture->publish_after(1000, 1, 60.0));
        }
        auto recovered = fixture->governor->evaluate_domain(
            ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
        TG_OK(recovered);
        TG_CHECK_EQ(recovered.value().state, ThermalState::NORMAL);
    }
}

TG_CASE(adversarial, rapid_threshold_oscillation_is_safe) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 70.0));
    for (int step = 0; step < 20; ++step) {
        auto policy = fixture->governor->get_policy(
            ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}});
        TG_OK(policy);
        ThermalPolicy updated = policy.value();
        updated.generation = ThermalPolicyGeneration{
            StrongId<ThermalPolicyGenerationTag>{policy.value().generation.value() + 1}};
        // Oscillate the derating threshold across the current temperature.
        const double derating = step % 2 == 0 ? 65.0 : 75.0;
        updated.thresholds.warning = DegreesCelsius{50.0};
        updated.thresholds.derating = DegreesCelsius{derating};
        updated.thresholds.critical = DegreesCelsius{derating + 10.0};
        updated.thresholds.recovery = DegreesCelsius{45.0};
        TG_OK(fixture->governor->set_policy(updated));
        auto evaluation = fixture->governor->evaluate_domain(
            ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
        TG_OK(evaluation);
        TG_CHECK(evaluation.value().decision != ThermalDecision::ALLOW ||
                 evaluation.value().state == ThermalState::NORMAL ||
                 evaluation.value().state == ThermalState::WARM);
    }
}

TG_CASE(adversarial, coupling_graph_cycle_terminates) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    for (std::uint64_t i = 1; i <= 3; ++i) {
        TG_STATUS_OK(fixture->add_device(i));
        TG_STATUS_OK(fixture->add_accelerator_domain(i, 1, i));
    }
    for (std::uint64_t i = 1; i <= 3; ++i) {
        CouplingRelation relation;
        relation.id = CouplingId{StrongId<CouplingIdTag>{i}};
        relation.source = ThermalDomainId{StrongId<ThermalDomainIdTag>{i}};
        relation.destination = ThermalDomainId{StrongId<ThermalDomainIdTag>{i % 3 + 1}};
        relation.type = CouplingType::SYNTHETIC_COUPLING;
        relation.provenance = Provenance::SYNTHETIC;
        relation.generation = CouplingGeneration{StrongId<CouplingGenerationTag>{1}};
        relation.weight = 1.0;
        TG_STATUS_OK(fixture->governor->register_coupling(relation));
    }
    // A hot member inside a cycle must propagate without looping forever.
    TG_OK(fixture->publish(1, 95.0, 1));
    for (std::uint64_t i = 1; i <= 3; ++i) {
        auto evaluation = fixture->governor->evaluate_domain(
            ThermalDomainId{StrongId<ThermalDomainIdTag>{i}});
        TG_OK(evaluation);
        TG_CHECK(evaluation.value().decision != ThermalDecision::ALLOW);
    }
}

TG_CASE(adversarial, corruption_attacks_on_persistence) {
    TG_PHASE("PERSISTENCE_CORRUPTION");
    auto encoded = encode_durable_state(make_durable());
    TG_OK(encoded);
    const std::vector<std::uint8_t> good = encoded.value();

    // Round trip must succeed first, otherwise the attacks prove nothing.
    auto round_trip = decode_durable_state(good);
    TG_OK(round_trip);
    TG_CHECK_EQ(round_trip.value().state.policies.size(), 1U);

    // Bad magic.
    std::vector<std::uint8_t> bad_magic = good;
    bad_magic[0] = static_cast<std::uint8_t>(bad_magic[0] ^ 0x5A);
    TG_ERROR_CODE(decode_durable_state(bad_magic), ThermalErrorCode::PERSISTENCE_CORRUPT);
    // Unsupported version.
    std::vector<std::uint8_t> bad_version = good;
    bad_version[8] = 0x7F;
    TG_ERROR_CODE(decode_durable_state(bad_version),
                  ThermalErrorCode::PERSISTENCE_UNSUPPORTED_VERSION);
    // Truncated.
    std::vector<std::uint8_t> truncated(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2));
    TG_ERROR_CODE(decode_durable_state(truncated), ThermalErrorCode::PERSISTENCE_TRUNCATED);
    // Trailing garbage.
    std::vector<std::uint8_t> trailing = good;
    trailing.push_back(0x00);
    TG_ERROR_CODE(decode_durable_state(trailing),
                  ThermalErrorCode::PERSISTENCE_TRAILING_GARBAGE);
    // Corrupted header checksum field.
    std::vector<std::uint8_t> header_crc = good;
    header_crc[32] = static_cast<std::uint8_t>(header_crc[32] ^ 0xFF);
    TG_ERROR_CODE(decode_durable_state(header_crc), ThermalErrorCode::PERSISTENCE_INTEGRITY);
    // Corrupted body.
    std::vector<std::uint8_t> body = good;
    body[good.size() / 2] = static_cast<std::uint8_t>(body[good.size() / 2] ^ 0xFF);
    TG_ERROR_CODE(decode_durable_state(body), ThermalErrorCode::PERSISTENCE_INTEGRITY);
    // Absurd declared body length.
    std::vector<std::uint8_t> absurd = good;
    absurd[24] = 0xFF;
    absurd[25] = 0xFF;
    absurd[26] = 0xFF;
    absurd[27] = 0xFF;
    absurd[28] = 0xFF;
    absurd[29] = 0xFF;
    absurd[30] = 0xFF;
    absurd[31] = 0x7F;
    auto absurd_result = decode_durable_state(absurd);
    TG_CHECK(!absurd_result.has_value());
}

TG_CASE(adversarial, corruption_attacks_on_transport_frames) {
    TG_PHASE("FRAME_CORRUPTION");
    const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5};
    auto encoded = encode_frame(MessageKind::PUBLISH_EVIDENCE, 0, payload.data(), payload.size());
    TG_OK(encoded);
    const std::vector<std::uint8_t> good = encoded.value();
    auto decoded = decode_frame(good.data(), good.size());
    TG_OK(decoded);
    TG_CHECK_EQ(decoded.value().payload, payload);

    std::vector<std::uint8_t> bad_magic = good;
    bad_magic[0] = static_cast<std::uint8_t>(bad_magic[0] ^ 0xFF);
    TG_ERROR_CODE(decode_frame(bad_magic.data(), bad_magic.size()),
                  ThermalErrorCode::FRAME_CORRUPT);

    std::vector<std::uint8_t> bad_version = good;
    bad_version[4] = 0x09;
    // Tampering with a header field also breaks the header checksum, so the
    // checksum is repaired to reach the version check itself.
    const std::uint32_t repaired_version = crc32c(bad_version.data(), 16);
    bad_version[16] = static_cast<std::uint8_t>(repaired_version & 0xFFU);
    bad_version[17] = static_cast<std::uint8_t>((repaired_version >> 8) & 0xFFU);
    bad_version[18] = static_cast<std::uint8_t>((repaired_version >> 16) & 0xFFU);
    bad_version[19] = static_cast<std::uint8_t>((repaired_version >> 24) & 0xFFU);
    TG_ERROR_CODE(decode_frame(bad_version.data(), bad_version.size()),
                  ThermalErrorCode::PROTOCOL_UNSUPPORTED);

    std::vector<std::uint8_t> bad_header_crc = good;
    bad_header_crc[16] = static_cast<std::uint8_t>(bad_header_crc[16] ^ 0xFF);
    TG_ERROR_CODE(decode_frame(bad_header_crc.data(), bad_header_crc.size()),
                  ThermalErrorCode::FRAME_CORRUPT);

    std::vector<std::uint8_t> bad_payload = good;
    bad_payload[21] = static_cast<std::uint8_t>(bad_payload[21] ^ 0xFF);
    TG_ERROR_CODE(decode_frame(bad_payload.data(), bad_payload.size()),
                  ThermalErrorCode::FRAME_CORRUPT);

    // Partial header and partial payload are truncation, not corruption.
    TG_ERROR_CODE(decode_frame(good.data(), 10), ThermalErrorCode::FRAME_TRUNCATED);
    TG_ERROR_CODE(decode_frame(good.data(), good.size() - 2),
                  ThermalErrorCode::FRAME_TRUNCATED);

    // Oversized declared payload is refused before any allocation.
    std::vector<std::uint8_t> oversized = good;
    oversized[12] = 0xFF;
    oversized[13] = 0xFF;
    oversized[14] = 0xFF;
    oversized[15] = 0x7F;
    // The header checksum no longer matches after tampering, so rebuild it.
    const std::uint32_t repaired = crc32c(oversized.data(), 16);
    oversized[16] = static_cast<std::uint8_t>(repaired & 0xFFU);
    oversized[17] = static_cast<std::uint8_t>((repaired >> 8) & 0xFFU);
    oversized[18] = static_cast<std::uint8_t>((repaired >> 16) & 0xFFU);
    oversized[19] = static_cast<std::uint8_t>((repaired >> 24) & 0xFFU);
    FrameLimits limits;
    TG_ERROR_CODE(decode_frame(oversized.data(), oversized.size(), limits),
                  ThermalErrorCode::FRAME_TOO_LARGE);

    // Unknown message kind.
    std::vector<std::uint8_t> unknown_kind = good;
    unknown_kind[6] = 0xEE;
    unknown_kind[7] = 0xEE;
    const std::uint32_t repaired_kind = crc32c(unknown_kind.data(), 16);
    unknown_kind[16] = static_cast<std::uint8_t>(repaired_kind & 0xFFU);
    unknown_kind[17] = static_cast<std::uint8_t>((repaired_kind >> 8) & 0xFFU);
    unknown_kind[18] = static_cast<std::uint8_t>((repaired_kind >> 16) & 0xFFU);
    unknown_kind[19] = static_cast<std::uint8_t>((repaired_kind >> 24) & 0xFFU);
    TG_ERROR_CODE(decode_frame(unknown_kind.data(), unknown_kind.size()),
                  ThermalErrorCode::FRAME_CORRUPT);
}

TG_CASE(adversarial, invalid_temperature_encodings_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    ThermalEvidence nan = craft(*fixture, 1, 0.0, 1, fixture->governor->epoch());
    nan.temperature = DegreesCelsius{std::nan("")};
    TG_ERROR_CODE(fixture->governor->publish_temperature(nan),
                  ThermalErrorCode::TEMPERATURE_INVALID);

    ThermalEvidence infinite = craft(*fixture, 1, 0.0, 2, fixture->governor->epoch());
    infinite.temperature = DegreesCelsius{std::numeric_limits<double>::infinity()};
    TG_ERROR_CODE(fixture->governor->publish_temperature(infinite),
                  ThermalErrorCode::TEMPERATURE_INVALID);

    ThermalEvidence absurd = craft(*fixture, 1, 0.0, 3, fixture->governor->epoch());
    absurd.temperature = DegreesCelsius{1.0e9};
    TG_ERROR_CODE(fixture->governor->publish_temperature(absurd),
                  ThermalErrorCode::TEMPERATURE_INVALID);
}

TG_CASE(adversarial, unknown_device_and_domain_are_typed_errors) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    ThermalEvidence unknown_device = craft(*fixture, 77, 40.0, 1, fixture->governor->epoch());
    TG_ERROR_CODE(fixture->governor->publish_temperature(unknown_device),
                  ThermalErrorCode::UNKNOWN_DEVICE);

    ThermalEvidence unknown_domain = craft(*fixture, 1, 40.0, 2, fixture->governor->epoch());
    unknown_domain.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{88}};
    unknown_domain.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    TG_ERROR_CODE(fixture->governor->publish_temperature(unknown_domain),
                  ThermalErrorCode::UNKNOWN_DOMAIN);
}

TG_CASE(adversarial, telemetry_source_disappearance_is_revalidation) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 60.0));
    // The device stops resolving capabilities: it becomes UNKNOWN, and an
    // unresolved capability cannot authorise execution.
    TG_STATUS_OK(fixture->governor->publish_capabilities(
        DeviceId{StrongId<DeviceIdTag>{1}}, DeviceGeneration{StrongId<DeviceGenerationTag>{1}},
        CapabilitySet{}));
    auto evaluation = fixture->governor->evaluate_domain(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(evaluation);
    TG_CHECK_EQ(evaluation.value().state, ThermalState::UNKNOWN);
    TG_CHECK(evaluation.value().decision != ThermalDecision::ALLOW);
}

TG_CASE(adversarial, unsupported_sensor_is_unsupported_not_unknown) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_OK(fixture->publish(1, 60.0));
    TG_STATUS_OK(fixture->governor->publish_capabilities(
        DeviceId{StrongId<DeviceIdTag>{1}}, DeviceGeneration{StrongId<DeviceGenerationTag>{1}},
        CapabilitySet{}));
    CapabilitySet unsupported;
    unsupported.set(ThermalCapability::TEMPERATURE, CapabilityState::UNSUPPORTED);
    TG_STATUS_OK(fixture->governor->publish_capabilities(
        DeviceId{StrongId<DeviceIdTag>{1}}, DeviceGeneration{StrongId<DeviceGenerationTag>{1}},
        unsupported));
    auto evaluation = fixture->governor->evaluate_domain(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(evaluation);
    TG_CHECK_EQ(evaluation.value().state, ThermalState::UNSUPPORTED);
    TG_CHECK_EQ(evaluation.value().decision, ThermalDecision::UNSUPPORTED);
}

TG_CASE(adversarial, repeated_start_stop_is_clean) {
    for (int iteration = 0; iteration < 12; ++iteration) {
        auto fixture = GovernorFixture::create();
        TG_CHECK(fixture != nullptr);
        prepare(tg_ctx, *fixture);
        TG_OK(fixture->publish(1, 60.0));
        TG_STATUS_OK(fixture->governor->shutdown());
    }
}

TG_CASE(adversarial, worker_death_during_mitigation_is_honest) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_STATUS_OK(fixture->governor->register_worker(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}},
        Label{"worker-A"}));
    TG_OK(fixture->publish(1, 88.0, 1, 1, 1, WorkerId{StrongId<WorkerIdTag>{1}},
                           WorkerBootId{StrongId<WorkerBootIdTag>{1}}));
    MitigationIntentRecord record;
    record.intent = MitigationIntent::REQUEST_CONTAINMENT;
    auto action = fixture->governor->authorize_mitigation(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "contain");
    TG_OK(action);
    TG_OK(fixture->governor->record_action_dispatch(action.value()));
    TG_STATUS_OK(fixture->governor->note_worker_death(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}},
        "killed mid-mitigation"));
    const auto after = fixture->governor->get_action(action.value()).value();
    TG_CHECK_EQ(after.lifecycle, ActionLifecycle::OUTCOME_UNKNOWN);
    // The domain is no longer authorised on the dead incarnation's evidence.
    auto evaluation = fixture->governor->evaluate_domain(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    TG_OK(evaluation);
    TG_CHECK_EQ(evaluation.value().state, ThermalState::REVALIDATION_REQUIRED);
}
