// Thermal Governor — versioned, integrity-checked durable state.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/persistence.hpp"

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

#include "thermal_governor/capability.hpp"
#include "thermal_governor/decision.hpp"
#include "thermal_governor/mitigation.hpp"
#include "thermal_governor/thermal_state.hpp"
#include "thermal_governor/version.hpp"

#include "binary.hpp"

namespace thermal_governor {
namespace {

/// Total container header extent on the wire:
///   magic u64 | version u32 | reserved u32 | record count u64 |
///   body length u64 | header checksum u32
constexpr std::size_t kContainerHeaderSize = 36;
/// Bytes covered by the header checksum. The checksum field itself is
/// deliberately excluded, so a checksum never covers itself.
constexpr std::size_t kContainerChecksumCoverage = 32;
constexpr std::size_t kContainerTrailerSize = 4;
constexpr std::size_t kRecordHeaderSize = 8;
constexpr std::size_t kRecordTrailerSize = 4;

// --- CRC-32C --------------------------------------------------------------

struct Crc32cTable {
    std::array<std::uint32_t, 256> values{};
    constexpr Crc32cTable() {
        constexpr std::uint32_t polynomial = 0x82F63B78U;
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1U) != 0U ? (crc >> 1) ^ polynomial : crc >> 1;
            }
            values[i] = crc;
        }
    }
};

constexpr Crc32cTable kCrcTable{};

// --- Bounds-checked encoding primitives -----------------------------------

// The encoder and decoder are shared with the wire codec so that a single
// audited implementation covers both durable state and framed transport.
using Writer = detail::BinaryWriter;
using Reader = detail::BinaryReader;

template <class Enum>
[[nodiscard]] bool enum_valid(std::uint32_t raw, std::uint32_t exclusive_max) {
    return raw < exclusive_max;
}

[[nodiscard]] Result<DegreesCelsius> read_temperature(Reader& reader) {
    const double raw = reader.real();
    if (!reader.ok()) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT, "temperature read failed"};
    }
    auto parsed = DegreesCelsius::try_from(raw);
    if (!parsed.has_value()) {
        return ThermalError{ThermalErrorCode::TEMPERATURE_INVALID,
                            "persisted temperature is out of range"};
    }
    return *parsed;
}

void write_string_vector(Writer& writer, const std::vector<std::string>& values) {
    writer.u32(static_cast<std::uint32_t>(values.size()));
    for (const auto& value : values) {
        writer.text(value);
    }
}

std::vector<std::string> read_string_vector(Reader& reader, std::size_t max_count,
                                            std::size_t max_string) {
    std::vector<std::string> out;
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > max_count) {
        reader.fail();
        return out;
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        out.push_back(reader.text(max_string));
    }
    return out;
}

// --- Record codecs --------------------------------------------------------

