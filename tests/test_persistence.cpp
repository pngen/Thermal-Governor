// Thermal Governor — durable state persistence tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "framework.hpp"
#include "process_util.hpp"

#include "thermal_governor/action.hpp"
#include "thermal_governor/coupling.hpp"
#include "thermal_governor/decision.hpp"
#include "thermal_governor/domain.hpp"
#include "thermal_governor/error.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/mitigation.hpp"
#include "thermal_governor/persistence.hpp"
#include "thermal_governor/policy.hpp"
#include "thermal_governor/quantity.hpp"
#include "thermal_governor/thermal_state.hpp"
#include "thermal_governor/time.hpp"
#include "thermal_governor/verification.hpp"
#include "thermal_governor/version.hpp"

using thermal_governor::ActionGeneration;
using thermal_governor::ActionId;
using thermal_governor::ActionLifecycle;
using thermal_governor::ActionVerification;
using thermal_governor::AggregationRule;
using thermal_governor::CoordinatorEpoch;
using thermal_governor::CoordinatorId;
using thermal_governor::CouplingGeneration;
using thermal_governor::CouplingId;
using thermal_governor::CouplingRelation;
using thermal_governor::CouplingType;
using thermal_governor::DegreesCelsius;
using thermal_governor::DeratingLevel;
using thermal_governor::DeviceGeneration;
using thermal_governor::DeviceId;
using thermal_governor::DomainMember;
using thermal_governor::DurableState;
using thermal_governor::DurableStore;
using thermal_governor::ExecutionClass;
using thermal_governor::Label;
using thermal_governor::MegaHertz;
using thermal_governor::Milliseconds;
using thermal_governor::MitigationIntent;
using thermal_governor::Percent;
using thermal_governor::PersistenceLimits;
using thermal_governor::PersistenceLoadResult;
using thermal_governor::PersistenceRecordType;
using thermal_governor::Provenance;
using thermal_governor::Result;
using thermal_governor::StateTransitionRecord;
using thermal_governor::StrongId;
using thermal_governor::SubjectRef;
using thermal_governor::TemperatureDelta;
using thermal_governor::TelemetryGeneration;
using thermal_governor::ThermalAction;
using thermal_governor::ThermalDecision;
using thermal_governor::ThermalDomainDefinition;
using thermal_governor::ThermalDomainGeneration;
using thermal_governor::ThermalDomainId;
using thermal_governor::ThermalDomainType;
using thermal_governor::ThermalErrorCode;
using thermal_governor::ThermalPolicy;
using thermal_governor::ThermalPolicyGeneration;
using thermal_governor::ThermalPolicyId;
using thermal_governor::ThermalState;
using thermal_governor::TopologyGeneration;
using thermal_governor::UnknownBehavior;
using thermal_governor::UnsupportedBehavior;
using thermal_governor::VerificationGeneration;
using thermal_governor::VerificationOutcome;
using thermal_governor::WorkloadRestriction;
using thermal_governor::WorkloadThermalProfile;
using thermal_governor::crc32c;
using thermal_governor::decode_durable_state;
using thermal_governor::encode_durable_state;
using thermal_governor::kPersistenceFormatVersion;

namespace {

// Container layout produced by encode_durable_state:
//   [0, 8)   magic
//   [8, 12)  format version
//   [12, 16) reserved
//   [16, 24) record count
//   [24, 32) body length
//   [32, 36) header checksum over bytes [0, 32)
//   [36, 36 + body length) body
//   [36 + body length, 40 + body length) body checksum
constexpr std::size_t kHeaderSize = 36;
constexpr std::size_t kHeaderChecksumCoverage = 32;
constexpr std::size_t kBodyLengthOffset = 24;
constexpr std::size_t kRecordCountOffset = 16;
constexpr std::size_t kRecordHeaderSize = 8;
constexpr std::size_t kRecordTrailerSize = 4;

void put_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::uint32_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU);
    }
}

void put_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::uint32_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xFFU);
    }
}

[[nodiscard]] std::uint16_t get_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

[[nodiscard]] std::uint32_t get_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::uint32_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (8U * index);
    }
    return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::uint32_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (8U * index);
    }
    return value;
}

/// Recompute the container header checksum after a deliberate header edit.
void repair_header_crc(std::vector<std::uint8_t>& bytes) {
    put_u32(bytes, 32, crc32c(bytes.data(), kHeaderChecksumCoverage));
}

/// Recompute the body checksum after a deliberate body edit.
void repair_body_crc(std::vector<std::uint8_t>& bytes) {
    const std::size_t body_length = static_cast<std::size_t>(get_u64(bytes, kBodyLengthOffset));
    put_u32(bytes, kHeaderSize + body_length,
            crc32c(bytes.data() + kHeaderSize, body_length));
}

/// Recompute one record checksum after a deliberate payload edit. The record
/// checksum covers the record header and its payload, never itself.
void repair_record_crc(std::vector<std::uint8_t>& bytes, std::size_t record_offset) {
    const std::size_t payload_length =
        static_cast<std::size_t>(get_u32(bytes, record_offset + 4));
    put_u32(bytes, record_offset + kRecordHeaderSize + payload_length,
            crc32c(bytes.data() + record_offset, kRecordHeaderSize + payload_length));
}

struct RecordLocation {
    std::size_t payload_offset = 0;
    std::size_t record_offset = 0;
    std::uint32_t payload_length = 0;
    std::uint16_t type = 0;
};

[[nodiscard]] std::vector<RecordLocation> locate_records(const std::vector<std::uint8_t>& bytes) {
    std::vector<RecordLocation> records;
    const std::size_t count = static_cast<std::size_t>(get_u64(bytes, kRecordCountOffset));
    std::size_t offset = kHeaderSize;
    for (std::size_t index = 0; index < count; ++index) {
        RecordLocation location;
        location.record_offset = offset;
        location.payload_offset = offset + kRecordHeaderSize;
        location.type = get_u16(bytes, offset);
        location.payload_length = get_u32(bytes, offset + 4);
        records.push_back(location);
        offset += kRecordHeaderSize + static_cast<std::size_t>(location.payload_length) +
                  kRecordTrailerSize;
    }
    return records;
}

