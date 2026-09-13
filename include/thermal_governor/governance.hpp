// Thermal Governor — public governance API.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_GOVERNANCE_HPP
#define THERMAL_GOVERNOR_GOVERNANCE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "thermal_governor/action.hpp"
#include "thermal_governor/admission.hpp"
#include "thermal_governor/backend.hpp"
#include "thermal_governor/capability.hpp"
#include "thermal_governor/coupling.hpp"
#include "thermal_governor/decision.hpp"
#include "thermal_governor/domain.hpp"
#include "thermal_governor/envelope.hpp"
#include "thermal_governor/error.hpp"
#include "thermal_governor/evidence.hpp"
#include "thermal_governor/explanation.hpp"
#include "thermal_governor/headroom.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/mitigation.hpp"
#include "thermal_governor/persistence.hpp"
#include "thermal_governor/policy.hpp"
#include "thermal_governor/protocol.hpp"
#include "thermal_governor/recovery.hpp"
#include "thermal_governor/snapshot.hpp"
#include "thermal_governor/time.hpp"
#include "thermal_governor/verification.hpp"

namespace thermal_governor {

/// Receipt returned by an evidence publication.
struct EvidenceReceipt {
    EvidenceComparison comparison = EvidenceComparison::INDEPENDENT;
    TelemetryGeneration telemetry_generation{};
    /// True when the observation became the current evidence for its subject.
    bool accepted = false;
    /// Domains whose evaluated state was recomputed as a result.
    std::vector<ThermalDomainId> affected_domains;
};

/// Acknowledgement reported by a worker or enforcement backend.
struct ActionAck {
    ActionId action_id{};
    ActionGeneration action_generation{};
    CoordinatorEpoch coordinator_epoch{};
    WorkerBootId worker_boot{};
    DeviceGeneration device_generation{};
    ThermalDomainGeneration domain_generation{};
    /// True when the backend reported acceptance of the instruction.
    bool accepted = false;
    std::string detail;
};

/// Result reported by a worker after attempting the instructed mitigation.
struct ActionResult {
    ActionId action_id{};
    ActionGeneration action_generation{};
    CoordinatorEpoch coordinator_epoch{};
    WorkerBootId worker_boot{};
    DeviceGeneration device_generation{};
    ThermalDomainGeneration domain_generation{};
    ThermalPolicyGeneration policy_generation{};
    TelemetryGeneration telemetry_generation{};

    /// Backend-level completion. This is NOT proof of thermal effect.
    bool backend_succeeded = false;
    std::string detail;
};

/// Runtime configuration.
struct GovernorConfig {
    CoordinatorId coordinator{StrongId<CoordinatorIdTag>{1}};
    /// Starting epoch. When a durable file is loaded, the persisted epoch is
    /// advanced by one to reflect the restart.
    CoordinatorEpoch initial_epoch{StrongId<CoordinatorEpochTag>{1}};

    std::string durable_path;
    PersistenceLimits persistence_limits;
    FrameLimits frame_limits;

    std::size_t max_domains = 100000;
    std::size_t max_devices = 100000;
    std::size_t max_workers = 4096;
    std::size_t max_actions = 100000;
    std::size_t max_verifications = 100000;
    std::size_t max_history_records = 200000;
    std::size_t max_recovery_samples = 64;
    std::size_t max_coupled_edges = 200000;

    std::shared_ptr<const IClock> clock;
};

/// The Thermal Governor coordinator.
///
/// Owns thermal-domain identity, membership, evidence generations, headroom,
/// policy evaluation, derating, throttling constraints, admission authority,
/// hysteresis, recovery gating, coupling, mitigation intents and post-action
/// verification.
///
/// Concurrency model: all mutation is serialised under an internal mutex
/// that is never held across network waits, callbacks, backend waits or
/// persistence I/O. Readers obtain an immutable copy-on-write snapshot via
/// atomic shared-pointer publication and never block writers beyond the
/// pointer swap.
class ThermalGovernor {
public:
    [[nodiscard]] static Result<std::unique_ptr<ThermalGovernor>> create(GovernorConfig config);
    ~ThermalGovernor();

    ThermalGovernor(const ThermalGovernor&) = delete;
    ThermalGovernor& operator=(const ThermalGovernor&) = delete;

    // --- Coordinator authority -------------------------------------------
    [[nodiscard]] CoordinatorId coordinator_id() const noexcept;
    [[nodiscard]] CoordinatorEpoch epoch() const noexcept;
    [[nodiscard]] ThermalPolicyGeneration policy_generation() const;
    [[nodiscard]] TopologyGeneration topology_generation() const;

    // --- Registration -----------------------------------------------------
    [[nodiscard]] Status register_device(const DeviceRegistration& registration);
    [[nodiscard]] Status register_thermal_domain(const ThermalDomainDefinition& definition);
    [[nodiscard]] Status update_thermal_domain(const ThermalDomainDefinition& definition);
    [[nodiscard]] Status register_coupling(const CouplingRelation& relation);
    [[nodiscard]] Status remove_coupling(CouplingId id);
    [[nodiscard]] Status publish_capabilities(DeviceId device, DeviceGeneration generation,
                                              const CapabilitySet& capabilities);