void write_policy(Writer& writer, const ThermalPolicy& policy) {
    writer.u64(policy.id.value());
    writer.u64(policy.generation.value());
    writer.text(policy.label.text());

    writer.real(policy.thresholds.warning.value());
    writer.real(policy.thresholds.derating.value());
    writer.real(policy.thresholds.critical.value());
    writer.real(policy.thresholds.recovery.value());
    writer.real(policy.thresholds.near_limit_band.value());

    writer.real(policy.margins.policy_safety_margin.value());
    writer.real(policy.margins.uncertainty_margin.value());
    writer.real(policy.margins.recovery_margin.value());

    writer.i64(policy.freshness.max_age.count());
    writer.u32(policy.freshness.required_consecutive_samples);
    writer.i64(policy.freshness.min_evidence_span.count());
    writer.u32(policy.freshness.min_distinct_witnesses);

    writer.u8(static_cast<std::uint8_t>(policy.recovery.require_no_thermal_throttle ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(
        policy.recovery.require_headroom_above_recovery_margin ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(policy.recovery.require_coupled_domains_recovered ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(policy.recovery.require_same_worker_boot ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(policy.recovery.require_same_coordinator_epoch ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(policy.recovery.require_same_device_generation ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(policy.recovery.require_explicit_authorization ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(
        policy.recovery.forbid_recovery_after_unsupported_evidence ? 1 : 0));

    writer.u8(static_cast<std::uint8_t>(policy.verification.required ? 1 : 0));
    writer.real(policy.verification.improvement_epsilon.value());
    writer.u32(policy.verification.required_samples);

    writer.real(policy.admission.min_effective_headroom.value());
    writer.u8(static_cast<std::uint8_t>(policy.admission.allow_derated_admission ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(policy.admission.allow_defer ? 1 : 0));

    writer.real(policy.concurrency.normal.value());
    writer.real(policy.concurrency.warm.value());
    writer.real(policy.concurrency.near_limit.value());
    writer.real(policy.concurrency.derated.value());
    writer.real(policy.concurrency.critical.value());

    writer.u8(static_cast<std::uint8_t>(policy.emergency.deny_all_execution ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(policy.emergency.allow_containment_intent ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(policy.emergency.allow_migration_intent ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(
        policy.emergency.allow_cooling_intervention_intent ? 1 : 0));

    writer.u8(static_cast<std::uint8_t>(policy.coupling.co_derate_coupled_domains ? 1 : 0));
    writer.u32(policy.coupling.max_propagation_depth);
    writer.u8(static_cast<std::uint8_t>(policy.coupling.require_coupled_recovery ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(policy.coupling.escalate_to_hottest_neighbour ? 1 : 0));

    writer.u8(static_cast<std::uint8_t>(policy.unknown_behavior));
    writer.u8(static_cast<std::uint8_t>(policy.unsupported_behavior));
    writer.u32(policy.allowed_intents);

    writer.u32(static_cast<std::uint32_t>(policy.workload_restrictions.size()));
    for (const auto& restriction : policy.workload_restrictions) {
        writer.u8(static_cast<std::uint8_t>(restriction.profile));
        writer.u8(static_cast<std::uint8_t>(restriction.allowed ? 1 : 0));
        writer.u8(static_cast<std::uint8_t>(restriction.required_class));
        writer.real(restriction.extra_headroom_demand.value());
    }
}

[[nodiscard]] Result<ThermalPolicy> read_policy(Reader& reader, const PersistenceLimits& limits) {
    ThermalPolicy policy;
    policy.id = ThermalPolicyId{StrongId<ThermalPolicyIdTag>{reader.u64()}};
    policy.generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{reader.u64()}};
    policy.label = Label{reader.text(limits.max_string_bytes)};

    policy.thresholds.warning = DegreesCelsius{};
    const auto set_temp = [&](DegreesCelsius& target) -> bool {
        auto parsed = read_temperature(reader);
        if (!parsed.has_value()) {
            return false;
        }
        target = parsed.value();
        return true;
    };
    if (!set_temp(policy.thresholds.warning) || !set_temp(policy.thresholds.derating) ||
        !set_temp(policy.thresholds.critical) || !set_temp(policy.thresholds.recovery)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT, "policy threshold decode failed"};
    }
    policy.thresholds.near_limit_band = TemperatureDelta{reader.real()};
    policy.margins.policy_safety_margin = TemperatureDelta{reader.real()};
    policy.margins.uncertainty_margin = TemperatureDelta{reader.real()};
    policy.margins.recovery_margin = TemperatureDelta{reader.real()};

    policy.freshness.max_age = Milliseconds{reader.i64()};
    policy.freshness.required_consecutive_samples = reader.u32();
    policy.freshness.min_evidence_span = Milliseconds{reader.i64()};
    policy.freshness.min_distinct_witnesses = reader.u32();

    const auto flag = [&reader]() { return reader.u8() != 0U; };
    policy.recovery.require_no_thermal_throttle = flag();
    policy.recovery.require_headroom_above_recovery_margin = flag();
    policy.recovery.require_coupled_domains_recovered = flag();
    policy.recovery.require_same_worker_boot = flag();
    policy.recovery.require_same_coordinator_epoch = flag();
    policy.recovery.require_same_device_generation = flag();
    policy.recovery.require_explicit_authorization = flag();
    policy.recovery.forbid_recovery_after_unsupported_evidence = flag();

    policy.verification.required = flag();
    policy.verification.improvement_epsilon = TemperatureDelta{reader.real()};
    policy.verification.required_samples = reader.u32();

    policy.admission.min_effective_headroom = TemperatureDelta{reader.real()};
    policy.admission.allow_derated_admission = flag();
    policy.admission.allow_defer = flag();

    policy.concurrency.normal = Percent{reader.real()};
    policy.concurrency.warm = Percent{reader.real()};
    policy.concurrency.near_limit = Percent{reader.real()};
    policy.concurrency.derated = Percent{reader.real()};
    policy.concurrency.critical = Percent{reader.real()};

    policy.emergency.deny_all_execution = flag();
    policy.emergency.allow_containment_intent = flag();
    policy.emergency.allow_migration_intent = flag();
    policy.emergency.allow_cooling_intervention_intent = flag();

    policy.coupling.co_derate_coupled_domains = flag();
    policy.coupling.max_propagation_depth = reader.u32();
    policy.coupling.require_coupled_recovery = flag();
    policy.coupling.escalate_to_hottest_neighbour = flag();

    const std::uint8_t unknown = reader.u8();
    const std::uint8_t unsupported = reader.u8();
    if (!enum_valid<UnknownBehavior>(unknown, 5) ||
        !enum_valid<UnsupportedBehavior>(unsupported, 4)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "policy carries an invalid behaviour enumeration"};
    }
    policy.unknown_behavior = static_cast<UnknownBehavior>(unknown);
    policy.unsupported_behavior = static_cast<UnsupportedBehavior>(unsupported);
    policy.allowed_intents = reader.u32();

    const std::uint32_t restriction_count = reader.u32();
    if (!reader.ok() || restriction_count > 64U) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "policy workload restriction count is out of range"};
    }
    policy.workload_restrictions.reserve(restriction_count);
    for (std::uint32_t i = 0; i < restriction_count; ++i) {
        WorkloadRestriction restriction;
        const std::uint8_t profile = reader.u8();
        const std::uint8_t allowed = reader.u8();
        const std::uint8_t exec_class = reader.u8();
        restriction.extra_headroom_demand = TemperatureDelta{reader.real()};
        if (!enum_valid<WorkloadThermalProfile>(profile, 6) ||
            !enum_valid<ExecutionClass>(exec_class, 4) || allowed > 1U) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                "policy workload restriction carries an invalid enumeration"};
        }
        restriction.profile = static_cast<WorkloadThermalProfile>(profile);
        restriction.allowed = allowed != 0U;
        restriction.required_class = static_cast<ExecutionClass>(exec_class);
        policy.workload_restrictions.push_back(restriction);
    }
    if (!reader.ok()) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT, "policy decode failed"};
    }
    return policy;
}

void write_subject(Writer& writer, const SubjectRef& subject) {
    writer.u8(static_cast<std::uint8_t>(subject.kind));
    writer.u64(subject.device.value());
    writer.u64(subject.device_generation.value());
    writer.u64(subject.node.value());
    writer.u64(subject.node_generation.value());
    writer.u64(subject.rack.value());
    writer.u64(subject.rack_generation.value());
    writer.u64(subject.cooling_zone.value());
    writer.u64(subject.chassis.value());
    writer.u64(subject.domain.value());
    writer.u64(subject.domain_generation.value());
}

[[nodiscard]] bool read_subject(Reader& reader, SubjectRef& subject) {
    const std::uint8_t kind = reader.u8();
    if (!enum_valid<SubjectKind>(kind, 6)) {
        return false;
    }
    subject.kind = static_cast<SubjectKind>(kind);
    subject.device = DeviceId{StrongId<DeviceIdTag>{reader.u64()}};
    subject.device_generation = DeviceGeneration{StrongId<DeviceGenerationTag>{reader.u64()}};
    subject.node = NodeId{StrongId<NodeIdTag>{reader.u64()}};
    subject.node_generation = NodeGeneration{StrongId<NodeGenerationTag>{reader.u64()}};
    subject.rack = RackId{StrongId<RackIdTag>{reader.u64()}};
    subject.rack_generation = RackGeneration{StrongId<RackGenerationTag>{reader.u64()}};
    subject.cooling_zone = CoolingZoneId{StrongId<CoolingZoneIdTag>{reader.u64()}};
    subject.chassis = ChassisId{StrongId<ChassisIdTag>{reader.u64()}};
    subject.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{reader.u64()}};
    subject.domain_generation =
        ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{reader.u64()}};
    return reader.ok();
}

void write_device(Writer& writer, const DeviceRegistration& device) {
    writer.u64(device.device.value());
    writer.u64(device.generation.value());
    writer.u64(device.node.value());
    writer.u64(device.node_generation.value());
    writer.u64(device.rack.value());
    writer.u64(device.rack_generation.value());
    writer.text(device.label.text());
    writer.u8(static_cast<std::uint8_t>(device.provenance));
    writer.u64(device.capabilities.generation.value());
    for (std::size_t i = 0; i < kThermalCapabilityCount; ++i) {
        const auto capability = static_cast<ThermalCapability>(i);
        writer.u8(static_cast<std::uint8_t>(device.capabilities.get(capability)));
        writer.text(device.capabilities.detail(capability));
    }
}

[[nodiscard]] Result<DeviceRegistration> read_device(Reader& reader,
                                                     const PersistenceLimits& limits) {
    DeviceRegistration device;
    device.device = DeviceId{StrongId<DeviceIdTag>{reader.u64()}};
    device.generation = DeviceGeneration{StrongId<DeviceGenerationTag>{reader.u64()}};
    device.node = NodeId{StrongId<NodeIdTag>{reader.u64()}};
    device.node_generation = NodeGeneration{StrongId<NodeGenerationTag>{reader.u64()}};
    device.rack = RackId{StrongId<RackIdTag>{reader.u64()}};
    device.rack_generation = RackGeneration{StrongId<RackGenerationTag>{reader.u64()}};
    device.label = Label{reader.text(limits.max_string_bytes)};
    const std::uint8_t provenance = reader.u8();
    if (!reader.ok() || provenance > static_cast<std::uint8_t>(Provenance::UNSUPPORTED)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "device carries an invalid provenance"};
    }
    device.provenance = static_cast<Provenance>(provenance);
    const std::uint64_t capability_generation = reader.u64();
    for (std::size_t i = 0; i < kThermalCapabilityCount; ++i) {
        const std::uint8_t state = reader.u8();
        const std::string detail = reader.text(limits.max_string_bytes);
        if (!reader.ok() || state > static_cast<std::uint8_t>(CapabilityState::UNSUPPORTED)) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                "device carries an invalid capability state"};
        }
        device.capabilities.set(static_cast<ThermalCapability>(i),
                                static_cast<CapabilityState>(state), detail);
    }
    device.capabilities.generation =
        CapabilityGeneration{StrongId<CapabilityGenerationTag>{capability_generation}};
    if (!reader.ok()) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT, "device decode failed"};
    }
    return device;
}