[[nodiscard]] RecordLocation find_record(const std::vector<std::uint8_t>& bytes,
                                         PersistenceRecordType type) {
    const auto wanted = static_cast<std::uint16_t>(type);
    for (const auto& record : locate_records(bytes)) {
        if (record.type == wanted) {
            return record;
        }
    }
    return RecordLocation{};
}

[[nodiscard]] Result<PersistenceLoadResult> encode_then_decode(const DurableState& state) {
    const auto encoded = encode_durable_state(state);
    if (!encoded.has_value()) {
        return encoded.error();
    }
    return decode_durable_state(encoded.value());
}

[[nodiscard]] std::vector<std::uint8_t> encoded_bytes(const DurableState& state) {
    const auto encoded = encode_durable_state(state);
    return encoded.has_value() ? encoded.value() : std::vector<std::uint8_t>{};
}

[[nodiscard]] ThermalPolicy secondary_policy() {
    ThermalPolicy policy = ThermalPolicy::make_default();
    policy.id = ThermalPolicyId{StrongId<thermal_governor::ThermalPolicyIdTag>{2}};
    policy.generation = ThermalPolicyGeneration{StrongId<thermal_governor::ThermalPolicyGenerationTag>{4}};
    policy.label = Label{"secondary"};
    policy.thresholds.warning = DegreesCelsius{70.25};
    policy.thresholds.derating = DegreesCelsius{78.5};
    policy.thresholds.critical = DegreesCelsius{86.75};
    policy.thresholds.recovery = DegreesCelsius{68.0};
    policy.thresholds.near_limit_band = TemperatureDelta{2.25};
    policy.margins.policy_safety_margin = TemperatureDelta{1.25};
    policy.margins.uncertainty_margin = TemperatureDelta{0.5};
    policy.margins.recovery_margin = TemperatureDelta{0.75};
    policy.freshness.max_age = Milliseconds{2500};
    policy.freshness.required_consecutive_samples = 5;
    policy.freshness.min_evidence_span = Milliseconds{4000};
    policy.freshness.min_distinct_witnesses = 2;
    policy.recovery.require_explicit_authorization = true;
    policy.recovery.forbid_recovery_after_unsupported_evidence = false;
    policy.verification.required = false;
    policy.verification.improvement_epsilon = TemperatureDelta{0.75};
    policy.verification.required_samples = 3;
    policy.admission.min_effective_headroom = TemperatureDelta{3.25};
    policy.admission.allow_derated_admission = false;
    policy.admission.allow_defer = false;
    policy.concurrency.normal = Percent{100.0};
    policy.concurrency.warm = Percent{87.5};
    policy.concurrency.near_limit = Percent{62.5};
    policy.concurrency.derated = Percent{37.5};
    policy.concurrency.critical = Percent{12.5};
    policy.emergency.allow_migration_intent = true;
    policy.coupling.max_propagation_depth = 4;
    policy.coupling.escalate_to_hottest_neighbour = true;
    policy.unknown_behavior = UnknownBehavior::DENY;
    policy.unsupported_behavior = UnsupportedBehavior::DEFER;
    policy.allowed_intents = 0x00000052U;
    policy.workload_restrictions = {
        WorkloadRestriction{WorkloadThermalProfile::CUSTOM, true,
                            ExecutionClass::BEST_EFFORT_THERMAL, TemperatureDelta{0.75}}};
    return policy;
}

[[nodiscard]] ThermalDomainDefinition make_domain_definition(std::uint64_t id,
                                                             std::uint64_t generation) {
    ThermalDomainDefinition definition;
    definition.id = ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{id}};
    definition.generation =
        ThermalDomainGeneration{StrongId<thermal_governor::ThermalDomainGenerationTag>{generation}};
    definition.type = ThermalDomainType::NODE;
    definition.label = Label{"node-" + std::to_string(id)};
    DomainMember first;
    first.subject = SubjectRef::for_device(
        DeviceId{StrongId<thermal_governor::DeviceIdTag>{1}},
        DeviceGeneration{StrongId<thermal_governor::DeviceGenerationTag>{1}});
    first.weight = 0.5;
    DomainMember second;
    second.subject = SubjectRef::for_device(
        DeviceId{StrongId<thermal_governor::DeviceIdTag>{2}},
        DeviceGeneration{StrongId<thermal_governor::DeviceGenerationTag>{3}});
    second.weight = 0.25;
    definition.members = {first, second};
    definition.children = {ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{7}}};
    definition.policy = ThermalPolicyId{StrongId<thermal_governor::ThermalPolicyIdTag>{1}};
    definition.policy_generation =
        ThermalPolicyGeneration{StrongId<thermal_governor::ThermalPolicyGenerationTag>{2}};
    definition.provenance = Provenance::REAL;
    definition.evidence_source = "node sensor";
    definition.aggregation = AggregationRule::CONFIGURED_WEIGHTED_RULE;
    definition.rack = thermal_governor::RackId{StrongId<thermal_governor::RackIdTag>{2}};
    definition.rack_generation =
        thermal_governor::RackGeneration{StrongId<thermal_governor::RackGenerationTag>{1}};
    definition.cooling_zone =
        thermal_governor::CoolingZoneId{StrongId<thermal_governor::CoolingZoneIdTag>{3}};
    definition.synthetic_aggregate_offset = 4.5;
    return definition;
}

[[nodiscard]] CouplingRelation make_coupling(std::uint64_t id, std::uint64_t source,
                                             std::uint64_t destination) {
    CouplingRelation relation;
    relation.id = CouplingId{StrongId<thermal_governor::CouplingIdTag>{id}};
    relation.source = ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{source}};
    relation.destination =
        ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{destination}};
    relation.type = CouplingType::SHARES_CHASSIS;
    relation.generation = CouplingGeneration{StrongId<thermal_governor::CouplingGenerationTag>{2}};
    relation.provenance = Provenance::SYNTHETIC;
    relation.evidence_source = "chassis";
    relation.weight = 0.5;
    relation.directed = false;
    return relation;
}

