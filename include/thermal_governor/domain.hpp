// Thermal Governor — first-class thermal domains.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_DOMAIN_HPP
#define THERMAL_GOVERNOR_DOMAIN_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "thermal_governor/capability.hpp"
#include "thermal_governor/coupling.hpp"
#include "thermal_governor/error.hpp"
#include "thermal_governor/evidence.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/policy.hpp"
#include "thermal_governor/provenance.hpp"

namespace thermal_governor {

/// Scope of a thermal domain.
enum class ThermalDomainType : std::uint8_t {
    ACCELERATOR = 0,
    ACCELERATOR_PARTITION,
    NODE,
    CHASSIS,
    RACK,
    COOLING_ZONE,
    COUPLED_RESOURCE_GROUP,
};

[[nodiscard]] constexpr std::string_view to_string(ThermalDomainType t) noexcept {
    switch (t) {
        case ThermalDomainType::ACCELERATOR: return "ACCELERATOR";
        case ThermalDomainType::ACCELERATOR_PARTITION: return "ACCELERATOR_PARTITION";
        case ThermalDomainType::NODE: return "NODE";
        case ThermalDomainType::CHASSIS: return "CHASSIS";
        case ThermalDomainType::RACK: return "RACK";
        case ThermalDomainType::COOLING_ZONE: return "COOLING_ZONE";
        case ThermalDomainType::COUPLED_RESOURCE_GROUP: return "COUPLED_RESOURCE_GROUP";
    }
    return "UNRECOGNISED_DOMAIN_TYPE";
}

/// Deterministic rule used to derive a node or rack aggregate thermal state
/// from member evidence. The rule that produced a result is always exposed.
enum class AggregationRule : std::uint8_t {
    HOTTEST_MEMBER_GOVERNS = 0,
    CONFIGURED_WEIGHTED_RULE,
    NODE_SENSOR_GOVERNS,
    SYNTHETIC_AGGREGATION,
};

[[nodiscard]] constexpr std::string_view to_string(AggregationRule r) noexcept {
    switch (r) {
        case AggregationRule::HOTTEST_MEMBER_GOVERNS: return "HOTTEST_MEMBER_GOVERNS";
        case AggregationRule::CONFIGURED_WEIGHTED_RULE: return "CONFIGURED_WEIGHTED_RULE";
        case AggregationRule::NODE_SENSOR_GOVERNS: return "NODE_SENSOR_GOVERNS";
        case AggregationRule::SYNTHETIC_AGGREGATION: return "SYNTHETIC_AGGREGATION";
    }
    return "UNRECOGNISED_AGGREGATION_RULE";
}

/// A governed device registration.
///
/// Identity, generation, node/rack membership and the declared capability
/// set are stable topology and configuration, so they are durable. Telemetry
/// never is.
struct DeviceRegistration {
    DeviceId device{};
    DeviceGeneration generation{};
    NodeId node{};
    NodeGeneration node_generation{};
    RackId rack{};
    RackGeneration rack_generation{};
    Label label;
    CapabilitySet capabilities;
    Provenance provenance = Provenance::UNKNOWN;

    friend bool operator==(const DeviceRegistration&, const DeviceRegistration&) = default;
};

/// A weighted member of a thermal domain.
struct DomainMember {
    SubjectRef subject{};
    /// Weight used by CONFIGURED_WEIGHTED_RULE. Must be > 0 when used.
    double weight = 1.0;

    friend bool operator==(const DomainMember&, const DomainMember&) = default;
};

/// A stable thermal domain definition.
struct ThermalDomainDefinition {
    ThermalDomainId id{};
    ThermalDomainGeneration generation{};
    ThermalDomainType type = ThermalDomainType::ACCELERATOR;
    Label label;

    std::vector<DomainMember> members;
    std::optional<ThermalDomainId> parent;
    std::vector<ThermalDomainId> children;

    ThermalPolicyId policy{};
    ThermalPolicyGeneration policy_generation{};

    /// Explicit provenance. Registration rejects UNKNOWN: a domain must
    /// declare honestly whether it is real, modelled or unsupported.
    Provenance provenance = Provenance::UNKNOWN;
    std::string evidence_source;

    AggregationRule aggregation = AggregationRule::HOTTEST_MEMBER_GOVERNS;

    RackId rack{};
    RackGeneration rack_generation{};
    CoolingZoneId cooling_zone{};

    /// Upper bound in degrees used by SYNTHETIC_AGGREGATION to model a
    /// modelled (not measured) aggregate. Ignored for other rules.
    std::optional<double> synthetic_aggregate_offset{};
};

/// Result of an aggregate computation, exposing which rule produced it.
struct AggregateResult {
    DegreesCelsius temperature{};
    AggregationRule rule = AggregationRule::HOTTEST_MEMBER_GOVERNS;
    SubjectRef governing_member{};
    Provenance provenance = Provenance::UNKNOWN;
    std::size_t contributing_members = 0;
};

/// Registry of thermal domain definitions plus membership indexing.
///
/// Bounded: registration fails with RESOURCE_EXHAUSTED beyond the configured
/// limits rather than growing without bound.
class DomainRegistry {
public:
    struct Limits {
        std::size_t max_domains = 100000;
        std::size_t max_members_per_domain = 10000;
        std::size_t max_children_per_domain = 10000;
    };

    DomainRegistry() = default;
    explicit DomainRegistry(Limits limits) : limits_(limits) {}

    /// Register a new domain. Fails on duplicate id, invalid provenance,
    /// out-of-range limits, or unknown parent/child references.
    [[nodiscard]] Status add(const ThermalDomainDefinition& definition);

    /// Replace an existing domain, advancing its generation. The generation
    /// must move strictly forward; a stale replacement is rejected.
    [[nodiscard]] Status update(const ThermalDomainDefinition& definition);

    /// Remove a domain. Fails when other domains still reference it as a
    /// parent or child.
    [[nodiscard]] Status remove(ThermalDomainId id);

    [[nodiscard]] const ThermalDomainDefinition* find(ThermalDomainId id) const noexcept;
    [[nodiscard]] ThermalDomainDefinition* find_mutable(ThermalDomainId id) noexcept;

    /// Domains whose member set contains the given device id.
    [[nodiscard]] std::vector<ThermalDomainId> domains_for_device(DeviceId device) const;

    [[nodiscard]] std::vector<ThermalDomainId> domains_for_subject(
        const SubjectRef& subject) const;

    [[nodiscard]] std::vector<ThermalDomainId> all_domain_ids() const;
    [[nodiscard]] std::size_t size() const noexcept { return definitions_.size(); }
    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

    [[nodiscard]] TopologyGeneration topology_generation() const noexcept {
        return topology_generation_.current();
    }

    /// Advance the topology generation after any structural mutation.
    void bump_topology_generation() { topology_generation_.advance(); }

private:
    void rebuild_membership();

    Limits limits_{};
    std::unordered_map<std::uint64_t, ThermalDomainDefinition> definitions_;
    std::unordered_map<std::string, std::vector<ThermalDomainId>> membership_;
    GenerationCounter<TopologyGeneration> topology_generation_{
        StrongId<TopologyGenerationTag>{1}};
};

/// Compute the deterministic aggregate temperature of a domain from the
/// member evidence supplied by the caller.
///
/// A node or rack aggregate is never presented as a physical measurement
/// unless the rule is NODE_SENSOR_GOVERNS and the evidence is REAL.
[[nodiscard]] Result<AggregateResult> compute_aggregate(
    const ThermalDomainDefinition& definition,
    const std::vector<const ThermalEvidence*>& member_evidence);

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_DOMAIN_HPP