void write_domain(Writer& writer, const ThermalDomainDefinition& definition) {
    writer.u64(definition.id.value());
    writer.u64(definition.generation.value());
    writer.u8(static_cast<std::uint8_t>(definition.type));
    writer.text(definition.label.text());

    writer.u32(static_cast<std::uint32_t>(definition.members.size()));
    for (const auto& member : definition.members) {
        write_subject(writer, member.subject);
        writer.real(member.weight);
    }
    writer.optional<ThermalDomainId>(
        definition.parent,
        [&writer](ThermalDomainId id) { writer.u64(id.value()); });
    writer.u32(static_cast<std::uint32_t>(definition.children.size()));
    for (const auto child : definition.children) {
        writer.u64(child.value());
    }
    writer.u64(definition.policy.value());
    writer.u64(definition.policy_generation.value());
    writer.u8(static_cast<std::uint8_t>(definition.provenance));
    writer.text(definition.evidence_source);
    writer.u8(static_cast<std::uint8_t>(definition.aggregation));
    writer.u64(definition.rack.value());
    writer.u64(definition.rack_generation.value());
    writer.u64(definition.cooling_zone.value());
    writer.optional<double>(definition.synthetic_aggregate_offset,
                            [&writer](double offset) { writer.real(offset); });
}

[[nodiscard]] Result<ThermalDomainDefinition> read_domain(Reader& reader,
                                                          const PersistenceLimits& limits) {
    ThermalDomainDefinition definition;
    definition.id = ThermalDomainId{StrongId<ThermalDomainIdTag>{reader.u64()}};
    definition.generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{reader.u64()}};
    const std::uint8_t type = reader.u8();
    if (!enum_valid<ThermalDomainType>(type, 7)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "domain carries an invalid domain type"};
    }
    definition.type = static_cast<ThermalDomainType>(type);
    definition.label = Label{reader.text(limits.max_string_bytes)};

    const std::uint32_t member_count = reader.u32();
    if (!reader.ok() || member_count > limits.max_domains) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT, "domain member count is absurd"};
    }
    definition.members.reserve(member_count);
    for (std::uint32_t i = 0; i < member_count; ++i) {
        DomainMember member;
        if (!read_subject(reader, member.subject)) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                "domain member subject is malformed"};
        }
        member.weight = reader.real();
        definition.members.push_back(member);
    }
    if (reader.presence()) {
        definition.parent = ThermalDomainId{StrongId<ThermalDomainIdTag>{reader.u64()}};
    }
    const std::uint32_t child_count = reader.u32();
    if (!reader.ok() || child_count > limits.max_domains) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT, "domain child count is absurd"};
    }
    definition.children.reserve(child_count);
    for (std::uint32_t i = 0; i < child_count; ++i) {
        definition.children.push_back(
            ThermalDomainId{StrongId<ThermalDomainIdTag>{reader.u64()}});
    }
    definition.policy = ThermalPolicyId{StrongId<ThermalPolicyIdTag>{reader.u64()}};
    definition.policy_generation =
        ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{reader.u64()}};
    const std::uint8_t provenance = reader.u8();
    if (!enum_valid<Provenance>(provenance, 4)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "domain carries an invalid provenance"};
    }
    definition.provenance = static_cast<Provenance>(provenance);
    definition.evidence_source = reader.text(limits.max_string_bytes);
    const std::uint8_t aggregation = reader.u8();
    if (!enum_valid<AggregationRule>(aggregation, 4)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "domain carries an invalid aggregation rule"};
    }
    definition.aggregation = static_cast<AggregationRule>(aggregation);
    definition.rack = RackId{StrongId<RackIdTag>{reader.u64()}};
    definition.rack_generation = RackGeneration{StrongId<RackGenerationTag>{reader.u64()}};
    definition.cooling_zone = CoolingZoneId{StrongId<CoolingZoneIdTag>{reader.u64()}};
    if (reader.presence()) {
        definition.synthetic_aggregate_offset = reader.real();
    }
    if (!reader.ok()) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT, "domain decode failed"};
    }
    return definition;
}

void write_coupling(Writer& writer, const CouplingRelation& relation) {
    writer.u64(relation.id.value());
    writer.u64(relation.source.value());
    writer.u64(relation.destination.value());
    writer.u8(static_cast<std::uint8_t>(relation.type));
    writer.u64(relation.generation.value());
    writer.u8(static_cast<std::uint8_t>(relation.provenance));
    writer.text(relation.evidence_source);
    writer.real(relation.weight);
    writer.u8(static_cast<std::uint8_t>(relation.directed ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(relation.valid_until.has_value() ? 1 : 0));
}

[[nodiscard]] Result<CouplingRelation> read_coupling(Reader& reader,
                                                     const PersistenceLimits& limits) {
    CouplingRelation relation;
    relation.id = CouplingId{StrongId<CouplingIdTag>{reader.u64()}};
    relation.source = ThermalDomainId{StrongId<ThermalDomainIdTag>{reader.u64()}};
    relation.destination = ThermalDomainId{StrongId<ThermalDomainIdTag>{reader.u64()}};
    const std::uint8_t type = reader.u8();
    if (!enum_valid<CouplingType>(type, 7)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "coupling carries an invalid coupling type"};
    }
    relation.type = static_cast<CouplingType>(type);
    relation.generation = CouplingGeneration{StrongId<CouplingGenerationTag>{reader.u64()}};
    const std::uint8_t provenance = reader.u8();
    if (!enum_valid<Provenance>(provenance, 4)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "coupling carries an invalid provenance"};
    }
    relation.provenance = static_cast<Provenance>(provenance);
    relation.evidence_source = reader.text(limits.max_string_bytes);
    relation.weight = reader.real();
    const std::uint8_t directed = reader.u8();
    const std::uint8_t has_expiry = reader.u8();
    if (directed > 1U || has_expiry > 1U) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "coupling carries an invalid flag"};
    }
    relation.directed = directed != 0U;
    if (!reader.ok()) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT, "coupling decode failed"};
    }
    // A persisted dynamic expiry is deliberately not restored: a coupling
    // observation that expired before a restart is not current knowledge.
    relation.valid_until.reset();
    return relation;
}