[[nodiscard]] StateTransitionRecord make_transition() {
    StateTransitionRecord record;
    record.domain = ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{1}};
    record.domain_generation =
        ThermalDomainGeneration{StrongId<thermal_governor::ThermalDomainGenerationTag>{3}};
    record.from = ThermalState::WARM;
    record.to = ThermalState::DERATED;
    record.derating_from = DeratingLevel::FULL_CAPABILITY;
    record.derating_to = DeratingLevel::DERATED_CONCURRENCY;
    record.decision = ThermalDecision::ALLOW_DERATED;
    record.temperature = DegreesCelsius{82.5};
    record.policy_generation =
        ThermalPolicyGeneration{StrongId<thermal_governor::ThermalPolicyGenerationTag>{2}};
    record.telemetry_generation =
        TelemetryGeneration{StrongId<thermal_governor::TelemetryGenerationTag>{8}};
    record.coordinator_epoch = CoordinatorEpoch{StrongId<thermal_governor::CoordinatorEpochTag>{7}};
    record.sequence = 12;
    return record;
}

[[nodiscard]] ThermalAction make_action(std::uint64_t id) {
    ThermalAction action;
    action.id = ActionId{StrongId<thermal_governor::ActionIdTag>{id}};
    action.generation = ActionGeneration{StrongId<thermal_governor::ActionGenerationTag>{2}};
    action.authority.coordinator_epoch =
        CoordinatorEpoch{StrongId<thermal_governor::CoordinatorEpochTag>{7}};
    action.authority.action_id = action.id;
    action.authority.action_generation = action.generation;
    action.authority.domain = ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{1}};
    action.authority.domain_generation =
        ThermalDomainGeneration{StrongId<thermal_governor::ThermalDomainGenerationTag>{3}};
    action.authority.device = DeviceId{StrongId<thermal_governor::DeviceIdTag>{5}};
    action.authority.device_generation =
        DeviceGeneration{StrongId<thermal_governor::DeviceGenerationTag>{6}};
    action.authority.policy_generation =
        ThermalPolicyGeneration{StrongId<thermal_governor::ThermalPolicyGenerationTag>{2}};
    action.authority.telemetry_generation =
        TelemetryGeneration{StrongId<thermal_governor::TelemetryGenerationTag>{8}};
    action.authority.worker = thermal_governor::WorkerId{StrongId<thermal_governor::WorkerIdTag>{4}};
    action.authority.worker_boot =
        thermal_governor::WorkerBootId{StrongId<thermal_governor::WorkerBootIdTag>{9}};
    action.authority.topology_generation =
        TopologyGeneration{StrongId<thermal_governor::TopologyGenerationTag>{1}};
    action.authority.capability_generation =
        thermal_governor::CapabilityGeneration{StrongId<thermal_governor::CapabilityGenerationTag>{2}};
    action.intent = MitigationIntent::REQUEST_CONCURRENCY_REDUCTION;
    action.requested_clock_ceiling = MegaHertz{1500};
    action.requested_concurrency = Percent{50.0};
    action.requested_admission = Percent{25.5};
    action.lifecycle = ActionLifecycle::DISPATCHED;
    action.provenance = Provenance::SYNTHETIC;
    action.detail = "reduce concurrency";
    action.verification_generation =
        VerificationGeneration{StrongId<thermal_governor::VerificationGenerationTag>{1}};
    action.verification_telemetry_generation =
        TelemetryGeneration{StrongId<thermal_governor::TelemetryGenerationTag>{8}};
    action.superseded_by = ActionId{StrongId<thermal_governor::ActionIdTag>{id + 100}};
    action.authorization_source = "operator";
    return action;
}

[[nodiscard]] ActionVerification make_verification() {
    ActionVerification verification;
    verification.action_id = ActionId{StrongId<thermal_governor::ActionIdTag>{5}};
    verification.generation =
        VerificationGeneration{StrongId<thermal_governor::VerificationGenerationTag>{3}};
    verification.outcome = VerificationOutcome::DERATING_EFFECTIVE;
    verification.resulting_lifecycle = ActionLifecycle::EFFECTIVE;
    verification.temperature_before = DegreesCelsius{91.25};
    verification.temperature_after = DegreesCelsius{84.5};
    verification.delta = TemperatureDelta{-6.75};
    verification.headroom_before = TemperatureDelta{-5.25};
    verification.headroom_after = TemperatureDelta{1.5};
    verification.state_before = ThermalState::CRITICAL;
    verification.state_after = ThermalState::DERATED;
    verification.derating_before = DeratingLevel::NO_SAFE_EXECUTION;
    verification.derating_after = DeratingLevel::DERATED_CONCURRENCY;
    verification.hard_constraint_still_violated = false;
    verification.derating_still_required = true;
    verification.recovery_conditions_satisfied = false;
    verification.telemetry_generation =
        TelemetryGeneration{StrongId<thermal_governor::TelemetryGenerationTag>{9}};
    verification.policy_generation =
        ThermalPolicyGeneration{StrongId<thermal_governor::ThermalPolicyGenerationTag>{2}};
    verification.coordinator_epoch =
        CoordinatorEpoch{StrongId<thermal_governor::CoordinatorEpochTag>{7}};
    return verification;
}

[[nodiscard]] DurableState make_state() {
    DurableState state;
    state.format_version = kPersistenceFormatVersion;
    state.coordinator = CoordinatorId{StrongId<thermal_governor::CoordinatorIdTag>{42}};
    state.coordinator_epoch = CoordinatorEpoch{StrongId<thermal_governor::CoordinatorEpochTag>{7}};
    state.policy_generation =
        ThermalPolicyGeneration{StrongId<thermal_governor::ThermalPolicyGenerationTag>{3}};
    state.topology_generation = TopologyGeneration{StrongId<thermal_governor::TopologyGenerationTag>{5}};
    state.sequence = 99;

    ThermalPolicy primary = ThermalPolicy::make_default();
    primary.id = ThermalPolicyId{StrongId<thermal_governor::ThermalPolicyIdTag>{1}};
    primary.generation =
        ThermalPolicyGeneration{StrongId<thermal_governor::ThermalPolicyGenerationTag>{1}};
    primary.label = Label{"primary"};
    state.policies = {primary, secondary_policy()};

    state.domains = {make_domain_definition(1, 3), make_domain_definition(2, 4)};
    state.domains[0].parent = ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{2}};

    CouplingRelation expiring = make_coupling(2, 2, 1);
    expiring.valid_until =
        thermal_governor::SteadyTimePoint{std::chrono::seconds{3600}};
    state.couplings = {make_coupling(1, 1, 2), expiring};

    state.transitions = {make_transition()};
    state.actions = {make_action(5), make_action(6)};
    state.verifications = {make_verification()};
    return state;
}

