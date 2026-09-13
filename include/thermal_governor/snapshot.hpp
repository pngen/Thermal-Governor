// Thermal Governor — immutable read snapshots.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_SNAPSHOT_HPP
#define THERMAL_GOVERNOR_SNAPSHOT_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "thermal_governor/action.hpp"
#include "thermal_governor/capability.hpp"
#include "thermal_governor/coupling.hpp"
#include "thermal_governor/domain.hpp"
#include "thermal_governor/envelope.hpp"
#include "thermal_governor/evidence.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/policy.hpp"
#include "thermal_governor/recovery.hpp"
#include "thermal_governor/verification.hpp"

namespace thermal_governor {

/// Point-in-time view of one thermal domain.
struct ThermalDomainSnapshot {
    ThermalDomainDefinition definition;

    ThermalState state = ThermalState::UNKNOWN;
    DeratingLevel derating = DeratingLevel::UNKNOWN;
    ThermalDecision decision = ThermalDecision::UNKNOWN;

    std::optional<ThermalEvidence> evidence;
    FreshnessState freshness = FreshnessState::UNKNOWN;

    HeadroomBreakdown headroom;
    ThermalEnvelope envelope;

    ThermalPolicyGeneration policy_generation{};
    TelemetryGeneration telemetry_generation{};
    CapabilityGeneration capability_generation{};

    RecoveryAssessment recovery;
    RecoveryGeneration recovery_generation{};

    std::vector<CouplingRelation> couplings;

    /// Instantaneous Thermal Governor authority over this domain.
    DeratingLevel authority = DeratingLevel::UNKNOWN;
    Percent authority_concurrency{0.0};

    Provenance provenance = Provenance::UNKNOWN;
};

/// Point-in-time view of one governed device.
struct DeviceSnapshot {
    DeviceId device{};
    DeviceGeneration generation{};
    NodeId node{};
    RackId rack{};
    Label label;

    std::optional<ThermalEvidence> evidence;
    FreshnessState freshness = FreshnessState::UNKNOWN;
    CapabilitySet capabilities;

    std::vector<ThermalDomainId> domains;

    std::optional<DegreesCelsius> temperature;
    std::optional<ThrottleObservation> throttle;
    HeadroomBreakdown headroom;

    ThermalState state = ThermalState::UNKNOWN;
    DeratingLevel derating = DeratingLevel::UNKNOWN;
    ThermalDecision decision = ThermalDecision::UNKNOWN;

    Provenance provenance = Provenance::UNKNOWN;
};

/// Worker incarnation registered with the coordinator.
struct WorkerSnapshot {
    WorkerId worker{};
    WorkerBootId boot{};
    Label label;
    bool connected = false;
    SteadyTimePoint registered_at{};
    SteadyTimePoint last_seen_at{};
    std::size_t evidence_published = 0;
    std::vector<DeviceId> devices;
};

/// An immutable, self-consistent view of the whole runtime.
///
/// Snapshots are produced by copy-on-write publication; readers never
/// observe a partially mutated generation set.
struct GovernanceSnapshot {
    CoordinatorId coordinator{};
    CoordinatorEpoch coordinator_epoch{};

    ThermalPolicyGeneration policy_generation{};
    TopologyGeneration topology_generation{};
    CapabilityGeneration capability_generation{};

    SteadyTimePoint captured_at{};

    std::map<std::uint64_t, ThermalDomainSnapshot> domains;
    std::map<std::uint64_t, DeviceSnapshot> devices;
    std::map<std::uint64_t, WorkerSnapshot> workers;
    std::map<std::uint64_t, ThermalAction> actions;
    std::map<std::uint64_t, ThermalPolicy> policies;

    std::vector<CouplingRelation> couplings;
    std::vector<RecoveryGeneration> recovery_generations;

    std::size_t historical_transitions = 0;
    std::size_t historical_actions = 0;
    std::size_t historical_verifications = 0;

    [[nodiscard]] const ThermalDomainSnapshot* find_domain(ThermalDomainId id) const;
    [[nodiscard]] const DeviceSnapshot* find_device(DeviceId id) const;
    [[nodiscard]] const WorkerSnapshot* find_worker(WorkerId id) const;
    [[nodiscard]] const ThermalAction* find_action(ActionId id) const;
    [[nodiscard]] const ThermalPolicy* find_policy(ThermalPolicyId id) const;

    /// Stable summary used by the CLI status command and by tests.
    [[nodiscard]] std::string render_summary() const;
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_SNAPSHOT_HPP