void write_transition(Writer& writer, const StateTransitionRecord& record) {
    writer.u64(record.domain.value());
    writer.u64(record.domain_generation.value());
    writer.u8(static_cast<std::uint8_t>(record.from));
    writer.u8(static_cast<std::uint8_t>(record.to));
    writer.u8(static_cast<std::uint8_t>(record.derating_from));
    writer.u8(static_cast<std::uint8_t>(record.derating_to));
    writer.u8(static_cast<std::uint8_t>(record.decision));
    writer.real(record.temperature.value());
    writer.u64(record.policy_generation.value());
    writer.u64(record.telemetry_generation.value());
    writer.u64(record.coordinator_epoch.value());
    writer.u64(record.sequence);
}

[[nodiscard]] Result<StateTransitionRecord> read_transition(Reader& reader) {
    StateTransitionRecord record;
    record.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{reader.u64()}};
    record.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{reader.u64()}};
    const std::uint8_t from = reader.u8();
    const std::uint8_t to = reader.u8();
    const std::uint8_t derating_from = reader.u8();
    const std::uint8_t derating_to = reader.u8();
    const std::uint8_t decision = reader.u8();
    if (!enum_valid<ThermalState>(from, kThermalStateCount) ||
        !enum_valid<ThermalState>(to, kThermalStateCount) ||
        !enum_valid<DeratingLevel>(derating_from, 10) ||
        !enum_valid<DeratingLevel>(derating_to, 10) ||
        !enum_valid<ThermalDecision>(decision, 7)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "state transition carries an invalid enumeration"};
    }
    record.from = static_cast<ThermalState>(from);
    record.to = static_cast<ThermalState>(to);
    record.derating_from = static_cast<DeratingLevel>(derating_from);
    record.derating_to = static_cast<DeratingLevel>(derating_to);
    record.decision = static_cast<ThermalDecision>(decision);
    const double temperature = reader.real();
    auto parsed = DegreesCelsius::try_from(temperature);
    if (!parsed.has_value()) {
        return ThermalError{ThermalErrorCode::TEMPERATURE_INVALID,
                            "persisted transition temperature is out of range"};
    }
    record.temperature = *parsed;
    record.policy_generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{reader.u64()}};
    record.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{reader.u64()}};
    record.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{reader.u64()}};
    record.sequence = reader.u64();
    if (!reader.ok()) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT, "state transition decode failed"};
    }
    return record;
}

void write_action(Writer& writer, const ThermalAction& action) {
    writer.u64(action.id.value());
    writer.u64(action.generation.value());
    writer.u64(action.authority.coordinator_epoch.value());
    writer.u64(action.authority.action_id.value());
    writer.u64(action.authority.action_generation.value());
    writer.u64(action.authority.domain.value());
    writer.u64(action.authority.domain_generation.value());
    writer.u64(action.authority.device.value());
    writer.u64(action.authority.device_generation.value());
    writer.u64(action.authority.policy_generation.value());
    writer.u64(action.authority.telemetry_generation.value());
    writer.u64(action.authority.worker.value());
    writer.u64(action.authority.worker_boot.value());
    writer.u64(action.authority.topology_generation.value());
    writer.u64(action.authority.capability_generation.value());
    writer.u8(static_cast<std::uint8_t>(action.intent));
    writer.u8(static_cast<std::uint8_t>(action.lifecycle));
    writer.u8(static_cast<std::uint8_t>(action.provenance));
    writer.optional<MegaHertz>(action.requested_clock_ceiling, [&writer](MegaHertz clock) {
        writer.u32(clock.value());
    });
    writer.optional<Percent>(action.requested_concurrency, [&writer](Percent percent) {
        writer.real(percent.value());
    });
    writer.optional<Percent>(action.requested_admission, [&writer](Percent percent) {
        writer.real(percent.value());
    });
    writer.text(action.detail);
    writer.u64(action.verification_generation.value());
    writer.u64(action.verification_telemetry_generation.value());
    writer.optional<ActionId>(action.superseded_by,
                              [&writer](ActionId id) { writer.u64(id.value()); });
    writer.text(action.authorization_source);
}

[[nodiscard]] Result<ThermalAction> read_action(Reader& reader, const PersistenceLimits& limits) {
    ThermalAction action;
    action.id = ActionId{StrongId<ActionIdTag>{reader.u64()}};
    action.generation = ActionGeneration{StrongId<ActionGenerationTag>{reader.u64()}};
    action.authority.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{reader.u64()}};
    action.authority.action_id = ActionId{StrongId<ActionIdTag>{reader.u64()}};
    action.authority.action_generation = ActionGeneration{StrongId<ActionGenerationTag>{reader.u64()}};
    action.authority.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{reader.u64()}};
    action.authority.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{reader.u64()}};
    action.authority.device = DeviceId{StrongId<DeviceIdTag>{reader.u64()}};
    action.authority.device_generation = DeviceGeneration{StrongId<DeviceGenerationTag>{reader.u64()}};
    action.authority.policy_generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{reader.u64()}};
    action.authority.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{reader.u64()}};
    action.authority.worker = WorkerId{StrongId<WorkerIdTag>{reader.u64()}};
    action.authority.worker_boot = WorkerBootId{StrongId<WorkerBootIdTag>{reader.u64()}};
    action.authority.topology_generation = TopologyGeneration{StrongId<TopologyGenerationTag>{reader.u64()}};
    action.authority.capability_generation = CapabilityGeneration{StrongId<CapabilityGenerationTag>{reader.u64()}};

    const std::uint8_t intent = reader.u8();
    const std::uint8_t lifecycle = reader.u8();
    const std::uint8_t provenance = reader.u8();
    if (!enum_valid<MitigationIntent>(intent, kMitigationIntentCount) ||
        !enum_valid<ActionLifecycle>(lifecycle, kActionLifecycleCount) ||
        !enum_valid<Provenance>(provenance, 4)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "action carries an invalid enumeration"};
    }
    action.intent = static_cast<MitigationIntent>(intent);
    action.lifecycle = static_cast<ActionLifecycle>(lifecycle);
    action.provenance = static_cast<Provenance>(provenance);

    if (reader.presence()) {
        action.requested_clock_ceiling = MegaHertz{reader.u32()};
    }
    if (reader.presence()) {
        const double percent = reader.real();
        auto parsed = Percent::try_from(percent);
        if (!parsed.has_value()) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                "action concurrency ceiling is out of range"};
        }
        action.requested_concurrency = *parsed;
    }
    if (reader.presence()) {
        const double percent = reader.real();
        auto parsed = Percent::try_from(percent);
        if (!parsed.has_value()) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                "action admission ceiling is out of range"};
        }
        action.requested_admission = *parsed;
    }
    action.detail = reader.text(limits.max_string_bytes);
    action.verification_generation = VerificationGeneration{StrongId<VerificationGenerationTag>{reader.u64()}};
    action.verification_telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{reader.u64()}};
    if (reader.presence()) {
        action.superseded_by = ActionId{StrongId<ActionIdTag>{reader.u64()}};
    }
    action.authorization_source = reader.text(limits.max_string_bytes);
    if (!reader.ok()) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT, "action decode failed"};
    }
    return action;
}