/// Expected record count for make_state: one coordinator record, two policies,
/// two domains, two couplings, one transition, two actions, one verification.
constexpr std::uint64_t kExpectedRecords = 11;

}  // namespace

TG_CASE(persistence, crc32c_matches_the_known_check_value) {
    const std::string check = "123456789";
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(check.data());
    TG_CHECK_EQ(crc32c(bytes, check.size()), 0xE3069283U);

    TG_PHASE("stability and seeding");
    TG_CHECK_EQ(crc32c(bytes, check.size()), crc32c(bytes, check.size()));
    TG_CHECK_EQ(crc32c(bytes, 0), 0U);
    TG_CHECK_EQ(crc32c(nullptr, 0), 0U);
    TG_CHECK(crc32c(bytes, check.size(), 0) != crc32c(bytes, check.size(), 1));

    TG_PHASE("a single flipped bit changes the checksum");
    std::string mutated = check;
    mutated[0] = '2';
    TG_CHECK(crc32c(reinterpret_cast<const std::uint8_t*>(mutated.data()), mutated.size()) !=
             0xE3069283U);
}

TG_CASE(persistence, round_trip_preserves_coordinator_state) {
    const DurableState source = make_state();
    const auto loaded = encode_then_decode(source);
    TG_CHECK(loaded.has_value());

    const DurableState& decoded = loaded.value().state;
    TG_CHECK_EQ(decoded.format_version, kPersistenceFormatVersion);
    TG_CHECK_EQ(decoded.coordinator.value(), source.coordinator.value());
    TG_CHECK_EQ(decoded.coordinator_epoch.value(), source.coordinator_epoch.value());
    TG_CHECK_EQ(decoded.policy_generation.value(), source.policy_generation.value());
    TG_CHECK_EQ(decoded.topology_generation.value(), source.topology_generation.value());
    TG_CHECK_EQ(decoded.sequence, source.sequence);

    TG_CHECK_EQ(loaded.value().records_read, kExpectedRecords);
    TG_CHECK(loaded.value().bytes_read > kHeaderSize);
    TG_CHECK_EQ(encoded_bytes(source).size(), loaded.value().bytes_read);
}

TG_CASE(persistence, round_trip_preserves_policies) {
    const DurableState source = make_state();
    const auto loaded = encode_then_decode(source);
    TG_CHECK(loaded.has_value());

    const auto& policies = loaded.value().state.policies;
    TG_CHECK_EQ(policies.size(), source.policies.size());

    const ThermalPolicy& primary = policies[0];
    TG_CHECK_EQ(primary.id.value(), source.policies[0].id.value());
    TG_CHECK_EQ(primary.generation.value(), source.policies[0].generation.value());
    TG_CHECK_EQ(primary.label.text(), std::string("primary"));
    TG_CHECK_EQ(primary.thresholds.critical.value(),
                source.policies[0].thresholds.critical.value());
    TG_CHECK_EQ(primary.allowed_intents, source.policies[0].allowed_intents);
    TG_CHECK_EQ(primary.workload_restrictions.size(),
                source.policies[0].workload_restrictions.size());

    const ThermalPolicy& secondary = policies[1];
    const ThermalPolicy& expected = source.policies[1];
    TG_CHECK_EQ(secondary.id.value(), expected.id.value());
    TG_CHECK_EQ(secondary.generation.value(), expected.generation.value());
    TG_CHECK_EQ(secondary.label.text(), std::string("secondary"));
    TG_CHECK_EQ(secondary.thresholds.warning.value(), 70.25);
    TG_CHECK_EQ(secondary.thresholds.derating.value(), 78.5);
    TG_CHECK_EQ(secondary.thresholds.critical.value(), 86.75);
    TG_CHECK_EQ(secondary.thresholds.recovery.value(), 68.0);
    TG_CHECK_EQ(secondary.thresholds.near_limit_band.value(), 2.25);
    TG_CHECK_EQ(secondary.margins.policy_safety_margin.value(), 1.25);
    TG_CHECK_EQ(secondary.margins.uncertainty_margin.value(), 0.5);
    TG_CHECK_EQ(secondary.margins.recovery_margin.value(), 0.75);
    TG_CHECK_EQ(secondary.freshness.max_age.count(), 2500);
    TG_CHECK_EQ(secondary.freshness.required_consecutive_samples, 5U);
    TG_CHECK_EQ(secondary.freshness.min_evidence_span.count(), 4000);
    TG_CHECK_EQ(secondary.freshness.min_distinct_witnesses, 2U);
    TG_CHECK(secondary.recovery.require_explicit_authorization);
    TG_CHECK(!secondary.recovery.forbid_recovery_after_unsupported_evidence);
    TG_CHECK(!secondary.verification.required);
    TG_CHECK_EQ(secondary.verification.improvement_epsilon.value(), 0.75);
    TG_CHECK_EQ(secondary.verification.required_samples, 3U);
    TG_CHECK_EQ(secondary.admission.min_effective_headroom.value(), 3.25);
    TG_CHECK(!secondary.admission.allow_derated_admission);
    TG_CHECK(!secondary.admission.allow_defer);
    TG_CHECK_EQ(secondary.concurrency.warm.value(), 87.5);
    TG_CHECK_EQ(secondary.concurrency.near_limit.value(), 62.5);
    TG_CHECK_EQ(secondary.concurrency.derated.value(), 37.5);
    TG_CHECK_EQ(secondary.concurrency.critical.value(), 12.5);
    TG_CHECK(secondary.emergency.allow_migration_intent);
    TG_CHECK_EQ(secondary.coupling.max_propagation_depth, 4U);
    TG_CHECK(secondary.coupling.escalate_to_hottest_neighbour);
    TG_CHECK(secondary.unknown_behavior == UnknownBehavior::DENY);
    TG_CHECK(secondary.unsupported_behavior == UnsupportedBehavior::DEFER);
    TG_CHECK_EQ(secondary.allowed_intents, 0x00000052U);
    TG_CHECK_EQ(secondary.workload_restrictions.size(), std::size_t{1});
    TG_CHECK(secondary.workload_restrictions[0].profile == WorkloadThermalProfile::CUSTOM);
    TG_CHECK(secondary.workload_restrictions[0].required_class ==
             ExecutionClass::BEST_EFFORT_THERMAL);
    TG_CHECK_EQ(secondary.workload_restrictions[0].extra_headroom_demand.value(), 0.75);
    TG_STATUS_OK(thermal_governor::validate_policy(secondary));
}