    // --- Policy -----------------------------------------------------------
    [[nodiscard]] Result<ThermalPolicyGeneration> set_policy(const ThermalPolicy& policy);
    [[nodiscard]] Result<ThermalPolicy> get_policy(ThermalPolicyId id) const;

    // --- Workers ----------------------------------------------------------
    [[nodiscard]] Status register_worker(WorkerId worker, WorkerBootId boot, Label label);
    [[nodiscard]] Status worker_heartbeat(WorkerId worker, WorkerBootId boot);
    [[nodiscard]] Status note_worker_death(WorkerId worker, WorkerBootId boot,
                                           std::string reason);

    // --- Evidence ---------------------------------------------------------
    [[nodiscard]] Result<EvidenceReceipt> publish_temperature(const ThermalEvidence& evidence);
    [[nodiscard]] Result<EvidenceReceipt> publish_throttle_evidence(
        const ThermalEvidence& evidence);
    [[nodiscard]] Result<EvidenceReceipt> publish_cooling_evidence(const ThermalEvidence& evidence);
    [[nodiscard]] Result<EvidenceReceipt> publish_node_temperature(const ThermalEvidence& evidence);
    [[nodiscard]] Result<EvidenceReceipt> publish_rack_temperature(const ThermalEvidence& evidence);

    // --- Evaluation -------------------------------------------------------
    [[nodiscard]] Result<ThermalEvaluation> evaluate_domain(ThermalDomainId domain);
    [[nodiscard]] Result<ThermalEvaluation> evaluate_device(DeviceId device);
    [[nodiscard]] Result<ThermalAdmissionResult> evaluate_admission(
        const ThermalAdmissionRequest& request);
    [[nodiscard]] Result<HeadroomBreakdown> query_headroom(ThermalDomainId domain) const;
    [[nodiscard]] Result<ThermalEnvelope> query_envelope(ThermalDomainId domain) const;
    [[nodiscard]] Result<ThermalState> query_state(ThermalDomainId domain) const;
    [[nodiscard]] Result<RecoveryAssessment> evaluate_recovery(ThermalDomainId domain) const;

    // --- Recovery ---------------------------------------------------------
    [[nodiscard]] Status authorize_recovery(ThermalDomainId domain, std::string justification);
    [[nodiscard]] Status revoke_recovery(ThermalDomainId domain);

    // --- Actions ----------------------------------------------------------
    [[nodiscard]] Result<ActionId> authorize_derating(ThermalDomainId domain,
                                                      MitigationIntentRecord record,
                                                      std::string rationale);
    [[nodiscard]] Result<ActionId> authorize_mitigation(ThermalDomainId domain,
                                                        MitigationIntentRecord record,
                                                        std::string rationale);
    [[nodiscard]] Result<ThermalAction> get_action(ActionId id) const;
    [[nodiscard]] Result<ActionLifecycle> record_action_dispatch(ActionId id);
    [[nodiscard]] Result<ActionLifecycle> record_action_ack(const ActionAck& ack);
    [[nodiscard]] Result<ActionLifecycle> record_action_result(const ActionResult& result);
    [[nodiscard]] Result<ActionVerification> verify_action(ActionId id,
                                                           const ThermalEvidence& fresh);
    [[nodiscard]] Result<CancellationResult> cancel_action(ActionId id, std::string reason);
    [[nodiscard]] std::vector<ThermalAction> actions() const;
    [[nodiscard]] std::vector<ActionVerification> verifications() const;

    // --- History ----------------------------------------------------------
    [[nodiscard]] std::vector<StateTransitionRecord> history() const;
    [[nodiscard]] std::size_t history_size() const;

    // --- Reads ------------------------------------------------------------
    [[nodiscard]] std::shared_ptr<const GovernanceSnapshot> snapshot() const;
    [[nodiscard]] Result<Explanation> explain(ThermalDomainId domain) const;
    [[nodiscard]] Result<Explanation> explain_admission(
        const ThermalAdmissionRequest& request) const;

    // --- Durability -------------------------------------------------------
    [[nodiscard]] Status persist();
    [[nodiscard]] Status load_durable();
    [[nodiscard]] const std::string& durable_path() const noexcept;

    // --- Lifecycle --------------------------------------------------------
    [[nodiscard]] Status shutdown();
    [[nodiscard]] bool shutting_down() const noexcept;

    /// Deterministic clock injection. Rejected while the runtime is running
    /// unless no evidence has been published yet.
    [[nodiscard]] Status set_clock(std::shared_ptr<const IClock> clock);

private:
    ThermalGovernor();
    struct Impl;
    std::unique_ptr<Impl> impl_;

    [[nodiscard]] Result<EvidenceReceipt> publish_impl(const ThermalEvidence& evidence);
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_GOVERNANCE_HPP