void write_verification(Writer& writer, const ActionVerification& verification) {
    writer.u64(verification.action_id.value());
    writer.u64(verification.generation.value());
    writer.u8(static_cast<std::uint8_t>(verification.outcome));
    writer.u8(static_cast<std::uint8_t>(verification.resulting_lifecycle));
    writer.real(verification.temperature_before.value());
    writer.real(verification.temperature_after.value());
    writer.real(verification.delta.value());
    writer.real(verification.headroom_before.value());
    writer.real(verification.headroom_after.value());
    writer.u8(static_cast<std::uint8_t>(verification.state_before));
    writer.u8(static_cast<std::uint8_t>(verification.state_after));
    writer.u8(static_cast<std::uint8_t>(verification.derating_before));
    writer.u8(static_cast<std::uint8_t>(verification.derating_after));
    writer.u8(static_cast<std::uint8_t>(verification.hard_constraint_still_violated ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(verification.derating_still_required ? 1 : 0));
    writer.u8(static_cast<std::uint8_t>(verification.recovery_conditions_satisfied ? 1 : 0));
    writer.u64(verification.telemetry_generation.value());
    writer.u64(verification.policy_generation.value());
    writer.u64(verification.coordinator_epoch.value());
}

[[nodiscard]] Result<ActionVerification> read_verification(Reader& reader) {
    ActionVerification verification;
    verification.action_id = ActionId{StrongId<ActionIdTag>{reader.u64()}};
    verification.generation = VerificationGeneration{StrongId<VerificationGenerationTag>{reader.u64()}};
    const std::uint8_t outcome = reader.u8();
    const std::uint8_t lifecycle = reader.u8();
    if (!enum_valid<VerificationOutcome>(outcome, 10) ||
        !enum_valid<ActionLifecycle>(lifecycle, kActionLifecycleCount)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "verification carries an invalid enumeration"};
    }
    verification.outcome = static_cast<VerificationOutcome>(outcome);
    verification.resulting_lifecycle = static_cast<ActionLifecycle>(lifecycle);

    const auto read_deg = [&reader](DegreesCelsius& target) -> bool {
        const double raw = reader.real();
        auto parsed = DegreesCelsius::try_from(raw);
        if (!parsed.has_value()) {
            return false;
        }
        target = *parsed;
        return true;
    };
    if (!read_deg(verification.temperature_before) || !read_deg(verification.temperature_after)) {
        return ThermalError{ThermalErrorCode::TEMPERATURE_INVALID,
                            "persisted verification temperature is out of range"};
    }
    verification.delta = TemperatureDelta{reader.real()};
    verification.headroom_before = TemperatureDelta{reader.real()};
    verification.headroom_after = TemperatureDelta{reader.real()};

    const std::uint8_t state_before = reader.u8();
    const std::uint8_t state_after = reader.u8();
    const std::uint8_t derating_before = reader.u8();
    const std::uint8_t derating_after = reader.u8();
    if (!enum_valid<ThermalState>(state_before, kThermalStateCount) ||
        !enum_valid<ThermalState>(state_after, kThermalStateCount) ||
        !enum_valid<DeratingLevel>(derating_before, 10) ||
        !enum_valid<DeratingLevel>(derating_after, 10)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "verification carries an invalid thermal state"};
    }
    verification.state_before = static_cast<ThermalState>(state_before);
    verification.state_after = static_cast<ThermalState>(state_after);
    verification.derating_before = static_cast<DeratingLevel>(derating_before);
    verification.derating_after = static_cast<DeratingLevel>(derating_after);
    verification.hard_constraint_still_violated = reader.u8() != 0U;
    verification.derating_still_required = reader.u8() != 0U;
    verification.recovery_conditions_satisfied = reader.u8() != 0U;
    verification.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{reader.u64()}};
    verification.policy_generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{reader.u64()}};
    verification.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{reader.u64()}};
    if (!reader.ok()) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT, "verification decode failed"};
    }
    return verification;
}

}  // namespace