TG_CASE(persistence, round_trip_preserves_domains) {
    const DurableState source = make_state();
    const auto loaded = encode_then_decode(source);
    TG_CHECK(loaded.has_value());

    const auto& domains = loaded.value().state.domains;
    TG_CHECK_EQ(domains.size(), std::size_t{2});

    const ThermalDomainDefinition& first = domains[0];
    TG_CHECK_EQ(first.id.value(), 1U);
    TG_CHECK_EQ(first.generation.value(), 3U);
    TG_CHECK(first.type == ThermalDomainType::NODE);
    TG_CHECK_EQ(first.label.text(), std::string("node-1"));
    TG_CHECK_EQ(first.members.size(), std::size_t{2});
    TG_CHECK(first.members[0].subject.kind == thermal_governor::SubjectKind::DEVICE);
    TG_CHECK_EQ(first.members[0].subject.device.value(), 1U);
    TG_CHECK_EQ(first.members[0].subject.device_generation.value(), 1U);
    TG_CHECK_EQ(first.members[0].weight, 0.5);
    TG_CHECK_EQ(first.members[1].subject.device.value(), 2U);
    TG_CHECK_EQ(first.members[1].weight, 0.25);
    TG_CHECK(first.parent.has_value());
    TG_CHECK_EQ(first.parent->value(), 2U);
    TG_CHECK_EQ(first.children.size(), std::size_t{1});
    TG_CHECK_EQ(first.children[0].value(), 7U);
    TG_CHECK_EQ(first.policy.value(), 1U);
    TG_CHECK_EQ(first.policy_generation.value(), 2U);
    TG_CHECK(first.provenance == Provenance::REAL);
    TG_CHECK_EQ(first.evidence_source, std::string("node sensor"));
    TG_CHECK(first.aggregation == AggregationRule::CONFIGURED_WEIGHTED_RULE);
    TG_CHECK_EQ(first.rack.value(), 2U);
    TG_CHECK_EQ(first.rack_generation.value(), 1U);
    TG_CHECK_EQ(first.cooling_zone.value(), 3U);
    TG_CHECK(first.synthetic_aggregate_offset.has_value());
    TG_CHECK_EQ(*first.synthetic_aggregate_offset, 4.5);

    TG_PHASE("a domain without a parent stays parentless");
    TG_CHECK(!domains[1].parent.has_value());
    TG_CHECK_EQ(domains[1].id.value(), 2U);
}

TG_CASE(persistence, round_trip_preserves_couplings) {
    const DurableState source = make_state();
    const auto loaded = encode_then_decode(source);
    TG_CHECK(loaded.has_value());

    const auto& couplings = loaded.value().state.couplings;
    TG_CHECK_EQ(couplings.size(), std::size_t{2});
    TG_CHECK_EQ(couplings[0].id.value(), 1U);
    TG_CHECK_EQ(couplings[0].source.value(), 1U);
    TG_CHECK_EQ(couplings[0].destination.value(), 2U);
    TG_CHECK(couplings[0].type == CouplingType::SHARES_CHASSIS);
    TG_CHECK_EQ(couplings[0].generation.value(), 2U);
    TG_CHECK(couplings[0].provenance == Provenance::SYNTHETIC);
    TG_CHECK_EQ(couplings[0].evidence_source, std::string("chassis"));
    TG_CHECK_EQ(couplings[0].weight, 0.5);
    TG_CHECK(!couplings[0].directed);
}

TG_CASE(persistence, coupling_expiry_is_deliberately_not_restored) {
    const DurableState source = make_state();
    TG_CHECK(source.couplings[1].valid_until.has_value());

    const auto loaded = encode_then_decode(source);
    TG_CHECK(loaded.has_value());
    const auto& restored = loaded.value().state.couplings[1];
    TG_CHECK_EQ(restored.id.value(), 2U);
    TG_CHECK(!restored.valid_until.has_value());
}

TG_CASE(persistence, round_trip_preserves_transitions) {
    const DurableState source = make_state();
    const auto loaded = encode_then_decode(source);
    TG_CHECK(loaded.has_value());

    const auto& transitions = loaded.value().state.transitions;
    TG_CHECK_EQ(transitions.size(), std::size_t{1});
    const StateTransitionRecord& record = transitions[0];
    TG_CHECK_EQ(record.domain.value(), 1U);
    TG_CHECK_EQ(record.domain_generation.value(), 3U);
    TG_CHECK(record.from == ThermalState::WARM);
    TG_CHECK(record.to == ThermalState::DERATED);
    TG_CHECK(record.derating_from == DeratingLevel::FULL_CAPABILITY);
    TG_CHECK(record.derating_to == DeratingLevel::DERATED_CONCURRENCY);
    TG_CHECK(record.decision == ThermalDecision::ALLOW_DERATED);
    TG_CHECK_EQ(record.temperature.value(), 82.5);
    TG_CHECK_EQ(record.policy_generation.value(), 2U);
    TG_CHECK_EQ(record.telemetry_generation.value(), 8U);
    TG_CHECK_EQ(record.coordinator_epoch.value(), 7U);
    TG_CHECK_EQ(record.sequence, 12U);
}