std::uint32_t crc32c(const std::uint8_t* data, std::size_t length, std::uint32_t seed) noexcept {
    std::uint32_t crc = ~seed;
    for (std::size_t i = 0; i < length; ++i) {
        crc = kCrcTable.values[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

Result<std::vector<std::uint8_t>> encode_durable_state(const DurableState& state,
                                                       const PersistenceLimits& limits) {
    if (state.policies.size() > limits.max_policies) {
        return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "too many policies to persist"};
    }
    if (state.devices.size() > limits.max_devices) {
        return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "too many devices to persist"};
    }
    if (state.domains.size() > limits.max_domains) {
        return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "too many domains to persist"};
    }
    if (state.couplings.size() > limits.max_couplings) {
        return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "too many couplings to persist"};
    }
    if (state.transitions.size() > limits.max_transitions) {
        return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED,
                            "too many transitions to persist"};
    }
    if (state.actions.size() > limits.max_actions) {
        return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "too many actions to persist"};
    }
    if (state.verifications.size() > limits.max_verifications) {
        return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED,
                            "too many verifications to persist"};
    }

    Writer body;
    std::uint64_t records = 0;

    const auto emit = [&body, &records, &limits](PersistenceRecordType type, const Writer& payload) {
        const auto& bytes = payload.bytes();
        if (bytes.size() > limits.max_record_bytes) {
            return false;
        }
        // The record checksum covers the record header AND its payload, and
        // never the checksum field itself. The record start therefore has
        // to be captured before the header is written.
        const std::size_t start = body.bytes().size();
        body.u16(static_cast<std::uint16_t>(type));
        body.u16(0);
        body.u32(static_cast<std::uint32_t>(bytes.size()));
        for (const auto byte : bytes) {
            body.u8(byte);
        }
        const std::uint32_t crc =
            crc32c(body.bytes().data() + start, kRecordHeaderSize + bytes.size());
        body.u32(crc);
        ++records;
        return true;
    };

    {
        Writer payload;
        payload.u32(state.format_version);
        payload.u64(state.coordinator.value());
        payload.u64(state.coordinator_epoch.value());
        payload.u64(state.policy_generation.value());
        payload.u64(state.topology_generation.value());
        payload.u64(state.sequence);
        if (!emit(PersistenceRecordType::COORDINATOR_STATE, payload)) {
            return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "record too large"};
        }
    }
    for (const auto& policy : state.policies) {
        Writer payload;
        write_policy(payload, policy);
        if (!emit(PersistenceRecordType::POLICY, payload)) {
            return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "record too large"};
        }
    }
    for (const auto& device : state.devices) {
        Writer payload;
        write_device(payload, device);
        if (!emit(PersistenceRecordType::DEVICE_REGISTRATION, payload)) {
            return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "record too large"};
        }
    }
    for (const auto& domain : state.domains) {
        Writer payload;
        write_domain(payload, domain);
        if (!emit(PersistenceRecordType::DOMAIN_DEFINITION, payload)) {
            return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "record too large"};
        }
    }
    for (const auto& coupling : state.couplings) {
        Writer payload;
        write_coupling(payload, coupling);
        if (!emit(PersistenceRecordType::COUPLING, payload)) {
            return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "record too large"};
        }
    }
    for (const auto& transition : state.transitions) {
        Writer payload;
        write_transition(payload, transition);
        if (!emit(PersistenceRecordType::STATE_TRANSITION, payload)) {
            return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "record too large"};
        }
    }
    for (const auto& action : state.actions) {
        Writer payload;
        write_action(payload, action);
        if (!emit(PersistenceRecordType::ACTION_RECORD, payload)) {
            return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "record too large"};
        }
    }
    for (const auto& verification : state.verifications) {
        Writer payload;
        write_verification(payload, verification);
        if (!emit(PersistenceRecordType::VERIFICATION_RECORD, payload)) {
            return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED, "record too large"};
        }
    }

    const std::vector<std::uint8_t> body_bytes = body.take();
    if (body_bytes.size() > limits.max_file_bytes) {
        return ThermalError{ThermalErrorCode::RESOURCE_EXHAUSTED,
                            "encoded durable state exceeds the file bound"};
    }

    Writer container;
    container.u64(kPersistenceMagic);
    container.u32(kPersistenceFormatVersion);
    container.u32(0);
    container.u64(records);
    container.u64(static_cast<std::uint64_t>(body_bytes.size()));
    container.u32(crc32c(container.bytes().data(), kContainerChecksumCoverage));
    for (const auto byte : body_bytes) {
        container.u8(byte);
    }
    container.u32(crc32c(body_bytes.data(), body_bytes.size()));
    return container.take();
}

Result<PersistenceLoadResult> decode_durable_state(const std::vector<std::uint8_t>& bytes,
                                                   const PersistenceLimits& limits) {
    if (bytes.size() > limits.max_file_bytes) {
        return ThermalError{ThermalErrorCode::FRAME_TOO_LARGE,
                            "durable container exceeds the configured bound"};
    }
    if (bytes.size() < kContainerHeaderSize + kContainerTrailerSize) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_TRUNCATED,
                            "durable container is shorter than its header"};
    }

    Reader header_reader(bytes.data(), bytes.size());
    const std::uint64_t magic = header_reader.u64();
    if (magic != kPersistenceMagic) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT, "durable container magic mismatch"};
    }
    const std::uint32_t version = header_reader.u32();
    (void)header_reader.u32();
    const std::uint64_t record_count = header_reader.u64();
    const std::uint64_t body_length = header_reader.u64();
    const std::uint32_t stored_header_crc = header_reader.u32();

    if (version != kPersistenceFormatVersion) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_UNSUPPORTED_VERSION,
                            "durable container version is not supported"};
    }
    if (crc32c(bytes.data(), kContainerChecksumCoverage) != stored_header_crc) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_INTEGRITY,
                            "durable container header integrity check failed"};
    }
    if (record_count > limits.max_records) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "durable container declares an absurd record count"};
    }
    // Checked arithmetic: the declared extents must not overflow.
    if (body_length > static_cast<std::uint64_t>(limits.max_file_bytes)) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "durable container declares an absurd body length"};
    }
    const std::uint64_t expected =
        static_cast<std::uint64_t>(kContainerHeaderSize) + body_length +
        static_cast<std::uint64_t>(kContainerTrailerSize);
    if (static_cast<std::uint64_t>(bytes.size()) < expected) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_TRUNCATED,
                            "durable container body is truncated"};
    }
    if (static_cast<std::uint64_t>(bytes.size()) > expected) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_TRAILING_GARBAGE,
                            "durable container has trailing bytes after its declared extent"};
    }

    const std::uint8_t* body = bytes.data() + kContainerHeaderSize;
    const std::uint32_t stored_body_crc = static_cast<std::uint32_t>(
        bytes[kContainerHeaderSize + static_cast<std::size_t>(body_length)]) |
        (static_cast<std::uint32_t>(
             bytes[kContainerHeaderSize + static_cast<std::size_t>(body_length) + 1]) << 8) |
        (static_cast<std::uint32_t>(
             bytes[kContainerHeaderSize + static_cast<std::size_t>(body_length) + 2]) << 16) |
        (static_cast<std::uint32_t>(
             bytes[kContainerHeaderSize + static_cast<std::size_t>(body_length) + 3]) << 24);
    if (crc32c(body, static_cast<std::size_t>(body_length)) != stored_body_crc) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_INTEGRITY,
                            "durable container body integrity check failed"};
    }

    PersistenceLoadResult result;
    result.state.format_version = version;

    std::set<std::uint64_t> policy_ids;
    std::set<std::uint64_t> device_ids;
    std::set<std::uint64_t> domain_ids;
    std::set<std::uint64_t> coupling_ids;
    std::set<std::uint64_t> action_ids;

    Reader reader(body, static_cast<std::size_t>(body_length));
    for (std::uint64_t index = 0; index < record_count; ++index) {
        const std::size_t record_start = reader.position();
        if (!reader.need(kRecordHeaderSize)) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_TRUNCATED,
                                "durable container ended inside a record header"};
        }
        const std::uint16_t raw_type = reader.u16();
        (void)reader.u16();
        const std::uint32_t payload_length = reader.u32();
        if (!reader.ok()) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_TRUNCATED,
                                "durable container record header is truncated"};
        }
        if (payload_length > limits.max_record_bytes) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                "durable container declares an absurd record length"};
        }
        if (raw_type == 0 || raw_type > 9) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                "durable container carries an unknown record type"};
        }
        if (!reader.need(static_cast<std::size_t>(payload_length) + kRecordTrailerSize)) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_TRUNCATED,
                                "durable container record payload is truncated"};
        }

        std::vector<std::uint8_t> payload = reader.raw_bytes(payload_length);
        const std::uint32_t stored_record_crc = reader.u32();
        if (!reader.ok()) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_TRUNCATED,
                                "durable container record trailer is truncated"};
        }
        // Record checksum covers the record header and its payload, and
        // never the checksum field itself.
        const std::uint32_t computed_record_crc =
            crc32c(body + record_start, kRecordHeaderSize + payload_length);
        if (computed_record_crc != stored_record_crc) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_INTEGRITY,
                                "durable container record integrity check failed"};
        }

        Reader payload_reader(payload.data(), payload.size());
        switch (static_cast<PersistenceRecordType>(raw_type)) {
            case PersistenceRecordType::COORDINATOR_STATE: {
                result.state.format_version = payload_reader.u32();
                result.state.coordinator =
                    CoordinatorId{StrongId<CoordinatorIdTag>{payload_reader.u64()}};
                result.state.coordinator_epoch =
                    CoordinatorEpoch{StrongId<CoordinatorEpochTag>{payload_reader.u64()}};
                result.state.policy_generation =
                    ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{payload_reader.u64()}};
                result.state.topology_generation =
                    TopologyGeneration{StrongId<TopologyGenerationTag>{payload_reader.u64()}};
                result.state.sequence = payload_reader.u64();
                if (result.state.format_version != kPersistenceFormatVersion) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_UNSUPPORTED_VERSION,
                                        "embedded state version is not supported"};
                }
                break;
            }
            case PersistenceRecordType::POLICY: {
                if (result.state.policies.size() >= limits.max_policies) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                        "durable container carries too many policies"};
                }
                auto policy = read_policy(payload_reader, limits);
                if (!policy.has_value()) {
                    return policy.error();
                }
                if (!policy_ids.insert(policy.value().id.value()).second) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                        "durable container repeats a policy identity"};
                }
                result.state.policies.push_back(std::move(policy.value()));
                break;
            }
            case PersistenceRecordType::DEVICE_REGISTRATION: {
                if (result.state.devices.size() >= limits.max_devices) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                        "durable container carries too many devices"};
                }
                auto device = read_device(payload_reader, limits);
                if (!device.has_value()) {
                    return device.error();
                }
                if (!device_ids.insert(device.value().device.value()).second) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                        "durable container repeats a device identity"};
                }
                result.state.devices.push_back(std::move(device.value()));
                break;
            }
            case PersistenceRecordType::DOMAIN_DEFINITION: {
                if (result.state.domains.size() >= limits.max_domains) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                        "durable container carries too many domains"};
                }
                auto domain = read_domain(payload_reader, limits);
                if (!domain.has_value()) {
                    return domain.error();
                }
                if (!domain_ids.insert(domain.value().id.value()).second) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                        "durable container repeats a domain identity"};
                }
                result.state.domains.push_back(std::move(domain.value()));
                break;
            }
            case PersistenceRecordType::COUPLING: {
                if (result.state.couplings.size() >= limits.max_couplings) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                        "durable container carries too many couplings"};
                }
                auto coupling = read_coupling(payload_reader, limits);
                if (!coupling.has_value()) {
                    return coupling.error();
                }
                if (!coupling_ids.insert(coupling.value().id.value()).second) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                        "durable container repeats a coupling identity"};
                }
                result.state.couplings.push_back(std::move(coupling.value()));
                break;
            }
            case PersistenceRecordType::STATE_TRANSITION: {
                if (result.state.transitions.size() >= limits.max_transitions) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                        "durable container carries too many transitions"};
                }
                auto transition = read_transition(payload_reader);
                if (!transition.has_value()) {
                    return transition.error();
                }
                result.state.transitions.push_back(std::move(transition.value()));
                break;
            }
            case PersistenceRecordType::ACTION_RECORD: {
                if (result.state.actions.size() >= limits.max_actions) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                        "durable container carries too many actions"};
                }
                auto action = read_action(payload_reader, limits);
                if (!action.has_value()) {
                    return action.error();
                }
                if (!action_ids.insert(action.value().id.value()).second) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                        "durable container repeats an action identity"};
                }
                result.state.actions.push_back(std::move(action.value()));
                break;
            }
            case PersistenceRecordType::VERIFICATION_RECORD: {
                if (result.state.verifications.size() >= limits.max_verifications) {
                    return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                        "durable container carries too many verifications"};
                }
                auto verification = read_verification(payload_reader);
                if (!verification.has_value()) {
                    return verification.error();
                }
                result.state.verifications.push_back(std::move(verification.value()));
                break;
            }
            case PersistenceRecordType::OPERATOR_CONFIG: {
                (void)payload_reader.text(limits.max_string_bytes);
                break;
            }
        }

        if (!payload_reader.ok() || !payload_reader.exhausted()) {
            return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                "durable container record payload is malformed"};
        }
        ++result.records_read;
    }

    if (!reader.exhausted()) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_TRAILING_GARBAGE,
                            "durable container has unread bytes inside its declared body"};
    }
    result.bytes_read = bytes.size();
    return result;
}