TG_CASE(persistence, round_trip_preserves_actions) {
    const DurableState source = make_state();
    const auto loaded = encode_then_decode(source);
    TG_CHECK(loaded.has_value());

    const auto& actions = loaded.value().state.actions;
    TG_CHECK_EQ(actions.size(), std::size_t{2});

    const ThermalAction& action = actions[0];
    TG_CHECK_EQ(action.id.value(), 5U);
    TG_CHECK_EQ(action.generation.value(), 2U);
    TG_CHECK_EQ(action.authority.coordinator_epoch.value(), 7U);
    TG_CHECK_EQ(action.authority.action_id.value(), 5U);
    TG_CHECK_EQ(action.authority.action_generation.value(), 2U);
    TG_CHECK_EQ(action.authority.domain.value(), 1U);
    TG_CHECK_EQ(action.authority.domain_generation.value(), 3U);
    TG_CHECK_EQ(action.authority.device.value(), 5U);
    TG_CHECK_EQ(action.authority.device_generation.value(), 6U);
    TG_CHECK_EQ(action.authority.policy_generation.value(), 2U);
    TG_CHECK_EQ(action.authority.telemetry_generation.value(), 8U);
    TG_CHECK_EQ(action.authority.worker.value(), 4U);
    TG_CHECK_EQ(action.authority.worker_boot.value(), 9U);
    TG_CHECK_EQ(action.authority.topology_generation.value(), 1U);
    TG_CHECK_EQ(action.authority.capability_generation.value(), 2U);
    TG_CHECK(action.intent == MitigationIntent::REQUEST_CONCURRENCY_REDUCTION);
    TG_CHECK(action.lifecycle == ActionLifecycle::DISPATCHED);
    TG_CHECK(action.provenance == Provenance::SYNTHETIC);
    TG_CHECK(action.requested_clock_ceiling.has_value());
    TG_CHECK_EQ(action.requested_clock_ceiling->value(), 1500U);
    TG_CHECK(action.requested_concurrency.has_value());
    TG_CHECK_EQ(action.requested_concurrency->value(), 50.0);
    TG_CHECK(action.requested_admission.has_value());
    TG_CHECK_EQ(action.requested_admission->value(), 25.5);
    TG_CHECK_EQ(action.detail, std::string("reduce concurrency"));
    TG_CHECK_EQ(action.verification_generation.value(), 1U);
    TG_CHECK_EQ(action.verification_telemetry_generation.value(), 8U);
    TG_CHECK(action.superseded_by.has_value());
    TG_CHECK_EQ(action.superseded_by->value(), 105U);
    TG_CHECK_EQ(action.authorization_source, std::string("operator"));

    TG_PHASE("the second action keeps its own identity");
    TG_CHECK_EQ(actions[1].id.value(), 6U);
    TG_CHECK(actions[1].superseded_by.has_value());
    TG_CHECK_EQ(actions[1].superseded_by->value(), 106U);
}

TG_CASE(persistence, round_trip_preserves_verifications) {
    const DurableState source = make_state();
    const auto loaded = encode_then_decode(source);
    TG_CHECK(loaded.has_value());

    const auto& verifications = loaded.value().state.verifications;
    TG_CHECK_EQ(verifications.size(), std::size_t{1});
    const ActionVerification& verification = verifications[0];
    TG_CHECK_EQ(verification.action_id.value(), 5U);
    TG_CHECK_EQ(verification.generation.value(), 3U);
    TG_CHECK(verification.outcome == VerificationOutcome::DERATING_EFFECTIVE);
    TG_CHECK(verification.resulting_lifecycle == ActionLifecycle::EFFECTIVE);
    TG_CHECK_EQ(verification.temperature_before.value(), 91.25);
    TG_CHECK_EQ(verification.temperature_after.value(), 84.5);
    TG_CHECK_EQ(verification.delta.value(), -6.75);
    TG_CHECK_EQ(verification.headroom_before.value(), -5.25);
    TG_CHECK_EQ(verification.headroom_after.value(), 1.5);
    TG_CHECK(verification.state_before == ThermalState::CRITICAL);
    TG_CHECK(verification.state_after == ThermalState::DERATED);
    TG_CHECK(verification.derating_before == DeratingLevel::NO_SAFE_EXECUTION);
    TG_CHECK(verification.derating_after == DeratingLevel::DERATED_CONCURRENCY);
    TG_CHECK(!verification.hard_constraint_still_violated);
    TG_CHECK(verification.derating_still_required);
    TG_CHECK(!verification.recovery_conditions_satisfied);
    TG_CHECK_EQ(verification.telemetry_generation.value(), 9U);
    TG_CHECK_EQ(verification.policy_generation.value(), 2U);
    TG_CHECK_EQ(verification.coordinator_epoch.value(), 7U);
}

TG_CASE(persistence, rejects_bad_magic) {
    std::vector<std::uint8_t> bytes = encoded_bytes(make_state());
    TG_CHECK(!bytes.empty());

    bytes[0] ^= 0xFFU;
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_CORRUPT);

    TG_PHASE("a zeroed magic is refused too");
    bytes = encoded_bytes(make_state());
    put_u64(bytes, 0, 0);
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_CORRUPT);
}

TG_CASE(persistence, rejects_unsupported_version) {
    std::vector<std::uint8_t> bytes = encoded_bytes(make_state());
    TG_CHECK(!bytes.empty());

    put_u32(bytes, 8, kPersistenceFormatVersion + 1);
    TG_ERROR_CODE(decode_durable_state(bytes),
                  ThermalErrorCode::PERSISTENCE_UNSUPPORTED_VERSION);

    TG_PHASE("an unsupported embedded state version is refused too");
    DurableState future = make_state();
    future.format_version = kPersistenceFormatVersion + 1;
    TG_ERROR_CODE(encode_then_decode(future),
                  ThermalErrorCode::PERSISTENCE_UNSUPPORTED_VERSION);
}

TG_CASE(persistence, rejects_truncated_container) {
    const std::vector<std::uint8_t> bytes = encoded_bytes(make_state());
    TG_CHECK(bytes.size() > kHeaderSize + kRecordHeaderSize);

    std::vector<std::uint8_t> truncated(bytes.begin(), bytes.end() - 8);
    TG_ERROR_CODE(decode_durable_state(truncated), ThermalErrorCode::PERSISTENCE_TRUNCATED);

    truncated.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize - 4));
    TG_ERROR_CODE(decode_durable_state(truncated), ThermalErrorCode::PERSISTENCE_TRUNCATED);

    truncated.clear();
    TG_ERROR_CODE(decode_durable_state(truncated), ThermalErrorCode::PERSISTENCE_TRUNCATED);

    TG_PHASE("cutting the body short is a truncation, not corruption");
    truncated.assign(bytes.begin(),
                     bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() - 32));
    TG_ERROR_CODE(decode_durable_state(truncated), ThermalErrorCode::PERSISTENCE_TRUNCATED);
}

TG_CASE(persistence, rejects_trailing_garbage) {
    std::vector<std::uint8_t> bytes = encoded_bytes(make_state());
    TG_CHECK(!bytes.empty());

    bytes.push_back(0x00U);
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_TRAILING_GARBAGE);

    TG_PHASE("a whole second container appended is trailing garbage");
    const std::vector<std::uint8_t> second = encoded_bytes(make_state());
    bytes.insert(bytes.end(), second.begin(), second.end());
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_TRAILING_GARBAGE);

    TG_PHASE("the untampered container still decodes");
    TG_OK(decode_durable_state(encoded_bytes(make_state())));
}

TG_CASE(persistence, rejects_corrupted_header_crc) {
    TG_PHASE("a flipped byte in the covered header region");
    std::vector<std::uint8_t> bytes = encoded_bytes(make_state());
    TG_CHECK(!bytes.empty());
    bytes[12] ^= 0xFFU;
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_INTEGRITY);

    TG_PHASE("a flipped byte inside the checksum field itself");
    bytes = encoded_bytes(make_state());
    bytes[32] ^= 0xFFU;
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_INTEGRITY);

    TG_PHASE("repairing the header checksum restores decodability");
    bytes = encoded_bytes(make_state());
    bytes[12] ^= 0xFFU;
    repair_header_crc(bytes);
    TG_OK(decode_durable_state(bytes));
}

TG_CASE(persistence, rejects_corrupted_body_crc) {
    std::vector<std::uint8_t> bytes = encoded_bytes(make_state());
    TG_CHECK(!bytes.empty());

    const std::size_t body_length = static_cast<std::size_t>(get_u64(bytes, kBodyLengthOffset));
    bytes[kHeaderSize + body_length] ^= 0xFFU;
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_INTEGRITY);

    TG_PHASE("a flipped body byte is caught by the body checksum");
    bytes = encoded_bytes(make_state());
    // The body checksum is verified before any record is parsed, so an edit
    // anywhere in the body is rejected even when the inner record checksum is
    // left untouched.
    bytes[kHeaderSize + 2] ^= 0xFFU;
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_INTEGRITY);

    bytes = encoded_bytes(make_state());
    bytes[kHeaderSize + kRecordHeaderSize] ^= 0xFFU;
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_INTEGRITY);

    TG_PHASE("the body checksum is a plain CRC over the body, checksum field excluded");
    bytes = encoded_bytes(make_state());
    const std::size_t body_length_again =
        static_cast<std::size_t>(get_u64(bytes, kBodyLengthOffset));
    TG_CHECK_EQ(get_u32(bytes, kHeaderSize + body_length_again),
                crc32c(bytes.data() + kHeaderSize, body_length_again));

    TG_PHASE("a re-checksummed record edit is invisible to the body checksum");
    // A record checksum is itself a CRC over the record, so re-checksumming an
    // edited record produces a difference whose CRC residue is zero: the body
    // checksum cannot distinguish it. This case therefore asserts the exact
    // behaviour rather than an expectation the format cannot meet.
    bytes = encoded_bytes(make_state());
    bytes[kHeaderSize + 2] ^= 0xFFU;
    repair_record_crc(bytes, kHeaderSize);
    const std::size_t body_after = static_cast<std::size_t>(get_u64(bytes, kBodyLengthOffset));
    TG_CHECK_EQ(crc32c(bytes.data() + kHeaderSize, body_after),
                get_u32(bytes, kHeaderSize + body_after));
    TG_OK(decode_durable_state(bytes));
}

TG_CASE(persistence, rejects_corrupted_record_crc) {
    std::vector<std::uint8_t> bytes = encoded_bytes(make_state());
    TG_CHECK(!bytes.empty());

    const RecordLocation policy = find_record(bytes, PersistenceRecordType::POLICY);
    TG_CHECK(policy.payload_length > 0);
    bytes[policy.payload_offset] ^= 0xFFU;
    repair_body_crc(bytes);
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_INTEGRITY);

    TG_PHASE("repairing the record checksum restores decodability");
    bytes = encoded_bytes(make_state());
    bytes[policy.payload_offset] ^= 0xFFU;
    repair_record_crc(bytes, policy.record_offset);
    repair_body_crc(bytes);
    TG_CHECK(decode_durable_state(bytes).has_value());
}

TG_CASE(persistence, rejects_absurd_record_count) {
    const PersistenceLimits limits;
    std::vector<std::uint8_t> bytes = encoded_bytes(make_state());
    TG_CHECK(!bytes.empty());

    put_u64(bytes, kRecordCountOffset, static_cast<std::uint64_t>(limits.max_records) + 1U);
    repair_header_crc(bytes);
    TG_ERROR_CODE(decode_durable_state(bytes, limits), ThermalErrorCode::PERSISTENCE_CORRUPT);

    TG_PHASE("a record count below the real one leaves unread body bytes");
    bytes = encoded_bytes(make_state());
    put_u64(bytes, kRecordCountOffset, 1U);
    repair_header_crc(bytes);
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_TRAILING_GARBAGE);
}

TG_CASE(persistence, rejects_absurd_body_length) {
    const PersistenceLimits limits;
    std::vector<std::uint8_t> bytes = encoded_bytes(make_state());
    TG_CHECK(!bytes.empty());

    put_u64(bytes, kBodyLengthOffset, static_cast<std::uint64_t>(limits.max_file_bytes) + 1U);
    repair_header_crc(bytes);
    TG_ERROR_CODE(decode_durable_state(bytes, limits), ThermalErrorCode::PERSISTENCE_CORRUPT);

    TG_PHASE("a body length far beyond the buffer is a truncation");
    bytes = encoded_bytes(make_state());
    put_u64(bytes, kBodyLengthOffset, 4096U);
    repair_header_crc(bytes);
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_TRUNCATED);
}

TG_CASE(persistence, rejects_unknown_record_type) {
    std::vector<std::uint8_t> bytes = encoded_bytes(make_state());
    TG_CHECK(!bytes.empty());

    const RecordLocation first = locate_records(bytes).front();
    put_u16(bytes, first.record_offset, std::uint16_t{9});
    repair_record_crc(bytes, first.record_offset);
    repair_body_crc(bytes);
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_CORRUPT);

    TG_PHASE("a zero record type is equally unknown");
    bytes = encoded_bytes(make_state());
    put_u16(bytes, first.record_offset, std::uint16_t{0});
    repair_record_crc(bytes, first.record_offset);
    repair_body_crc(bytes);
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_CORRUPT);
}