Status DurableStore::atomic_replace(const std::string& temp_path, const std::string& target_path) {
#ifdef _WIN32
    const auto to_wide = [](const std::string& text) {
        if (text.empty()) {
            return std::wstring{};
        }
        const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                               static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(static_cast<std::size_t>(size), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                              out.data(), size);
        return out;
    };
    const std::wstring wide_temp = to_wide(temp_path);
    const std::wstring wide_target = to_wide(target_path);

    if (::ReplaceFileW(wide_target.c_str(), wide_temp.c_str(), nullptr,
                       REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != 0) {
        return Status::success();
    }
    const DWORD replace_error = ::GetLastError();
    if (replace_error != ERROR_FILE_NOT_FOUND && replace_error != ERROR_PATH_NOT_FOUND) {
        return Status::failure(ThermalErrorCode::PERSISTENCE_IO,
                               "ReplaceFileW failed with error " +
                                   std::to_string(static_cast<unsigned long>(replace_error)));
    }
    if (::MoveFileExW(wide_temp.c_str(), wide_target.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return Status::success();
    }
    return Status::failure(ThermalErrorCode::PERSISTENCE_IO,
                           "MoveFileExW failed with error " +
                               std::to_string(static_cast<unsigned long>(::GetLastError())));
#else
    if (::rename(temp_path.c_str(), target_path.c_str()) == 0) {
        return Status::success();
    }
    return Status::failure(ThermalErrorCode::PERSISTENCE_IO, "rename failed");
#endif
}

bool DurableStore::exists() const {
#ifdef _WIN32
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, path_.c_str(),
                                           static_cast<int>(path_.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, path_.c_str(), static_cast<int>(path_.size()), wide.data(),
                          size);
    const DWORD attributes = ::GetFileAttributesW(wide.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    return ::access(path_.c_str(), 0) == 0;
#endif
}

namespace {

/// A temporary sibling that is unique per save. Two concurrent durable writes
/// therefore never race on one scratch file, while the authoritative
/// replacement stays atomic: the last completed save wins and no reader can
/// observe a partially written container.
/// Serialises saves that target the same destination path.
///
/// Two concurrent writers must not race inside the replace step: the
/// scratch files are already unique per save, but the authoritative
/// replacement itself is a single-slot operation, so it is gated here
/// rather than left to filesystem error handling.
[[nodiscard]] std::mutex& save_gate_for(const std::string& path) {
    static std::mutex registry_mutex;
    static std::map<std::string, std::mutex> registry;
    std::lock_guard<std::mutex> lock(registry_mutex);
    // std::map guarantees reference stability, so the returned gate stays
    // valid for the lifetime of the process.
    return registry[path];
}

[[nodiscard]] std::string unique_temporary_path(const std::string& base) {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t index = ++counter;
    return base + ".tmp-" + std::to_string(index);
}

}  // namespace

Status DurableStore::save(const DurableState& state) {
    if (path_.empty()) {
        return Status::failure(ThermalErrorCode::INVALID_ARGUMENT, "durable path is empty");
    }
    auto encoded = encode_durable_state(state, limits_);
    if (!encoded.has_value()) {
        return Status(encoded.error());
    }
    const std::vector<std::uint8_t>& bytes = encoded.value();

    // One save at a time per destination path.
    std::mutex& gate = save_gate_for(path_);
    std::lock_guard<std::mutex> save_lock(gate);

#ifdef _WIN32
    const std::string temp_path = unique_temporary_path(path_);
    const auto to_wide = [](const std::string& text) {
        const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                               static_cast<int>(text.size()), nullptr, 0);
        std::wstring out(static_cast<std::size_t>(size), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                              out.data(), size);
        return out;
    };
    const std::wstring wide_temp = to_wide(temp_path);

    HANDLE file = ::CreateFileW(wide_temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Status::failure(ThermalErrorCode::PERSISTENCE_IO,
                               "cannot create the temporary durable file");
    }

    std::size_t written = 0;
    while (written < bytes.size()) {
        const std::size_t remaining = bytes.size() - written;
        const DWORD chunk = static_cast<DWORD>(remaining > 1U << 20 ? 1U << 20 : remaining);
        DWORD this_write = 0;
        if (::WriteFile(file, bytes.data() + written, chunk, &this_write, nullptr) == 0) {
            ::CloseHandle(file);
            ::DeleteFileW(wide_temp.c_str());
            return Status::failure(ThermalErrorCode::PERSISTENCE_IO, "durable write failed");
        }
        written += this_write;
    }
    // The durability boundary: flush to the device before replacing the
    // authoritative file, so an intended-durable mutation is never
    // acknowledged ahead of persistence.
    if (::FlushFileBuffers(file) == 0) {
        ::CloseHandle(file);
        ::DeleteFileW(wide_temp.c_str());
        return Status::failure(ThermalErrorCode::PERSISTENCE_IO, "durable flush failed");
    }
    ::CloseHandle(file);

    auto replaced = atomic_replace(temp_path, path_);
    if (!replaced.ok()) {
        ::DeleteFileW(wide_temp.c_str());
        return replaced;
    }
    return Status::success();
#else
    const std::string temp_path = unique_temporary_path(path_);
    std::FILE* file = std::fopen(temp_path.c_str(), "wb");
    if (file == nullptr) {
        return Status::failure(ThermalErrorCode::PERSISTENCE_IO, "cannot create temporary file");
    }
    const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    if (written != bytes.size() || std::fflush(file) != 0) {
        std::fclose(file);
        std::remove(temp_path.c_str());
        return Status::failure(ThermalErrorCode::PERSISTENCE_IO, "durable write failed");
    }
    std::fclose(file);
    auto replaced = atomic_replace(temp_path, path_);
    if (!replaced.ok()) {
        std::remove(temp_path.c_str());
        return replaced;
    }
    return Status::success();
#endif
}

Result<PersistenceLoadResult> DurableStore::load() const {
    if (path_.empty()) {
        return ThermalError{ThermalErrorCode::INVALID_ARGUMENT, "durable path is empty"};
    }
#ifdef _WIN32
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, path_.c_str(),
                                           static_cast<int>(path_.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, path_.c_str(), static_cast<int>(path_.size()), wide.data(),
                          size);
    HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_IO, "durable file is not readable"};
    }
    LARGE_INTEGER file_size{};
    if (::GetFileSizeEx(file, &file_size) == 0) {
        ::CloseHandle(file);
        return ThermalError{ThermalErrorCode::PERSISTENCE_IO, "durable file size query failed"};
    }
    if (file_size.QuadPart < 0 ||
        static_cast<std::uint64_t>(file_size.QuadPart) > limits_.max_file_bytes) {
        ::CloseHandle(file);
        return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                            "durable file exceeds the configured bound"};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size.QuadPart));
    std::size_t read = 0;
    while (read < bytes.size()) {
        DWORD this_read = 0;
        const DWORD chunk = static_cast<DWORD>(
            (bytes.size() - read) > 1U << 20 ? 1U << 20 : (bytes.size() - read));
        if (::ReadFile(file, bytes.data() + read, chunk, &this_read, nullptr) == 0) {
            ::CloseHandle(file);
            return ThermalError{ThermalErrorCode::PERSISTENCE_IO, "durable read failed"};
        }
        read += this_read;
    }
    ::CloseHandle(file);
    return decode_durable_state(bytes, limits_);
#else
    std::FILE* file = std::fopen(path_.c_str(), "rb");
    if (file == nullptr) {
        return ThermalError{ThermalErrorCode::PERSISTENCE_IO, "durable file is not readable"};
    }
    std::vector<std::uint8_t> bytes;
    std::uint8_t buffer[65536];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + got);
        if (bytes.size() > limits_.max_file_bytes) {
            std::fclose(file);
            return ThermalError{ThermalErrorCode::PERSISTENCE_CORRUPT,
                                "durable file exceeds the configured bound"};
        }
    }
    std::fclose(file);
    return decode_durable_state(bytes, limits_);
#endif
}

Status DurableStore::remove_all() {
    std::remove((path_ + ".tmp").c_str());
    for (std::uint64_t index = 1; index <= 64; ++index) {
        std::remove((path_ + ".tmp-" + std::to_string(index)).c_str());
    }
    if (std::remove(path_.c_str()) == 0) {
        return Status::success();
    }
    return exists() ? Status::failure(ThermalErrorCode::PERSISTENCE_IO, "cannot remove durable file")
                    : Status::success();
}

}  // namespace thermal_governor