TG_CASE(persistence, rejects_duplicate_policy_identity) {
    DurableState state = make_state();
    state.policies[1].id = state.policies[0].id;
    TG_ERROR_CODE(encode_then_decode(state), ThermalErrorCode::PERSISTENCE_CORRUPT);

    TG_PHASE("distinct identities still decode");
    state = make_state();
    TG_OK(encode_then_decode(state));
}

TG_CASE(persistence, rejects_duplicate_domain_identity) {
    DurableState state = make_state();
    state.domains[1].id = state.domains[0].id;
    TG_ERROR_CODE(encode_then_decode(state), ThermalErrorCode::PERSISTENCE_CORRUPT);
}

TG_CASE(persistence, rejects_duplicate_coupling_identity) {
    DurableState state = make_state();
    state.couplings[1].id = state.couplings[0].id;
    TG_ERROR_CODE(encode_then_decode(state), ThermalErrorCode::PERSISTENCE_CORRUPT);
}

TG_CASE(persistence, rejects_duplicate_action_identity) {
    DurableState state = make_state();
    state.actions[1].id = state.actions[0].id;
    TG_ERROR_CODE(encode_then_decode(state), ThermalErrorCode::PERSISTENCE_CORRUPT);
}

TG_CASE(persistence, rejects_invalid_enumeration_inside_a_payload) {
    TG_PHASE("an invalid domain type inside the domain payload");
    std::vector<std::uint8_t> bytes = encoded_bytes(make_state());
    TG_CHECK(!bytes.empty());
    const RecordLocation domain = find_record(bytes, PersistenceRecordType::DOMAIN_DEFINITION);
    TG_CHECK(domain.payload_length > 0);
    bytes[domain.payload_offset + 16] = 9U;   // id u64 then generation u64 then type u8
    repair_record_crc(bytes, domain.record_offset);
    repair_body_crc(bytes);
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_CORRUPT);

    TG_PHASE("an invalid behaviour enumeration inside the policy payload");
    bytes = encoded_bytes(make_state());
    const RecordLocation policy = find_record(bytes, PersistenceRecordType::POLICY);
    TG_CHECK(policy.payload_length > 0);
    // The policy payload ends with the workload restrictions, the allowed
    // intent mask and the two behaviour enumerations. Working backwards:
    // 4 + 6 * 11 bytes of restrictions and counts, then a 4 byte intent mask,
    // then the unsupported behaviour byte and the unknown behaviour byte.
    bytes[policy.payload_offset + policy.payload_length - 75] = 9U;
    repair_record_crc(bytes, policy.record_offset);
    repair_body_crc(bytes);
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_CORRUPT);

    TG_PHASE("an invalid workload profile inside a restriction");
    bytes = encoded_bytes(make_state());
    const std::size_t restriction_start =
        policy.payload_offset + policy.payload_length - 6U * 11U;
    bytes[restriction_start] = 42U;
    repair_record_crc(bytes, policy.record_offset);
    repair_body_crc(bytes);
    TG_ERROR_CODE(decode_durable_state(bytes), ThermalErrorCode::PERSISTENCE_CORRUPT);
}

TG_CASE(persistence, durable_store_round_trip_and_cleanup) {
    const std::string path = tg::unique_temp_path("thermal-governor-durable");
    const std::string temporary = path + ".tmp";
    tg::remove_file(path);
    tg::remove_file(temporary);

    DurableStore store{path};
    TG_CHECK_EQ(store.path(), path);
    TG_CHECK(!store.exists());

    TG_PHASE("save publishes the authoritative file and removes the sibling");
    const DurableState state = make_state();
    TG_STATUS_OK(store.save(state));
    TG_CHECK(store.exists());
    std::string sibling;
    TG_CHECK(!tg::read_file(temporary, sibling));

    TG_PHASE("load restores exactly what was saved");
    const auto loaded = store.load();
    TG_CHECK(loaded.has_value());
    TG_CHECK_EQ(loaded.value().state.coordinator.value(), state.coordinator.value());
    TG_CHECK_EQ(loaded.value().state.sequence, state.sequence);
    TG_CHECK_EQ(loaded.value().state.policies.size(), std::size_t{2});
    TG_CHECK_EQ(loaded.value().state.domains.size(), std::size_t{2});
    TG_CHECK_EQ(loaded.value().state.couplings.size(), std::size_t{2});
    TG_CHECK_EQ(loaded.value().state.transitions.size(), std::size_t{1});
    TG_CHECK_EQ(loaded.value().state.actions.size(), std::size_t{2});
    TG_CHECK_EQ(loaded.value().state.verifications.size(), std::size_t{1});
    TG_CHECK_EQ(loaded.value().records_read, kExpectedRecords);

    TG_PHASE("an overwrite keeps the store usable");
    DurableState updated = state;
    updated.sequence = 1234;
    TG_STATUS_OK(store.save(updated));
    const auto reloaded = store.load();
    TG_CHECK(reloaded.has_value());
    TG_CHECK_EQ(reloaded.value().state.sequence, 1234U);

    TG_PHASE("a corrupted file is reported, never silently accepted");
    const std::string garbage(64, 'x');
    TG_CHECK(tg::write_file(path, garbage));
    TG_ERROR_CODE(store.load(), ThermalErrorCode::PERSISTENCE_CORRUPT);

    TG_PHASE("remove_all removes the file and is idempotent");
    TG_STATUS_OK(store.remove_all());
    TG_CHECK(!store.exists());
    std::string content;
    TG_CHECK(!tg::read_file(path, content));
    TG_STATUS_OK(store.remove_all());
    TG_CHECK(!store.exists());
    tg::remove_file(temporary);
}

TG_CASE(persistence, durable_store_rejects_empty_path) {
    DurableStore store{std::string{}};
    TG_STATUS_ERROR_CODE(store.save(make_state()), ThermalErrorCode::INVALID_ARGUMENT);
    TG_ERROR_CODE(store.load(), ThermalErrorCode::INVALID_ARGUMENT);
    TG_CHECK(!store.exists());
}
