// Thermal Governor — the coordinator runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/governance.hpp"
#include "thermal_governor/version.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "format.hpp"
#include "thermal_governor/transport.hpp"

namespace thermal_governor {
namespace {

constexpr ThermalPolicyId kDefaultPolicyId{StrongId<ThermalPolicyIdTag>{1}};

[[nodiscard]] Status fail(ThermalErrorCode code, std::string detail) {
    return Status::failure(code, std::move(detail));
}

template <class T>
[[nodiscard]] Result<T> fail_result(ThermalErrorCode code, std::string detail) {
    return ThermalError{code, std::move(detail)};
}

/// A worker incarnation registered with the coordinator.
struct WorkerRecord {
    WorkerId worker{};
    WorkerBootId boot{};
    Label label;
    bool alive = false;
    SteadyTimePoint registered_at{};
    SteadyTimePoint last_seen_at{};
    std::size_t evidence_published = 0;
    std::vector<DeviceId> devices;
};

/// Synthesize the primary evidence for a domain from its members.
///
/// A single member contributes its own evidence unchanged, preserving REAL
/// provenance. Multiple members are aggregated by the domain rule, and the
/// synthesized record is never presented as a stronger provenance than the
/// rule can support.
struct DomainEvidence {
    ThermalEvidence evidence;
    bool synthesized = false;
    AggregationRule rule = AggregationRule::HOTTEST_MEMBER_GOVERNS;
};

[[nodiscard]] std::optional<DomainEvidence> gather_domain_evidence(
    const ThermalDomainDefinition& definition,
    const std::unordered_map<std::string, ThermalEvidence>& evidence) {
    // Direct domain-scoped evidence takes precedence.
    const SubjectRef self = SubjectRef::for_domain(definition.id, definition.generation);
    const auto direct = evidence.find(self.key());
    if (direct != evidence.end()) {
        DomainEvidence out;
        out.evidence = direct->second;
        return out;
    }

    std::vector<const ThermalEvidence*> members;
    members.reserve(definition.members.size());
    for (const auto& member : definition.members) {
        const auto it = evidence.find(member.subject.key());
        if (it != evidence.end()) {
            members.push_back(&it->second);
        }
    }
    if (members.empty()) {
        return std::nullopt;
    }
    if (members.size() == 1) {
        DomainEvidence out;
        out.evidence = *members.front();
        return out;
    }

    auto aggregate = compute_aggregate(definition, members);
    if (!aggregate.has_value()) {
        return std::nullopt;
    }

    DomainEvidence out;
    out.synthesized = true;
    out.rule = aggregate.value().rule;
    out.evidence = *members.front();
    out.evidence.temperature = aggregate.value().temperature;
    out.evidence.provenance = aggregate.value().provenance;
    // The aggregate keeps the governing member identity, so the record
    // still carries a real device generation and the policy requirement
    // that recovery witnesses share the current device generation remains
    // satisfiable.
    for (const auto* candidate : members) {
        if (candidate->subject == aggregate.value().governing_member) {
            out.evidence.subject = candidate->subject;
            break;
        }
    }
    out.evidence.domain = definition.id;
    out.evidence.domain_generation = definition.generation;
    if (out.evidence.provenance != Provenance::REAL) {
        out.evidence.source = MeasurementSource::SYNTHETIC_MODEL;
        for (const auto& candidate : members) {
            if (candidate->subject == aggregate.value().governing_member) {
                out.evidence.throttle = candidate->throttle;
                out.evidence.current_clock = candidate->current_clock;
                out.evidence.max_clock = candidate->max_clock;
                out.evidence.temperature_limit = candidate->temperature_limit;
                break;
            }
        }
    }
    return out;
}

}  // namespace

struct ThermalGovernor::Impl {
    GovernorConfig config;
    std::shared_ptr<const IClock> clock;

    /// Serialises every mutation. Never held across network waits, callbacks,
    /// backend waits or persistence I/O.
    mutable std::mutex state_mutex;
    /// Guards the published immutable snapshot pointer only.
    mutable std::mutex snapshot_mutex;

    CoordinatorEpoch epoch{StrongId<CoordinatorEpochTag>{1}};
    ThermalPolicyGeneration policy_generation{StrongId<ThermalPolicyGenerationTag>{1}};
    TopologyGeneration topology_generation{StrongId<TopologyGenerationTag>{1}};
    CapabilityGeneration capability_generation{StrongId<CapabilityGenerationTag>{1}};
    ActionGeneration action_generation{StrongId<ActionGenerationTag>{1}};
    VerificationGeneration verification_generation{StrongId<VerificationGenerationTag>{1}};
    RecoveryGeneration recovery_generation{StrongId<RecoveryGenerationTag>{1}};
    TelemetryGeneration telemetry_generation{StrongId<TelemetryGenerationTag>{1}};

    /// Advance a generation. Generations are monotonic by construction,
    /// so a stale observation can never move one backwards.
    template <class Id>
    static void advance(Id& generation) noexcept {
        generation = Id{StrongId<typename Id::tag_type>{generation.value() + 1}};
    }

    /// Adopt an observed generation only when it is strictly newer.
    template <class Id>
    static void observe(Id& generation, Id candidate) noexcept {
        if (candidate.value() > generation.value()) {
            generation = candidate;
        }
    }
    std::uint64_t sequence = 0;

    std::unordered_map<std::uint64_t, ThermalPolicy> policies;
    std::unordered_map<std::uint64_t, DeviceRegistration> devices;
    DomainRegistry domains;
    CouplingGraph couplings;

    std::vector<StateTransitionRecord> history;
    std::unordered_map<std::uint64_t, ThermalAction> actions;
    std::vector<ActionVerification> verifications;

    // --- Dynamic state: never persisted, never silently fresh -------------
    std::unordered_map<std::string, ThermalEvidence> evidence;
    std::unordered_set<std::string> invalidated_evidence;
    std::unordered_map<std::uint64_t, RecoveryHistory> recovery_histories;
    std::unordered_map<std::uint64_t, ThermalEvaluation> evaluations;
    std::unordered_set<std::uint64_t> explicit_recovery;
    std::unordered_map<std::uint64_t, WorkerRecord> workers;

    std::shared_ptr<const GovernanceSnapshot> published;
    std::atomic<bool> stopping{false};

    [[nodiscard]] const ThermalPolicy* policy_for(const ThermalDomainDefinition& definition) const {
        auto it = policies.find(definition.policy.value());
        if (it != policies.end()) {
            return &it->second;
        }
        it = policies.find(kDefaultPolicyId.value());
        if (it != policies.end()) {
            return &it->second;
        }
        return nullptr;
    }

    [[nodiscard]] RecoveryHistory& history_for(ThermalDomainId domain) {
        return recovery_histories[domain.value()];
    }

    void record_transition(const ThermalDomainId domain,
                           const ThermalDomainGeneration domain_generation,
                           const ThermalEvaluation& evaluation) {
        const auto it = evaluations.find(domain.value());
        const bool first_evaluation = it == evaluations.end();
        const ThermalState previous_state =
            first_evaluation ? ThermalState::UNKNOWN : it->second.state;
        const DeratingLevel previous_derating =
            first_evaluation ? DeratingLevel::UNKNOWN : it->second.derating;
        const bool changed = first_evaluation || it->second.state != evaluation.state ||
                             it->second.derating != evaluation.derating ||
                             it->second.decision != evaluation.decision;

        // The stored evaluation is refreshed on every recomputation so that
        // query_state, query_headroom, query_envelope and evaluate_recovery
        // always reflect the newest admissible evidence, even while
        // hysteresis retains the same state. Only the durable history record
        // is deduplicated.
        evaluations[domain.value()] = evaluation;
        if (!changed) {
            return;
        }

        StateTransitionRecord record;
        record.domain = domain;
        record.domain_generation = domain_generation;
        record.from = previous_state;
        record.to = evaluation.state;
        record.derating_from = previous_derating;
        record.derating_to = evaluation.derating;
        record.decision = evaluation.decision;
        record.temperature = evaluation.headroom.current_temperature;
        record.policy_generation = policy_generation;
        record.telemetry_generation = evaluation.telemetry_generation;
        record.coordinator_epoch = epoch;
        record.sequence = ++sequence;
        if (history.size() >= config.max_history_records) {
            history.erase(history.begin(),
                          history.begin() + static_cast<std::ptrdiff_t>(
                                                history.size() - config.max_history_records + 1));
        }
        history.push_back(record);
    }

    [[nodiscard]] std::shared_ptr<const GovernanceSnapshot> build_snapshot() const {
        auto snapshot = std::make_shared<GovernanceSnapshot>();
        snapshot->coordinator = config.coordinator;
        snapshot->coordinator_epoch = epoch;
        snapshot->policy_generation = policy_generation;
        snapshot->topology_generation = topology_generation;
        snapshot->capability_generation = capability_generation;
        snapshot->captured_at = clock->now();
        snapshot->historical_transitions = history.size();
        snapshot->historical_actions = actions.size();
        snapshot->historical_verifications = verifications.size();

        for (const auto& [key, policy] : policies) {
            (void)key;
            snapshot->policies.emplace(policy.id.value(), policy);
        }
        for (const auto domain_id : domains.all_domain_ids()) {
            const auto* definition = domains.find(domain_id);
            if (definition == nullptr) {
                continue;
            }
            ThermalDomainSnapshot domain_snapshot;
            domain_snapshot.definition = *definition;
            domain_snapshot.provenance = definition->provenance;

            const auto evaluation = evaluations.find(domain_id.value());
            if (evaluation != evaluations.end()) {
                domain_snapshot.state = evaluation->second.state;
                domain_snapshot.derating = evaluation->second.derating;
                domain_snapshot.decision = evaluation->second.decision;
                domain_snapshot.headroom = evaluation->second.headroom;
                domain_snapshot.recovery = evaluation->second.recovery;
                domain_snapshot.policy_generation = evaluation->second.policy_generation;
                domain_snapshot.telemetry_generation = evaluation->second.telemetry_generation;
                domain_snapshot.capability_generation = evaluation->second.capability_generation;
                domain_snapshot.authority = evaluation->second.derating;
                domain_snapshot.authority_concurrency =
                    evaluation->second.permitted_concurrency;
                domain_snapshot.envelope = make_envelope(*definition, evaluation->second);
            }
            const SubjectRef self =
                SubjectRef::for_domain(definition->id, definition->generation);
            const auto direct = evidence.find(self.key());
            if (direct != evidence.end()) {
                domain_snapshot.evidence = direct->second;
                domain_snapshot.freshness = direct->second.freshness;
            }
            for (const auto& edge : couplings.edges()) {
                if (edge.source == domain_id || edge.destination == domain_id) {
                    domain_snapshot.couplings.push_back(edge);
                }
            }
            snapshot->domains.emplace(domain_id.value(), std::move(domain_snapshot));
        }
        for (const auto& [key, registration] : devices) {
            (void)key;
            DeviceSnapshot device_snapshot;
            device_snapshot.device = registration.device;
            device_snapshot.generation = registration.generation;
            device_snapshot.node = registration.node;
            device_snapshot.rack = registration.rack;
            device_snapshot.label = registration.label;
            device_snapshot.capabilities = registration.capabilities;
            device_snapshot.provenance = registration.provenance;
            device_snapshot.domains = domains.domains_for_device(registration.device);

            const SubjectRef subject =
                SubjectRef::for_device(registration.device, registration.generation);
            const auto it = evidence.find(subject.key());
            if (it != evidence.end()) {
                device_snapshot.evidence = it->second;
                device_snapshot.freshness = it->second.freshness;
                device_snapshot.temperature = it->second.temperature;
                device_snapshot.throttle = it->second.throttle;
            }
            if (!device_snapshot.domains.empty()) {
                const auto evaluation = evaluations.find(device_snapshot.domains.front().value());
                if (evaluation != evaluations.end()) {
                    device_snapshot.state = evaluation->second.state;
                    device_snapshot.derating = evaluation->second.derating;
                    device_snapshot.decision = evaluation->second.decision;
                    device_snapshot.headroom = evaluation->second.headroom;
                }
            }
            snapshot->devices.emplace(registration.device.value(), std::move(device_snapshot));
        }
        for (const auto& [key, record] : workers) {
            (void)key;
            WorkerSnapshot worker_snapshot;
            worker_snapshot.worker = record.worker;
            worker_snapshot.boot = record.boot;
            worker_snapshot.label = record.label;
            worker_snapshot.connected = record.alive;
            worker_snapshot.registered_at = record.registered_at;
            worker_snapshot.last_seen_at = record.last_seen_at;
            worker_snapshot.evidence_published = record.evidence_published;
            worker_snapshot.devices = record.devices;
            snapshot->workers.emplace(record.worker.value(), std::move(worker_snapshot));
        }
        for (const auto& [key, action] : actions) {
            (void)key;
            snapshot->actions.emplace(action.id.value(), action);
        }
        snapshot->couplings = couplings.edges();
        return snapshot;
    }

    [[nodiscard]] ThermalEnvelope make_envelope(const ThermalDomainDefinition& definition,
                                                const ThermalEvaluation& evaluation) const {
        ThermalEnvelope envelope;
        envelope.domain = evaluation.domain;
        envelope.domain_generation = evaluation.domain_generation;
        envelope.subject_kind = definition.type == ThermalDomainType::ACCELERATOR
                                    ? SubjectKind::DEVICE
                                    : SubjectKind::THERMAL_DOMAIN;
        envelope.state = evaluation.state;
        envelope.derating = evaluation.derating;
        envelope.decision = evaluation.decision;
        envelope.maximum_legal_temperature = evaluation.headroom.governing_limit;
        envelope.vendor_temperature_limit = evaluation.headroom.vendor_limit;
        envelope.thresholds = policy_for(definition) != nullptr
                                  ? policy_for(definition)->thresholds
                                  : ThermalThresholds{};
        envelope.headroom = evaluation.headroom;
        envelope.permitted_concurrency = evaluation.permitted_concurrency;
        envelope.permitted_execution_class = evaluation.permitted_execution_class;
        envelope.permitted_clock_ceiling = evaluation.permitted_clock_ceiling;
        envelope.permitted_workload_intensity = evaluation.permitted_workload_intensity;
        envelope.prohibited_intents = evaluation.prohibited_intents;
        envelope.required_mitigation = evaluation.intents;
        envelope.reasons = evaluation.reasons;
        envelope.provenance = evaluation.provenance;
        envelope.throttle_class = evaluation.throttle_class;
        envelope.coordinator_epoch = evaluation.coordinator_epoch;
        envelope.policy_generation = evaluation.policy_generation;
        envelope.telemetry_generation = evaluation.telemetry_generation;
        envelope.capability_generation = evaluation.capability_generation;
        envelope.topology_generation = evaluation.topology_generation;
        const auto recovery = explicit_recovery.find(definition.id.value());
        envelope.recovery_generation =
            recovery != explicit_recovery.end() ? recovery_generation : RecoveryGeneration{};
        return envelope;
    }

    /// Recompute every domain whose evaluated state depends on the supplied
    /// subject key. Coupled neighbours are recomputed as well so that
    /// propagation is reflected immediately.
    /// Recompute every domain whose evaluated state depends on the supplied
    /// subject key, together with its coupled neighbours in both directions.
    void recompute_domains_for_subject(const std::string& subject_key) {
        std::set<std::uint64_t> pending;
        for (const auto domain_id : domains.all_domain_ids()) {
            const auto* definition = domains.find(domain_id);
            if (definition == nullptr) {
                continue;
            }
            const SubjectRef self =
                SubjectRef::for_domain(definition->id, definition->generation);
            if (self.key() == subject_key) {
                pending.insert(domain_id.value());
                continue;
            }
            for (const auto& member : definition->members) {
                if (member.subject.key() == subject_key) {
                    pending.insert(domain_id.value());
                    break;
                }
            }
        }
        // Coupled neighbours must also be refreshed because their evaluated
        // state embeds coupled pressure.
        std::set<std::uint64_t> with_neighbours = pending;
        for (const auto id : pending) {
            const ThermalDomainId origin{StrongId<ThermalDomainIdTag>{id}};
            for (const auto& [neighbour, depth] :
                 couplings.reachable_from(origin, kCouplingRecomputeDepth)) {
                (void)depth;
                with_neighbours.insert(neighbour.value());
            }
            for (const auto& [neighbour, depth] :
                 couplings.incoming_reachable(origin, kCouplingRecomputeDepth)) {
                (void)depth;
                with_neighbours.insert(neighbour.value());
            }
        }
        for (const auto id : with_neighbours) {
            recompute_domain(ThermalDomainId{StrongId<ThermalDomainIdTag>{id}});
        }
    }

    static constexpr std::uint32_t kCouplingRecomputeDepth = 3;

    void recompute_domain(ThermalDomainId domain_id) {
        const auto* definition = domains.find(domain_id);
        if (definition == nullptr) {
            return;
        }
        auto evaluation = evaluate_locked(*definition);
        if (evaluation.has_value()) {
            record_transition(domain_id, definition->generation, evaluation.value());
        }
    }

    void recompute_all_domains() {
        for (const auto domain_id : domains.all_domain_ids()) {
            recompute_domain(domain_id);
        }
    }

    [[nodiscard]] Result<ThermalEvaluation> evaluate_locked(
        const ThermalDomainDefinition& definition) {
        const ThermalPolicy* policy = policy_for(definition);
        if (policy == nullptr) {
            return fail_result<ThermalEvaluation>(ThermalErrorCode::UNKNOWN_POLICY,
                                                  "no policy is registered for this domain");
        }

        EvaluationInput input;
        input.domain = definition.id;
        input.domain_generation = definition.generation;
        input.domain_type = definition.type;
        input.policy = *policy;
        input.coordinator_epoch = epoch;
        input.policy_generation = policy->generation;
        input.topology_generation = topology_generation;

        const auto previous = evaluations.find(definition.id.value());
        if (previous != evaluations.end()) {
            input.previous_state = previous->second.state;
            input.previous_derating = previous->second.derating;
        }

        // Capability resolution from the members' declared capabilities.
        CapabilityState temperature_capability = CapabilityState::UNKNOWN;
        CapabilityState throttle_capability = CapabilityState::UNKNOWN;
        CapabilityState limit_capability = CapabilityState::UNKNOWN;
        bool first_capability = true;
        int device_member_count = 0;
        for (const auto& member : definition.members) {
            if (member.subject.kind != SubjectKind::DEVICE) {
                continue;
            }
            const auto it = devices.find(member.subject.device.value());
            if (it == devices.end()) {
                continue;
            }
            const auto& set = it->second.capabilities;
            const auto temperature = set.get(ThermalCapability::TEMPERATURE);
            const auto throttle = set.get(ThermalCapability::THROTTLE_REASONS);
            const auto limit = set.get(ThermalCapability::TEMPERATURE_LIMIT);
            ++device_member_count;
            if (first_capability) {
                temperature_capability = temperature;
                throttle_capability = throttle;
                limit_capability = limit;
                first_capability = false;
                continue;
            }
            // The weakest resolved capability governs the aggregate: a
            // single unsupported member cannot be hidden by a supported one.
            if (temperature == CapabilityState::UNSUPPORTED ||
                temperature_capability == CapabilityState::UNSUPPORTED) {
                temperature_capability = CapabilityState::UNSUPPORTED;
            } else if (temperature == CapabilityState::UNKNOWN ||
                       temperature_capability == CapabilityState::UNKNOWN) {
                temperature_capability = CapabilityState::UNKNOWN;
            }
            if (throttle == CapabilityState::UNSUPPORTED ||
                throttle_capability == CapabilityState::UNSUPPORTED) {
                throttle_capability = CapabilityState::UNSUPPORTED;
            } else if (throttle == CapabilityState::UNKNOWN ||
                       throttle_capability == CapabilityState::UNKNOWN) {
                throttle_capability = CapabilityState::UNKNOWN;
            }
            if (limit == CapabilityState::UNSUPPORTED ||
                limit_capability == CapabilityState::UNSUPPORTED) {
                limit_capability = CapabilityState::UNSUPPORTED;
            } else if (limit == CapabilityState::UNKNOWN ||
                       limit_capability == CapabilityState::UNKNOWN) {
                limit_capability = CapabilityState::UNKNOWN;
            }
        }
        // A domain with no usable DEVICE member resolves its capability from
        // the evidence it actually holds rather than failing outright.
        const bool no_device_capability_contribution = device_member_count == 0;
        auto gathered = gather_domain_evidence(definition, evidence);
        ThermalEvidence primary;
        if (gathered.has_value()) {
            primary = gathered->evidence;
            const auto invalidated = invalidated_evidence.find(primary.subject.key());
            if (invalidated != invalidated_evidence.end()) {
                input.freshness = FreshnessState::GENERATION_INVALIDATED;
            } else {
                const auto age = clock->now() - primary.measured_at;
                input.freshness = age > policy->freshness.max_age ? FreshnessState::STALE
                                                                  : FreshnessState::FRESH;
            }
            input.evidence = &primary;
        } else {
            input.freshness = FreshnessState::UNKNOWN;
            input.evidence = nullptr;
        }

        // A domain without DEVICE members (a node, rack or cooling-zone
        // domain, or a purely synthetic group) resolves its capability from
        // the evidence it actually holds. With no evidence at all the
        // capability stays UNKNOWN, which is honest and never ALLOW.
        if (no_device_capability_contribution) {
            if (gathered.has_value()) {
                switch (gathered->evidence.provenance) {
                    case Provenance::REAL:
                        temperature_capability = CapabilityState::SUPPORTED_REAL;
                        break;
                    case Provenance::SYNTHETIC:
                        temperature_capability = CapabilityState::SUPPORTED_SYNTHETIC;
                        break;
                    case Provenance::UNSUPPORTED:
                        temperature_capability = CapabilityState::UNSUPPORTED;
                        break;
                    case Provenance::UNKNOWN:
                        temperature_capability = CapabilityState::UNKNOWN;
                        break;
                }
                throttle_capability = gathered->evidence.throttle.has_value()
                                          ? temperature_capability
                                          : CapabilityState::UNSUPPORTED;
                limit_capability = gathered->evidence.temperature_limit.has_value()
                                       ? temperature_capability
                                       : CapabilityState::UNSUPPORTED;
            } else {
                temperature_capability = CapabilityState::UNKNOWN;
                throttle_capability = CapabilityState::UNKNOWN;
                limit_capability = CapabilityState::UNKNOWN;
            }
        }
        input.temperature_capability = temperature_capability;
        input.throttle_capability = throttle_capability;
        input.limit_capability = limit_capability;

        // Explicit conflict marker: a subject with recorded conflicting
        // duplicate evidence is never evaluated as healthy.
        if (conflicted_subjects.find(definition.id.value()) != conflicted_subjects.end()) {
            input.evidence_conflict = true;
        }

        // Recovery inputs.
        RecoveryInput recovery;
        recovery.history = &history_for(definition.id);
        recovery.current_epoch = epoch;
        if (input.evidence != nullptr) {
            recovery.current_boot = input.evidence->worker_boot;
            recovery.current_device_generation = input.evidence->subject.device_generation;
            recovery.current_domain_generation = input.evidence->domain_generation;
            recovery.evidence_provenance = input.evidence->provenance;
        }
        recovery.explicit_authorization =
            explicit_recovery.find(definition.id.value()) != explicit_recovery.end();
        recovery.coupled_domains_checked = !definition.members.empty();

        bool coupled_recovered = true;
        std::vector<CoupledPressure> pressure;
        for (const auto& [neighbour, depth] :
             couplings.incoming_reachable(definition.id, policy->coupling.max_propagation_depth)) {
            const auto* neighbour_definition = domains.find(neighbour);
            if (neighbour_definition == nullptr) {
                continue;
            }
            const auto neighbour_evaluation = evaluations.find(neighbour.value());
            if (neighbour_evaluation == evaluations.end()) {
                continue;
            }
            const auto& neighbour_state = neighbour_evaluation->second.state;
            if (neighbour_state == ThermalState::DERATED ||
                neighbour_state == ThermalState::THROTTLING ||
                neighbour_state == ThermalState::CRITICAL ||
                neighbour_state == ThermalState::UNKNOWN ||
                neighbour_state == ThermalState::UNSUPPORTED ||
                neighbour_state == ThermalState::REVALIDATION_REQUIRED) {
                coupled_recovered = false;
            }
            CoupledPressure entry;
            entry.domain = neighbour;
            entry.generation = neighbour_definition->generation;
            entry.state = neighbour_state;
            entry.temperature = neighbour_evaluation->second.headroom.current_temperature;
            entry.propagation = depth == 0 ? PropagationClass::NO_PROPAGATION
                                           : (depth == 1 ? PropagationClass::COUPLED_DOMAIN
                                                         : PropagationClass::DOMAIN_WIDE);
            entry.provenance = neighbour_definition->provenance;
            entry.depth = depth;
            entry.recovery_eligible = neighbour_evaluation->second.recovery.allowed;
            for (const auto& edge : couplings.edges()) {
                if (edge.source == neighbour && edge.destination == definition.id) {
                    entry.coupling = edge.type;
                    if (edge.provenance == Provenance::SYNTHETIC) {
                        entry.provenance = Provenance::SYNTHETIC;
                    }
                    break;
                }
            }
            pressure.push_back(entry);
        }
        recovery.coupled_domains_recovered = coupled_recovered;
        input.coupled = std::move(pressure);
        input.recovery = recovery;

        auto evaluation = evaluate_thermal(input);
        evaluation.capability_generation = capability_generation;
        evaluation.policy_generation = policy->generation;
        if (input.evidence != nullptr) {
            evaluation.telemetry_generation = input.evidence->telemetry_generation;
            if (gathered.has_value() && gathered->synthesized) {
                evaluation.provenance =
                    combine(input.evidence->provenance, Provenance::SYNTHETIC);
            }
        }
        return evaluation;
    }

    std::unordered_set<std::uint64_t> conflicted_subjects;
    std::unordered_map<std::uint64_t, ThermalEvaluation> action_baselines;
    std::string history_note;

    /// Publish a fresh immutable snapshot. Called with state_mutex held.
    void publish_locked();

    /// Retire every claim held by a worker incarnation. Called with
    /// state_mutex held.
    void retire_boot_locked(WorkerId worker, WorkerBootId boot);
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ThermalGovernor::ThermalGovernor() : impl_(std::make_unique<Impl>()) {}
ThermalGovernor::~ThermalGovernor() = default;

void ThermalGovernor::Impl::publish_locked() {
    auto snapshot = build_snapshot();
    std::lock_guard<std::mutex> lock(snapshot_mutex);
    published = std::move(snapshot);
}

Result<std::unique_ptr<ThermalGovernor>> ThermalGovernor::create(GovernorConfig config) {
    std::unique_ptr<ThermalGovernor> governor(new ThermalGovernor());
    Impl& impl = *governor->impl_;

    impl.config = std::move(config);
    impl.clock = impl.config.clock != nullptr ? impl.config.clock : SystemClock::shared();
    impl.epoch = impl.config.initial_epoch;
    if (!impl.epoch.is_valid()) {
        impl.epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{1}};
    }

    // The default policy is always present so that a freshly created runtime
    // can evaluate without an explicit policy installation.
    ThermalPolicy default_policy = ThermalPolicy::make_default();
    auto validation = validate_policy(default_policy);
    if (!validation.ok()) {
        return fail_result<std::unique_ptr<ThermalGovernor>>(ThermalErrorCode::POLICY_INVALID,
                                                             "default policy failed validation");
    }
    impl.policies.emplace(default_policy.id.value(), default_policy);
    impl.policy_generation = default_policy.generation;
    impl.capability_generation = CapabilityGeneration{StrongId<CapabilityGenerationTag>{1}};

    auto startup = SocketRuntime::ensure();
    if (!startup.ok()) {
        return fail_result<std::unique_ptr<ThermalGovernor>>(ThermalErrorCode::TRANSPORT_IO,
                                                             "socket runtime initialisation failed");
    }

    impl.published = impl.build_snapshot();
    return governor;
}

CoordinatorId ThermalGovernor::coordinator_id() const noexcept { return impl_->config.coordinator; }

CoordinatorEpoch ThermalGovernor::epoch() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->epoch;
}

ThermalPolicyGeneration ThermalGovernor::policy_generation() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->policy_generation;
}

TopologyGeneration ThermalGovernor::topology_generation() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->topology_generation;
}

const std::string& ThermalGovernor::durable_path() const noexcept {
    return impl_->config.durable_path;
}

bool ThermalGovernor::shutting_down() const noexcept { return impl_->stopping.load(); }

Status ThermalGovernor::set_clock(std::shared_ptr<const IClock> clock) {
    if (clock == nullptr) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT, "clock must not be null");
    }
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (!impl_->evidence.empty()) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT,
                    "the clock cannot be replaced once evidence has been published");
    }
    impl_->clock = std::move(clock);
    impl_->publish_locked();
    return Status::success();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

Status ThermalGovernor::register_device(const DeviceRegistration& registration) {
    if (!registration.device.is_valid() || !registration.generation.is_valid()) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT,
                    "device identity and generation must be non-zero");
    }
    if (registration.provenance == Provenance::UNKNOWN) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT,
                    "device provenance must be declared explicitly");
    }
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->devices.size() >= impl_->config.max_devices) {
        return fail(ThermalErrorCode::RESOURCE_EXHAUSTED, "device limit reached");
    }
    const auto existing = impl_->devices.find(registration.device.value());
    if (existing != impl_->devices.end() &&
        registration.generation.value() < existing->second.generation.value()) {
        return fail(ThermalErrorCode::STALE_DEVICE_GENERATION,
                    "device registration would move the device generation backwards");
    }
    impl_->devices[registration.device.value()] = registration;
    Impl::advance(impl_->capability_generation);
    impl_->recompute_all_domains();
    impl_->publish_locked();
    return Status::success();
}

Status ThermalGovernor::publish_capabilities(DeviceId device, DeviceGeneration generation,
                                             const CapabilitySet& capabilities) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto it = impl_->devices.find(device.value());
    if (it == impl_->devices.end()) {
        return fail(ThermalErrorCode::UNKNOWN_DEVICE, "device is not registered");
    }
    if (!(it->second.generation == generation)) {
        return fail(ThermalErrorCode::STALE_DEVICE_GENERATION,
                    "capability publication carries a stale device generation");
    }
    it->second.capabilities = capabilities;
    Impl::advance(impl_->capability_generation);
    impl_->recompute_all_domains();
    impl_->publish_locked();
    return Status::success();
}

Status ThermalGovernor::register_thermal_domain(const ThermalDomainDefinition& definition) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->domains.size() >= impl_->config.max_domains) {
        return fail(ThermalErrorCode::RESOURCE_EXHAUSTED, "thermal domain limit reached");
    }
    auto status = impl_->domains.add(definition);
    if (!status.ok()) {
        return status;
    }
    impl_->history_for(definition.id).set_capacity(impl_->config.max_recovery_samples);
    impl_->recompute_domain(definition.id);
    Impl::advance(impl_->topology_generation);
    impl_->publish_locked();
    return Status::success();
}

Status ThermalGovernor::update_thermal_domain(const ThermalDomainDefinition& definition) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    auto status = impl_->domains.update(definition);
    if (!status.ok()) {
        return status;
    }
    // A domain generation change invalidates every observation recorded
    // against the previous generation.
    for (const auto& [key, record] : impl_->evidence) {
        if (record.domain == definition.id &&
            record.domain_generation.value() < definition.generation.value()) {
            impl_->invalidated_evidence.insert(key);
        }
    }
    impl_->recompute_domain(definition.id);
    Impl::advance(impl_->topology_generation);
    impl_->publish_locked();
    return Status::success();
}

Status ThermalGovernor::register_coupling(const CouplingRelation& relation) {
    if (!relation.id.is_valid()) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT, "coupling id must be non-zero");
    }
    if (relation.provenance == Provenance::UNKNOWN) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT,
                    "coupling provenance must be declared explicitly");
    }
    if (!(relation.weight > 0.0) || relation.weight > 1.0) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT, "coupling weight must lie in (0, 1]");
    }
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->couplings.size() >= impl_->config.max_coupled_edges) {
        return fail(ThermalErrorCode::RESOURCE_EXHAUSTED, "coupling edge limit reached");
    }
    if (impl_->domains.find(relation.source) == nullptr) {
        return fail(ThermalErrorCode::UNKNOWN_DOMAIN, "coupling source domain is not registered");
    }
    if (impl_->domains.find(relation.destination) == nullptr) {
        return fail(ThermalErrorCode::UNKNOWN_DOMAIN,
                    "coupling destination domain is not registered");
    }
    impl_->couplings.add(relation);
    impl_->recompute_all_domains();
    Impl::advance(impl_->topology_generation);
    impl_->publish_locked();
    return Status::success();
}

Status ThermalGovernor::remove_coupling(CouplingId id) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (!impl_->couplings.remove(id)) {
        return fail(ThermalErrorCode::UNKNOWN_DOMAIN, "coupling is not registered");
    }
    impl_->recompute_all_domains();
    Impl::advance(impl_->topology_generation);
    impl_->publish_locked();
    return Status::success();
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------

Result<ThermalPolicyGeneration> ThermalGovernor::set_policy(const ThermalPolicy& policy) {
    auto validation = validate_policy(policy);
    if (!validation.ok()) {
        return fail_result<ThermalPolicyGeneration>(validation.code(), validation.error().detail);
    }
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto existing = impl_->policies.find(policy.id.value());
    if (existing != impl_->policies.end() &&
        policy.generation.value() <= existing->second.generation.value()) {
        return fail_result<ThermalPolicyGeneration>(
            ThermalErrorCode::STALE_POLICY,
            "policy update must advance the policy generation");
    }
    impl_->policies[policy.id.value()] = policy;
    Impl::advance(impl_->policy_generation);
    impl_->recompute_all_domains();
    impl_->publish_locked();
    return impl_->policy_generation;
}

Result<ThermalPolicy> ThermalGovernor::get_policy(ThermalPolicyId id) const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto it = impl_->policies.find(id.value());
    if (it == impl_->policies.end()) {
        return fail_result<ThermalPolicy>(ThermalErrorCode::UNKNOWN_POLICY,
                                          "policy is not registered");
    }
    return it->second;
}

// ---------------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------------

void ThermalGovernor::Impl::retire_boot_locked(WorkerId worker, WorkerBootId boot) {
    // 1. Revoke live authority.
    for (auto& [key, record] : workers) {
        (void)key;
        if (record.worker == worker && record.boot == boot) {
            record.alive = false;
        }
    }
    // 2. Reject delayed telemetry from the retired boot: every observation it
    //    produced becomes revalidation-required rather than current.
    for (const auto& [key, record] : evidence) {
        if (record.worker == worker && record.worker_boot == boot) {
            invalidated_evidence.insert(key);
        }
    }
    // 3. Preserve committed history, but drop recovery samples that can no
    //    longer witness a current incarnation.
    for (auto& [key, recovery_history] : recovery_histories) {
        (void)key;
        recovery_history.invalidate_for_boot(boot);
    }
    // 4. In-flight actions bound to the retired incarnation are classified
    //    honestly rather than optimistically.
    for (auto& [key, action] : actions) {
        (void)key;
        if (!(action.authority.worker == worker) || !(action.authority.worker_boot == boot)) {
            continue;
        }
        if (is_terminal(action.lifecycle)) {
            continue;
        }
        action.lifecycle = ActionLifecycle::OUTCOME_UNKNOWN;
        action.terminal_at = clock->now();
        action.detail = "worker incarnation retired before the outcome was proven";
        action.reasons.add(ThermalReasonCode::EVIDENCE_GENERATION_INVALIDATED);
    }
    action_baselines.clear();
}

Status ThermalGovernor::register_worker(WorkerId worker, WorkerBootId boot, Label label) {
    if (!worker.is_valid() || !boot.is_valid()) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT,
                    "worker identity and boot identity must be non-zero");
    }
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto existing = impl_->workers.find(worker.value());
    if (existing != impl_->workers.end()) {
        if (existing->second.boot.value() > boot.value()) {
            return fail(ThermalErrorCode::STALE_WORKER,
                        "worker boot identity would move backwards");
        }
        if (existing->second.boot == boot) {
            existing->second.alive = true;
            existing->second.label = std::move(label);
            impl_->publish_locked();
            return Status::success();
        }
        // Reincarnation: the previous boot loses every claim it held.
        impl_->retire_boot_locked(worker, existing->second.boot);
    }
    if (impl_->workers.size() >= impl_->config.max_workers) {
        return fail(ThermalErrorCode::RESOURCE_EXHAUSTED, "worker limit reached");
    }
    WorkerRecord record;
    record.worker = worker;
    record.boot = boot;
    record.label = std::move(label);
    record.alive = true;
    record.registered_at = impl_->clock->now();
    record.last_seen_at = record.registered_at;
    impl_->workers[worker.value()] = std::move(record);
    impl_->recompute_all_domains();
    impl_->publish_locked();
    return Status::success();
}

Status ThermalGovernor::worker_heartbeat(WorkerId worker, WorkerBootId boot) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto it = impl_->workers.find(worker.value());
    if (it == impl_->workers.end()) {
        return fail(ThermalErrorCode::UNKNOWN_DEVICE, "worker is not registered");
    }
    if (!(it->second.boot == boot)) {
        return fail(ThermalErrorCode::STALE_WORKER, "heartbeat carries a stale worker boot");
    }
    it->second.last_seen_at = impl_->clock->now();
    it->second.alive = true;
    return Status::success();
}

Status ThermalGovernor::note_worker_death(WorkerId worker, WorkerBootId boot, std::string reason) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto it = impl_->workers.find(worker.value());
    if (it == impl_->workers.end()) {
        return fail(ThermalErrorCode::UNKNOWN_DEVICE, "worker is not registered");
    }
    if (!(it->second.boot == boot)) {
        return fail(ThermalErrorCode::STALE_WORKER,
                    "worker death notice carries a stale worker boot");
    }
    it->second.alive = false;
    impl_->retire_boot_locked(worker, boot);
    impl_->recompute_all_domains();
    impl_->publish_locked();
    impl_->history_note = std::move(reason);
    return Status::success();
}

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

Result<EvidenceReceipt> ThermalGovernor::publish_impl(const ThermalEvidence& incoming) {
    auto structure = validate_evidence_structure(incoming);
    if (!structure.ok()) {
        return fail_result<EvidenceReceipt>(structure.code(), structure.error().detail);
    }

    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->stopping.load()) {
        return fail_result<EvidenceReceipt>(ThermalErrorCode::REVALIDATION_REQUIRED,
                                            "runtime is shutting down");
    }

    // Coordinator epoch fence.
    if (!(incoming.coordinator_epoch == impl_->epoch)) {
        return fail_result<EvidenceReceipt>(ThermalErrorCode::STALE_EPOCH,
                                            "evidence carries a stale coordinator epoch");
    }

    // Worker incarnation fence.
    if (incoming.worker.is_valid()) {
        const auto worker = impl_->workers.find(incoming.worker.value());
        if (worker == impl_->workers.end()) {
            return fail_result<EvidenceReceipt>(ThermalErrorCode::STALE_WORKER,
                                                "evidence names an unregistered worker");
        }
        if (!(worker->second.boot == incoming.worker_boot)) {
            return fail_result<EvidenceReceipt>(ThermalErrorCode::STALE_WORKER,
                                                "evidence carries a stale worker boot identity");
        }
        if (!worker->second.alive) {
            return fail_result<EvidenceReceipt>(ThermalErrorCode::STALE_WORKER,
                                                "evidence was produced by a retired worker");
        }
    }

    // Device generation fence.
    if (incoming.subject.kind == SubjectKind::DEVICE) {
        const auto device = impl_->devices.find(incoming.subject.device.value());
        if (device == impl_->devices.end()) {
            return fail_result<EvidenceReceipt>(ThermalErrorCode::UNKNOWN_DEVICE,
                                                "evidence names an unregistered device");
        }
        if (!(device->second.generation == incoming.subject.device_generation)) {
            return fail_result<EvidenceReceipt>(
                ThermalErrorCode::STALE_DEVICE_GENERATION,
                "evidence carries a stale device generation");
        }
    }

    // Thermal domain generation fence.
    if (incoming.domain.is_valid()) {
        const auto* definition = impl_->domains.find(incoming.domain);
        if (definition == nullptr) {
            return fail_result<EvidenceReceipt>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                                "evidence names an unregistered thermal domain");
        }
        if (!(definition->generation == incoming.domain_generation)) {
            return fail_result<EvidenceReceipt>(
                ThermalErrorCode::STALE_DOMAIN_GENERATION,
                "evidence carries a stale thermal domain generation");
        }
    }

    const std::string key = incoming.subject.key();
    const auto held = impl_->evidence.find(key);
    if (held != impl_->evidence.end()) {
        // Sequencing is scoped to a worker incarnation. The incarnation
        // itself has already been fenced above, so evidence from a NEW boot
        // is a fresh authority: a reincarnated worker legitimately restarts
        // its own sequence numbering and must not be rejected as stale
        // against the retired incarnation it replaced. Within one
        // incarnation the sequence must still be monotonic.
        const bool same_incarnation = held->second.worker == incoming.worker &&
                                      held->second.worker_boot == incoming.worker_boot;
        if (same_incarnation) {
            const EvidenceComparison comparison = compare_evidence(held->second, incoming);
        switch (comparison) {
            case EvidenceComparison::IDENTICAL_DUPLICATE: {
                EvidenceReceipt receipt;
                receipt.comparison = comparison;
                receipt.telemetry_generation = held->second.telemetry_generation;
                receipt.accepted = false;
                return receipt;
            }
            case EvidenceComparison::CONFLICTING_DUPLICATE:
                return fail_result<EvidenceReceipt>(
                    ThermalErrorCode::DUPLICATE_CONFLICT,
                    "evidence conflicts with the observation already held for this subject");
            case EvidenceComparison::SUPERSEDED:
                return fail_result<EvidenceReceipt>(
                    ThermalErrorCode::STALE_TELEMETRY,
                    "evidence sequence predates the observation already held");
            case EvidenceComparison::INDEPENDENT:
                break;
        }
            if (incoming.telemetry_generation.value() <
                held->second.telemetry_generation.value()) {
                return fail_result<EvidenceReceipt>(ThermalErrorCode::STALE_TELEMETRY,
                                                    "telemetry generation moved backwards");
            }
        }
    }

    impl_->conflicted_subjects.erase(incoming.subject.device.value());
    impl_->invalidated_evidence.erase(key);
    Impl::observe(impl_->telemetry_generation, incoming.telemetry_generation);
    impl_->evidence[key] = incoming;

    if (incoming.worker.is_valid()) {
        const auto worker = impl_->workers.find(incoming.worker.value());
        if (worker != impl_->workers.end()) {
            ++worker->second.evidence_published;
            worker->second.last_seen_at = impl_->clock->now();
        }
    }

    // Record a recovery sample for every domain the subject belongs to.
    std::vector<ThermalDomainId> affected = impl_->domains.domains_for_subject(incoming.subject);
    if (incoming.domain.is_valid()) {
        affected.push_back(incoming.domain);
    }
    std::sort(affected.begin(), affected.end());
    affected.erase(std::unique(affected.begin(), affected.end()), affected.end());

    for (const auto domain_id : affected) {
        const auto* definition = impl_->domains.find(domain_id);
        if (definition == nullptr) {
            continue;
        }
        RecoverySample sample;
        sample.evidence_id = incoming.evidence_id;
        sample.temperature = incoming.temperature;
        sample.at = incoming.measured_at;
        sample.worker = incoming.worker;
        sample.worker_boot = incoming.worker_boot;
        sample.coordinator_epoch = incoming.coordinator_epoch;
        sample.device_generation = incoming.subject.device_generation;
        sample.domain_generation = definition->generation;
        sample.telemetry_generation = incoming.telemetry_generation;
        sample.provenance = incoming.provenance;
        sample.admissible = incoming.integrity == IntegrityStatus::OK;
        if (incoming.throttle.has_value()) {
            sample.throttle_class = incoming.throttle->classification;
        }
        impl_->history_for(domain_id).push(sample);
        impl_->recompute_domain(domain_id);
    }

    // Coupled neighbours embed this domain state and must be refreshed in
    // both directions: a publication changes the pressure this domain
    // exerts on its destinations and the pressure it observes from its
    // sources.
    std::set<std::uint64_t> neighbours;
    for (const auto domain_id : affected) {
        for (const auto& [neighbour, depth] :
             impl_->couplings.reachable_from(domain_id, Impl::kCouplingRecomputeDepth)) {
            (void)depth;
            neighbours.insert(neighbour.value());
        }
        for (const auto& [neighbour, depth] :
             impl_->couplings.incoming_reachable(domain_id, Impl::kCouplingRecomputeDepth)) {
            (void)depth;
            neighbours.insert(neighbour.value());
        }
    }
    for (const auto id : neighbours) {
        impl_->recompute_domain(ThermalDomainId{StrongId<ThermalDomainIdTag>{id}});
    }

    impl_->publish_locked();

    EvidenceReceipt receipt;
    receipt.comparison = EvidenceComparison::INDEPENDENT;
    receipt.telemetry_generation = incoming.telemetry_generation;
    receipt.accepted = true;
    receipt.affected_domains = affected;
    return receipt;
}

Result<EvidenceReceipt> ThermalGovernor::publish_temperature(const ThermalEvidence& evidence) {
    return publish_impl(evidence);
}

Result<EvidenceReceipt> ThermalGovernor::publish_throttle_evidence(
    const ThermalEvidence& evidence) {
    if (!evidence.throttle.has_value()) {
        return fail_result<EvidenceReceipt>(ThermalErrorCode::INVALID_ARGUMENT,
                                            "throttle evidence requires a throttle observation");
    }
    return publish_impl(evidence);
}

Result<EvidenceReceipt> ThermalGovernor::publish_cooling_evidence(
    const ThermalEvidence& evidence) {
    if (!evidence.cooling.has_value() || !evidence.cooling->has_any()) {
        return fail_result<EvidenceReceipt>(
            ThermalErrorCode::CAPABILITY_UNSUPPORTED,
            "cooling evidence requires at least one cooling observation");
    }
    return publish_impl(evidence);
}

Result<EvidenceReceipt> ThermalGovernor::publish_node_temperature(
    const ThermalEvidence& evidence) {
    if (evidence.subject.kind != SubjectKind::NODE) {
        return fail_result<EvidenceReceipt>(ThermalErrorCode::INVALID_ARGUMENT,
                                            "node temperature requires a NODE subject");
    }
    return publish_impl(evidence);
}

Result<EvidenceReceipt> ThermalGovernor::publish_rack_temperature(
    const ThermalEvidence& evidence) {
    if (evidence.subject.kind != SubjectKind::RACK) {
        return fail_result<EvidenceReceipt>(ThermalErrorCode::INVALID_ARGUMENT,
                                            "rack temperature requires a RACK subject");
    }
    return publish_impl(evidence);
}


// ---------------------------------------------------------------------------
// Evaluation and queries
// ---------------------------------------------------------------------------

Result<ThermalEvaluation> ThermalGovernor::evaluate_domain(ThermalDomainId domain) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto* definition = impl_->domains.find(domain);
    if (definition == nullptr) {
        return fail_result<ThermalEvaluation>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                              "thermal domain is not registered");
    }
    auto evaluation = impl_->evaluate_locked(*definition);
    if (!evaluation.has_value()) {
        return evaluation.error();
    }
    impl_->record_transition(domain, definition->generation, evaluation.value());
    return evaluation;
}

Result<ThermalEvaluation> ThermalGovernor::evaluate_device(DeviceId device) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->devices.find(device.value()) == impl_->devices.end()) {
        return fail_result<ThermalEvaluation>(ThermalErrorCode::UNKNOWN_DEVICE,
                                              "device is not registered");
    }
    const auto domains = impl_->domains.domains_for_device(device);
    if (domains.empty()) {
        return fail_result<ThermalEvaluation>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                              "device is not a member of any thermal domain");
    }
    const auto* definition = impl_->domains.find(domains.front());
    if (definition == nullptr) {
        return fail_result<ThermalEvaluation>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                              "thermal domain is not registered");
    }
    auto evaluation = impl_->evaluate_locked(*definition);
    if (!evaluation.has_value()) {
        return evaluation.error();
    }
    impl_->record_transition(domains.front(), definition->generation, evaluation.value());
    return evaluation;
}

Result<ThermalAdmissionResult> ThermalGovernor::evaluate_admission(
    const ThermalAdmissionRequest& request) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);

    const auto* definition = impl_->domains.find(request.domain);
    if (definition == nullptr) {
        return fail_result<ThermalAdmissionResult>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                                   "thermal domain is not registered");
    }
    if (request.expected_domain_generation.is_valid() &&
        !(request.expected_domain_generation == definition->generation)) {
        return fail_result<ThermalAdmissionResult>(
            ThermalErrorCode::STALE_DOMAIN_GENERATION,
            "admission request carries a stale thermal domain generation");
    }
    if (request.expected_coordinator_epoch.has_value() &&
        !(*request.expected_coordinator_epoch == impl_->epoch)) {
        return fail_result<ThermalAdmissionResult>(
            ThermalErrorCode::STALE_EPOCH,
            "admission request carries a stale coordinator epoch");
    }

    const ThermalPolicy* policy = impl_->policy_for(*definition);
    if (policy == nullptr) {
        return fail_result<ThermalAdmissionResult>(ThermalErrorCode::UNKNOWN_POLICY,
                                                   "no policy is registered for this domain");
    }
    if (request.expected_policy_generation.has_value() &&
        !(*request.expected_policy_generation == policy->generation)) {
        return fail_result<ThermalAdmissionResult>(
            ThermalErrorCode::STALE_POLICY,
            "admission request carries a stale thermal policy generation");
    }

    EvaluationInput input;
    input.domain = definition->id;
    input.domain_generation = definition->generation;
    input.domain_type = definition->type;
    input.policy = *policy;
    input.coordinator_epoch = impl_->epoch;
    input.policy_generation = policy->generation;
    input.topology_generation = impl_->topology_generation;
    input.workload_profile = request.profile;
    const auto previous = impl_->evaluations.find(definition->id.value());
    if (previous != impl_->evaluations.end()) {
        input.previous_state = previous->second.state;
        input.previous_derating = previous->second.derating;
    }

    // Reuse the stored capability resolution by running the same evaluation
    // path the runtime uses for its published state.
    auto stored = impl_->evaluate_locked(*definition);
    if (!stored.has_value()) {
        return stored.error();
    }

    ThermalAdmissionResult result;
    result.domain = definition->id;
    result.domain_generation = definition->generation;
    result.workload = request.workload;
    result.profile = request.profile;
    result.state = stored.value().state;
    result.derating = stored.value().derating;
    result.governing_limit = stored.value().headroom.governing_limit;
    result.current_temperature = stored.value().headroom.current_temperature;
    result.effective_headroom = stored.value().headroom.effective_headroom;
    result.coordinator_epoch = impl_->epoch;
    result.policy_generation = policy->generation;
    result.telemetry_generation = stored.value().telemetry_generation;
    result.capability_generation = impl_->capability_generation;
    result.topology_generation = impl_->topology_generation;
    result.permitted_class = stored.value().permitted_execution_class;
    result.permitted_concurrency = stored.value().permitted_concurrency;
    result.permitted_clock_ceiling = stored.value().permitted_clock_ceiling;
    result.required_headroom =
        TemperatureDelta{policy->admission.min_effective_headroom.value() +
                         workload_headroom_demand(*policy, request.profile).value()};
    result.reasons = stored.value().reasons;
    result.constraints = stored.value().intents;

    // Workload-class eligibility is a hard predicate.
    ExecutionClass required_class = ExecutionClass::UNRESTRICTED;
    const bool profile_listed =
        workload_execution_class(*policy, request.profile, required_class);
    if (!profile_listed) {
        result.decision = AdmissionDecision::DENY;
        result.reasons.add(ThermalReasonCode::WORKLOAD_PROFILE_EXCLUDED);
        return result;
    }

    switch (stored.value().decision) {
        case ThermalDecision::UNKNOWN:
            result.decision = AdmissionDecision::UNKNOWN;
            return result;
        case ThermalDecision::UNSUPPORTED:
            result.decision = AdmissionDecision::UNSUPPORTED;
            return result;
        case ThermalDecision::REVALIDATION_REQUIRED:
            result.decision = AdmissionDecision::REVALIDATION_REQUIRED;
            return result;
        case ThermalDecision::DENY:
            result.decision = AdmissionDecision::DENY;
            return result;
        case ThermalDecision::DEFER:
            result.decision = policy->admission.allow_defer ? AdmissionDecision::DEFER
                                                            : AdmissionDecision::DENY;
            result.reasons.add(ThermalReasonCode::ADMISSION_DEFERRED_BY_POLICY);
            return result;
        case ThermalDecision::ALLOW:
        case ThermalDecision::ALLOW_DERATED:
            break;
    }

    bool derated = stored.value().decision == ThermalDecision::ALLOW_DERATED;

    if (!stored.value().headroom.meets_demand(result.required_headroom)) {
        if (policy->admission.allow_derated_admission) {
            derated = true;
            result.reasons.add(ThermalReasonCode::DERATED_ADMISSION_ALLOWED);
        } else {
            result.decision = AdmissionDecision::DENY;
            result.reasons.add(ThermalReasonCode::DERATED_ADMISSION_FORBIDDEN);
            return result;
        }
    } else {
        result.reasons.add(ThermalReasonCode::EFFECTIVE_HEADROOM_SUFFICIENT);
    }

    if (request.requested_concurrency.has_value() &&
        request.requested_concurrency->value() > result.permitted_concurrency.value()) {
        derated = true;
        result.reasons.add(ThermalReasonCode::CONCURRENCY_CEILING_APPLIED);
    }
    if (request.requested_class != ExecutionClass::UNRESTRICTED &&
        result.permitted_class == ExecutionClass::BEST_EFFORT_THERMAL &&
        request.requested_class != ExecutionClass::BEST_EFFORT_THERMAL) {
        derated = true;
        result.reasons.add(ThermalReasonCode::WORKLOAD_REQUIRES_DERATING);
    }
    if (required_class != ExecutionClass::UNRESTRICTED &&
        required_class != result.permitted_class) {
        // The policy-required execution class already exceeds what the
        // thermal envelope permits.
        if (result.permitted_class == ExecutionClass::BEST_EFFORT_THERMAL) {
            result.decision = policy->admission.allow_derated_admission
                                  ? AdmissionDecision::ADMIT_DERATED
                                  : AdmissionDecision::DENY;
            result.reasons.add(ThermalReasonCode::WORKLOAD_REQUIRES_DERATING);
            return result;
        }
        derated = true;
    }

    result.decision = derated ? AdmissionDecision::ADMIT_DERATED : AdmissionDecision::ADMIT;
    result.reasons.canonicalise();
    return result;
}

Result<HeadroomBreakdown> ThermalGovernor::query_headroom(ThermalDomainId domain) const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto it = impl_->evaluations.find(domain.value());
    if (it == impl_->evaluations.end()) {
        return fail_result<HeadroomBreakdown>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                              "thermal domain has not been evaluated");
    }
    return it->second.headroom;
}

Result<ThermalEnvelope> ThermalGovernor::query_envelope(ThermalDomainId domain) const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto* definition = impl_->domains.find(domain);
    if (definition == nullptr) {
        return fail_result<ThermalEnvelope>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                            "thermal domain is not registered");
    }
    const auto it = impl_->evaluations.find(domain.value());
    if (it == impl_->evaluations.end()) {
        return fail_result<ThermalEnvelope>(ThermalErrorCode::REVALIDATION_REQUIRED,
                                            "thermal domain has not been evaluated");
    }
    return impl_->make_envelope(*definition, it->second);
}

Result<ThermalState> ThermalGovernor::query_state(ThermalDomainId domain) const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->domains.find(domain) == nullptr) {
        return fail_result<ThermalState>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                         "thermal domain is not registered");
    }
    const auto it = impl_->evaluations.find(domain.value());
    if (it == impl_->evaluations.end()) {
        return fail_result<ThermalState>(ThermalErrorCode::REVALIDATION_REQUIRED,
                                         "thermal domain has not been evaluated");
    }
    return it->second.state;
}

Result<RecoveryAssessment> ThermalGovernor::evaluate_recovery(ThermalDomainId domain) const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto* definition = impl_->domains.find(domain);
    if (definition == nullptr) {
        return fail_result<RecoveryAssessment>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                               "thermal domain is not registered");
    }
    const ThermalPolicy* policy = impl_->policy_for(*definition);
    if (policy == nullptr) {
        return fail_result<RecoveryAssessment>(ThermalErrorCode::UNKNOWN_POLICY,
                                               "no policy is registered for this domain");
    }
    const auto evaluation = impl_->evaluations.find(domain.value());
    if (evaluation == impl_->evaluations.end()) {
        return fail_result<RecoveryAssessment>(ThermalErrorCode::REVALIDATION_REQUIRED,
                                               "thermal domain has not been evaluated");
    }
    return evaluation->second.recovery;
}

Status ThermalGovernor::authorize_recovery(ThermalDomainId domain, std::string justification) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto* definition = impl_->domains.find(domain);
    if (definition == nullptr) {
        return fail(ThermalErrorCode::UNKNOWN_DOMAIN, "thermal domain is not registered");
    }
    Impl::advance(impl_->recovery_generation);
    impl_->explicit_recovery.insert(domain.value());
    impl_->history_note = std::move(justification);
    impl_->recompute_domain(domain);
    impl_->publish_locked();
    return Status::success();
}

Status ThermalGovernor::revoke_recovery(ThermalDomainId domain) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto* definition = impl_->domains.find(domain);
    if (definition == nullptr) {
        return fail(ThermalErrorCode::UNKNOWN_DOMAIN, "thermal domain is not registered");
    }
    impl_->explicit_recovery.erase(domain.value());
    impl_->recompute_domain(domain);
    impl_->publish_locked();
    return Status::success();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

Result<ActionId> ThermalGovernor::authorize_mitigation(ThermalDomainId domain,
                                                       MitigationIntentRecord record,
                                                       std::string rationale) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto* definition = impl_->domains.find(domain);
    if (definition == nullptr) {
        return fail_result<ActionId>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                     "thermal domain is not registered");
    }
    if (record.intent == MitigationIntent::NO_ACTION) {
        return fail_result<ActionId>(ThermalErrorCode::ACTION_INFEASIBLE,
                                     "NO_ACTION is not an actionable mitigation");
    }
    const ThermalPolicy* policy = impl_->policy_for(*definition);
    if (policy == nullptr) {
        return fail_result<ActionId>(ThermalErrorCode::UNKNOWN_POLICY,
                                     "no policy is registered for this domain");
    }
    if (!policy_allows(*policy, record.intent)) {
        return fail_result<ActionId>(ThermalErrorCode::ACTION_INFEASIBLE,
                                     "policy does not permit this mitigation intent");
    }
    if (impl_->actions.size() >= impl_->config.max_actions) {
        return fail_result<ActionId>(ThermalErrorCode::RESOURCE_EXHAUSTED,
                                     "action queue limit reached");
    }

    auto evaluation = impl_->evaluate_locked(*definition);
    if (!evaluation.has_value()) {
        return fail_result<ActionId>(evaluation.error().code, evaluation.error().detail);
    }
    if (evaluation.value().state == ThermalState::CRITICAL &&
        is_restoring_intent(record.intent)) {
        return fail_result<ActionId>(ThermalErrorCode::ACTION_INFEASIBLE,
                                     "a critical thermal state forbids capability restoration");
    }
    if (evaluation.value().state == ThermalState::UNSUPPORTED &&
        is_restoring_intent(record.intent)) {
        return fail_result<ActionId>(ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                                     "unsupported thermal evidence cannot authorise restoration");
    }

    Impl::advance(impl_->action_generation);
    ThermalAction action;
    action.id = ActionId{StrongId<ActionIdTag>{impl_->action_generation.value()}};
    action.generation = impl_->action_generation;
    action.intent = record.intent;
    action.authority.coordinator_epoch = impl_->epoch;
    action.authority.action_id = action.id;
    action.authority.action_generation = action.generation;
    action.authority.domain = domain;
    action.authority.domain_generation = definition->generation;
    action.authority.policy_generation = policy->generation;
    action.authority.telemetry_generation = evaluation.value().telemetry_generation;
    action.authority.topology_generation = impl_->topology_generation;
    action.authority.capability_generation = impl_->capability_generation;
    // Bind the action to the worker incarnation and device generation that
    // actually produced the evidence the decision rests on. A stale plan then
    // fails revalidation instead of executing against superseded state.
    if (const auto gathered = gather_domain_evidence(*definition, impl_->evidence);
        gathered.has_value()) {
        action.authority.worker = gathered->evidence.worker;
        action.authority.worker_boot = gathered->evidence.worker_boot;
        action.authority.device = gathered->evidence.subject.device;
        action.authority.device_generation = gathered->evidence.subject.device_generation;
        action.authority.telemetry_generation = gathered->evidence.telemetry_generation;
    }
    if (evaluation.value().state == ThermalState::UNKNOWN ||
        evaluation.value().state == ThermalState::UNSUPPORTED ||
        evaluation.value().state == ThermalState::REVALIDATION_REQUIRED) {
        action.provenance = Provenance::UNKNOWN;
    } else {
        action.provenance = evaluation.value().provenance;
    }
    action.requested_clock_ceiling = record.clock_ceiling;
    action.requested_concurrency = record.concurrency_ceiling;
    action.requested_admission = record.admission_ceiling;
    action.lifecycle = ActionLifecycle::AUTHORIZED;
    action.authorized_at = impl_->clock->now();
    action.planned_at = action.authorized_at;
    action.authorization_source = "thermal-governor";
    action.detail = rationale.empty() ? record.rationale : std::move(rationale);
    action.reasons = evaluation.value().reasons;

    impl_->action_baselines[action.id.value()] = evaluation.value();
    impl_->actions[action.id.value()] = action;
    impl_->publish_locked();
    return action.id;
}

Result<ActionId> ThermalGovernor::authorize_derating(ThermalDomainId domain,
                                                     MitigationIntentRecord record,
                                                     std::string rationale) {
    auto evaluation = evaluate_domain(domain);
    if (!evaluation.has_value()) {
        return fail_result<ActionId>(evaluation.error().code, evaluation.error().detail);
    }
    if (record.intent == MitigationIntent::NO_ACTION) {
        // A derating authorisation defaults to the intents the evaluation
        // actually demands.
        for (const auto& demanded : evaluation.value().intents.records()) {
            if (demanded.intent == MitigationIntent::NO_ACTION) {
                continue;
            }
            record = demanded;
            break;
        }
        if (record.intent == MitigationIntent::NO_ACTION) {
            return fail_result<ActionId>(ThermalErrorCode::ACTION_INFEASIBLE,
                                         "the evaluation demands no derating action");
        }
    }
    return authorize_mitigation(domain, record, std::move(rationale));
}

Result<ThermalAction> ThermalGovernor::get_action(ActionId id) const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto it = impl_->actions.find(id.value());
    if (it == impl_->actions.end()) {
        return fail_result<ThermalAction>(ThermalErrorCode::UNKNOWN_ACTION,
                                          "action is not registered");
    }
    return it->second;
}

Result<ActionLifecycle> ThermalGovernor::record_action_dispatch(ActionId id) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto it = impl_->actions.find(id.value());
    if (it == impl_->actions.end()) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::UNKNOWN_ACTION,
                                            "action is not registered");
    }
    ThermalAction& action = it->second;
    if (is_terminal(action.lifecycle)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::ACTION_SUPERSEDED,
                                            "action has already reached a terminal state");
    }
    if (action.lifecycle != ActionLifecycle::AUTHORIZED) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::ACTION_INFEASIBLE,
                                            "action is not in the AUTHORIZED state");
    }

    // Revalidate immediately before dispatch. A stale plan must not execute.
    AuthoritySnapshot current;
    current.coordinator_epoch = impl_->epoch;
    current.action_generation = impl_->action_generation;
    current.policy_generation = impl_->policy_generation;
    const auto* definition = impl_->domains.find(action.authority.domain);
    current.domain_generation =
        definition != nullptr ? definition->generation : ThermalDomainGeneration{};
    current.topology_generation = impl_->topology_generation;
    current.capability_generation = impl_->capability_generation;
    const auto evaluation = impl_->evaluations.find(action.authority.domain.value());
    if (evaluation != impl_->evaluations.end()) {
        current.telemetry_generation = evaluation->second.telemetry_generation;
    }
    // The current worker incarnation and device generation are read from the
    // evidence the runtime actually holds, never from the plan itself.
    if (const auto gathered = gather_domain_evidence(*definition, impl_->evidence);
        gathered.has_value()) {
        current.worker_boot = gathered->evidence.worker_boot;
        current.device_generation = gathered->evidence.subject.device_generation;
        current.telemetry_generation = gathered->evidence.telemetry_generation;
    }

    auto revalidated = revalidate_action_authority(action.authority, current);
    if (!revalidated.ok()) {
        action.lifecycle = ActionLifecycle::SUPERSEDED;
        action.terminal_at = impl_->clock->now();
        action.detail = revalidated.error().render();
        impl_->publish_locked();
        return fail_result<ActionLifecycle>(revalidated.code(), revalidated.error().detail);
    }

    action.lifecycle = ActionLifecycle::DISPATCHED;
    action.dispatched_at = impl_->clock->now();
    impl_->publish_locked();
    return action.lifecycle;
}

Result<ActionLifecycle> ThermalGovernor::record_action_ack(const ActionAck& ack) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto it = impl_->actions.find(ack.action_id.value());
    if (it == impl_->actions.end()) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::UNKNOWN_ACTION,
                                            "action is not registered");
    }
    ThermalAction& action = it->second;

    if (!(ack.action_generation == action.generation)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::STALE_ACTION,
                                            "acknowledgement carries a stale action generation");
    }
    if (!(ack.coordinator_epoch == impl_->epoch)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::STALE_EPOCH,
                                            "acknowledgement carries a stale coordinator epoch");
    }
    if (action.authority.worker.is_valid() && !(ack.worker_boot == action.authority.worker_boot)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::STALE_WORKER,
                                            "acknowledgement carries a stale worker boot");
    }
    if (action.authority.device.is_valid() &&
        !(ack.device_generation == action.authority.device_generation)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::STALE_DEVICE_GENERATION,
                                            "acknowledgement carries a stale device generation");
    }
    if (!(ack.domain_generation == action.authority.domain_generation)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::STALE_DOMAIN_GENERATION,
                                            "acknowledgement carries a stale domain generation");
    }
    if (is_terminal(action.lifecycle)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::ACTION_SUPERSEDED,
                                            "action has already reached a terminal state");
    }
    if (action.lifecycle == ActionLifecycle::ACKNOWLEDGED ||
        action.lifecycle == ActionLifecycle::VERIFYING) {
        // An identical repeat acknowledgement is idempotent; a divergent one
        // is an explicit conflict.
        if (ack.accepted == (action.detail.find("rejected") == std::string::npos)) {
            return action.lifecycle;
        }
        return fail_result<ActionLifecycle>(
            ThermalErrorCode::DUPLICATE_CONFLICT,
            "a conflicting acknowledgement was already recorded for this action");
    }
    if (action.lifecycle != ActionLifecycle::DISPATCHED) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::ACTION_INFEASIBLE,
                                            "action is not in the DISPATCHED state");
    }

    action.acknowledged_at = impl_->clock->now();
    if (!ack.accepted) {
        action.lifecycle = ActionLifecycle::FAILED;
        action.terminal_at = action.acknowledged_at;
        action.detail = ack.detail.empty() ? "backend rejected the mitigation instruction"
                                           : ack.detail;
        action.reasons.add(ThermalReasonCode::NONE);
        impl_->publish_locked();
        return action.lifecycle;
    }
    // ACKNOWLEDGED is not EFFECTIVE. Verification is still mandatory.
    action.lifecycle = ActionLifecycle::ACKNOWLEDGED;
    action.detail = ack.detail;
    impl_->publish_locked();
    return action.lifecycle;
}

Result<ActionLifecycle> ThermalGovernor::record_action_result(const ActionResult& result) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto it = impl_->actions.find(result.action_id.value());
    if (it == impl_->actions.end()) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::UNKNOWN_ACTION,
                                            "action is not registered");
    }
    ThermalAction& action = it->second;

    if (!(result.action_generation == action.generation)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::STALE_ACTION,
                                            "result carries a stale action generation");
    }
    if (!(result.coordinator_epoch == impl_->epoch)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::STALE_EPOCH,
                                            "result carries a stale coordinator epoch");
    }
    if (action.authority.worker.is_valid() &&
        !(result.worker_boot == action.authority.worker_boot)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::STALE_WORKER,
                                            "result carries a stale worker boot identity");
    }
    if (action.authority.device.is_valid() &&
        !(result.device_generation == action.authority.device_generation)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::STALE_DEVICE_GENERATION,
                                            "result carries a stale device generation");
    }
    if (!(result.domain_generation == action.authority.domain_generation)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::STALE_DOMAIN_GENERATION,
                                            "result carries a stale domain generation");
    }
    // A result that echoes a superseded generation is stale, and a result
    // that arrives after the governing policy itself advanced can no longer
    // commit against the decision it was authorised under.
    bool policy_superseded = !(result.policy_generation == action.authority.policy_generation);
    if (!policy_superseded) {
        const auto* current_definition = impl_->domains.find(action.authority.domain);
        const ThermalPolicy* current_policy =
            current_definition != nullptr ? impl_->policy_for(*current_definition) : nullptr;
        policy_superseded = current_policy != nullptr &&
                            !(current_policy->generation == action.authority.policy_generation);
    }
    if (policy_superseded) {
        action.lifecycle = ActionLifecycle::SUPERSEDED;
        action.terminal_at = impl_->clock->now();
        action.detail = "policy generation advanced before the action result arrived";
        impl_->publish_locked();
        return fail_result<ActionLifecycle>(
            ThermalErrorCode::STALE_POLICY,
            "action result arrived after the thermal policy generation advanced");
    }
    if (is_terminal(action.lifecycle)) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::ACTION_SUPERSEDED,
                                            "action has already reached a terminal state");
    }
    if (action.lifecycle == ActionLifecycle::VERIFYING) {
        return action.lifecycle;
    }
    if (action.lifecycle != ActionLifecycle::ACKNOWLEDGED &&
        action.lifecycle != ActionLifecycle::DISPATCHED) {
        return fail_result<ActionLifecycle>(ThermalErrorCode::ACTION_INFEASIBLE,
                                            "action is not awaiting a result");
    }

    action.detail = result.detail;
    if (!result.backend_succeeded) {
        action.lifecycle = ActionLifecycle::FAILED;
        action.terminal_at = impl_->clock->now();
        impl_->publish_locked();
        return action.lifecycle;
    }
    // A backend success is a claim, not proof. Verification decides effect.
    action.lifecycle = ActionLifecycle::VERIFYING;
    impl_->publish_locked();
    return action.lifecycle;
}

Result<ActionVerification> ThermalGovernor::verify_action(ActionId id,
                                                          const ThermalEvidence& fresh) {
    // Publish first so that the verification consumes current evidence under
    // the ordinary authority path.
    auto published = publish_temperature(fresh);
    if (!published.has_value()) {
        return fail_result<ActionVerification>(published.error().code,
                                               published.error().detail);
    }

    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto it = impl_->actions.find(id.value());
    if (it == impl_->actions.end()) {
        return fail_result<ActionVerification>(ThermalErrorCode::UNKNOWN_ACTION,
                                               "action is not registered");
    }
    ThermalAction& action = it->second;
    if (is_terminal(action.lifecycle)) {
        return fail_result<ActionVerification>(
            ThermalErrorCode::ACTION_SUPERSEDED,
            "action already reached a terminal state and its verification cannot commit");
    }
    if (action.lifecycle != ActionLifecycle::ACKNOWLEDGED &&
        action.lifecycle != ActionLifecycle::VERIFYING &&
        action.lifecycle != ActionLifecycle::DISPATCHED) {
        return fail_result<ActionVerification>(ThermalErrorCode::ACTION_INFEASIBLE,
                                               "action is not awaiting verification");
    }
    if (impl_->verifications.size() >= impl_->config.max_verifications) {
        return fail_result<ActionVerification>(ThermalErrorCode::RESOURCE_EXHAUSTED,
                                               "verification record limit reached");
    }

    const auto* definition = impl_->domains.find(action.authority.domain);
    if (definition == nullptr) {
        return fail_result<ActionVerification>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                               "action thermal domain is no longer registered");
    }

    const auto baseline = impl_->action_baselines.find(id.value());
    if (baseline == impl_->action_baselines.end()) {
        return fail_result<ActionVerification>(
            ThermalErrorCode::ACTION_INFEASIBLE,
            "no planning baseline is retained for this action");
    }

    auto after = impl_->evaluate_locked(*definition);
    if (!after.has_value()) {
        return fail_result<ActionVerification>(after.error().code, after.error().detail);
    }

    VerificationInput input;
    input.action = &action;
    input.before = baseline->second;
    input.after = after.value();
    input.current.coordinator_epoch = impl_->epoch;
    input.current.domain_generation = definition->generation;
    input.current.device_generation = action.authority.device_generation;
    input.current.worker_boot = action.authority.worker_boot;
    input.current.policy_generation = impl_->policy_generation;
    input.current.topology_generation = impl_->topology_generation;
    input.current.capability_generation = impl_->capability_generation;
    input.current.action_generation = impl_->action_generation;
    input.current.telemetry_generation = after.value().telemetry_generation;
    input.verification_policy_generation = impl_->policy_generation;
    const std::string domain_key =
        SubjectRef::for_domain(definition->id, definition->generation).key();
    const bool domain_invalidated =
        impl_->invalidated_evidence.find(domain_key) != impl_->invalidated_evidence.end();
    input.freshness =
        domain_invalidated ? FreshnessState::GENERATION_INVALIDATED : FreshnessState::FRESH;
    input.now = impl_->clock->now();
    // Fresh evidence means an observation taken after the mitigation was
    // dispatched. A reading that predates the dispatch cannot prove effect,
    // and an ACKNOWLEDGED instruction is not an EFFECTIVE one.
    input.fresh_evidence = input.freshness == FreshnessState::FRESH &&
                           fresh.measured_at > action.dispatched_at;
    input.secondary_hard_violation =
        after.value().state == ThermalState::CRITICAL &&
        baseline->second.state != ThermalState::CRITICAL;

    auto verification = thermal_governor::verify_action(input);
    if (!verification.has_value()) {
        return verification.error();
    }

    Impl::advance(impl_->verification_generation);
    ActionVerification record = verification.value();
    record.generation = impl_->verification_generation;
    action.lifecycle = record.resulting_lifecycle;
    action.verification_generation = record.generation;
    action.verification_telemetry_generation = record.telemetry_generation;
    if (is_terminal(action.lifecycle)) {
        action.terminal_at = impl_->clock->now();
    }
    impl_->verifications.push_back(record);
    impl_->record_transition(definition->id, definition->generation, after.value());
    impl_->publish_locked();
    return record;
}

Result<CancellationResult> ThermalGovernor::cancel_action(ActionId id, std::string reason) {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto it = impl_->actions.find(id.value());
    if (it == impl_->actions.end()) {
        return fail_result<CancellationResult>(ThermalErrorCode::UNKNOWN_ACTION,
                                               "action is not registered");
    }
    ThermalAction& action = it->second;
    action.detail = std::move(reason);

    switch (action.lifecycle) {
        case ActionLifecycle::PLANNED:
        case ActionLifecycle::AUTHORIZED:
            // Never dispatched: nothing was executed, so nothing may later
            // report success.
            action.lifecycle = ActionLifecycle::CANCELLED;
            action.terminal_at = impl_->clock->now();
            impl_->publish_locked();
            return CancellationResult::CANCELLED_BEFORE_DISPATCH;
        case ActionLifecycle::DISPATCHED:
        case ActionLifecycle::ACKNOWLEDGED:
        case ActionLifecycle::VERIFYING:
            // The instruction may already have touched hardware state. The
            // honest answer is that verification is required.
            action.lifecycle = ActionLifecycle::VERIFYING;
            impl_->publish_locked();
            return CancellationResult::VERIFICATION_REQUIRED;
        case ActionLifecycle::EFFECTIVE:
        case ActionLifecycle::PARTIALLY_EFFECTIVE:
            return CancellationResult::ALREADY_EFFECTIVE;
        case ActionLifecycle::INEFFECTIVE:
        case ActionLifecycle::WORSENED:
        case ActionLifecycle::FAILED:
        case ActionLifecycle::SUPERSEDED:
        case ActionLifecycle::CANCELLED:
        case ActionLifecycle::OUTCOME_UNKNOWN:
            return CancellationResult::OUTCOME_UNKNOWN;
    }
    return CancellationResult::OUTCOME_UNKNOWN;
}

std::vector<ThermalAction> ThermalGovernor::actions() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    std::vector<ThermalAction> out;
    out.reserve(impl_->actions.size());
    for (const auto& [key, action] : impl_->actions) {
        (void)key;
        out.push_back(action);
    }
    std::sort(out.begin(), out.end(),
              [](const ThermalAction& a, const ThermalAction& b) { return a.id < b.id; });
    return out;
}

std::vector<ActionVerification> ThermalGovernor::verifications() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->verifications;
}

std::vector<StateTransitionRecord> ThermalGovernor::history() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->history;
}

std::size_t ThermalGovernor::history_size() const {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    return impl_->history.size();
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------

std::shared_ptr<const GovernanceSnapshot> ThermalGovernor::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->snapshot_mutex);
    return impl_->published;
}

Result<Explanation> ThermalGovernor::explain(ThermalDomainId domain) const {
    // Evaluation is a mutation-free recomputation, but it does publish a
    // transition when the state changed, so it takes the ordinary lock.
    auto evaluation = const_cast<ThermalGovernor*>(this)->evaluate_domain(domain);
    if (!evaluation.has_value()) {
        return fail_result<Explanation>(evaluation.error().code, evaluation.error().detail);
    }
    auto envelope = query_envelope(domain);
    if (!envelope.has_value()) {
        return fail_result<Explanation>(envelope.error().code, envelope.error().detail);
    }
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto* definition = impl_->domains.find(domain);
    if (definition == nullptr) {
        return fail_result<Explanation>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                        "thermal domain is not registered");
    }
    ThermalPolicy policy = *impl_->policy_for(*definition);
    Explanation explanation = build_explanation(evaluation.value(), envelope.value(), policy);
    return explanation;
}

Result<Explanation> ThermalGovernor::explain_admission(
    const ThermalAdmissionRequest& request) const {
    auto result = const_cast<ThermalGovernor*>(this)->evaluate_admission(request);
    if (!result.has_value()) {
        return fail_result<Explanation>(result.error().code, result.error().detail);
    }
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const auto* definition = impl_->domains.find(request.domain);
    if (definition == nullptr) {
        return fail_result<Explanation>(ThermalErrorCode::UNKNOWN_DOMAIN,
                                        "thermal domain is not registered");
    }
    const ThermalPolicy* policy = impl_->policy_for(*definition);
    return build_admission_explanation(result.value(), policy != nullptr ? *policy
                                                                        : ThermalPolicy{});
}

// ---------------------------------------------------------------------------
// Durability
// ---------------------------------------------------------------------------

Status ThermalGovernor::persist() {
    DurableState durable;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        path = impl_->config.durable_path;
        if (path.empty()) {
            return fail(ThermalErrorCode::INVALID_ARGUMENT,
                        "no durable path is configured for this runtime");
        }
        durable.format_version = kPersistenceFormatVersion;
        durable.coordinator = impl_->config.coordinator;
        durable.coordinator_epoch = impl_->epoch;
        durable.policy_generation = impl_->policy_generation;
        durable.topology_generation = impl_->topology_generation;
        durable.sequence = impl_->sequence;
        for (const auto& [key, policy] : impl_->policies) {
            (void)key;
            durable.policies.push_back(policy);
        }
        std::sort(durable.policies.begin(), durable.policies.end(),
                  [](const ThermalPolicy& a, const ThermalPolicy& b) { return a.id < b.id; });
        for (const auto& [key, registration] : impl_->devices) {
            (void)key;
            durable.devices.push_back(registration);
        }
        std::sort(durable.devices.begin(), durable.devices.end(),
                  [](const DeviceRegistration& a, const DeviceRegistration& b) {
                      return a.device < b.device;
                  });
        for (const auto domain_id : impl_->domains.all_domain_ids()) {
            const auto* definition = impl_->domains.find(domain_id);
            if (definition != nullptr) {
                durable.domains.push_back(*definition);
            }
        }
        durable.couplings = impl_->couplings.edges();
        durable.transitions = impl_->history;
        for (const auto& [key, action] : impl_->actions) {
            (void)key;
            durable.actions.push_back(action);
        }
        std::sort(durable.actions.begin(), durable.actions.end(),
                  [](const ThermalAction& a, const ThermalAction& b) { return a.id < b.id; });
        durable.verifications = impl_->verifications;
    }

    // Persistence I/O happens outside the state lock.
    DurableStore store(path, impl_->config.persistence_limits);
    return store.save(durable);
}

Status ThermalGovernor::load_durable() {
    std::string path;
    PersistenceLimits limits;
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        path = impl_->config.durable_path;
        limits = impl_->config.persistence_limits;
    }
    if (path.empty()) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT,
                    "no durable path is configured for this runtime");
    }

    DurableStore store(path, limits);
    if (!store.exists()) {
        return Status::success();
    }
    auto loaded = store.load();
    if (!loaded.has_value()) {
        return Status(loaded.error());
    }

    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const DurableState& durable = loaded.value().state;

    for (const auto& policy : durable.policies) {
        impl_->policies[policy.id.value()] = policy;
    }
    if (durable.policy_generation.is_valid()) {
        Impl::observe(impl_->policy_generation, durable.policy_generation);
    }
    if (durable.topology_generation.is_valid()) {
        Impl::observe(impl_->topology_generation, durable.topology_generation);
    }
    impl_->sequence = std::max(impl_->sequence, durable.sequence);

    // Device identity, generation, node/rack membership and declared
    // capabilities are stable configuration. Restoring them lets the
    // runtime say REVALIDATION_REQUIRED rather than UNKNOWN after a
    // restart: it knows what it could measure, and knows that it holds
    // no current measurement.
    for (const auto& device : durable.devices) {
        impl_->devices[device.device.value()] = device;
    }
    if (!durable.devices.empty()) {
        Impl::advance(impl_->capability_generation);
    }
    for (const auto& definition : durable.domains) {
        auto status = impl_->domains.add(definition);
        if (!status.ok()) {
            return fail(ThermalErrorCode::PERSISTENCE_CORRUPT,
                        "persisted thermal domain could not be restored: " +
                            status.error().detail);
        }
        impl_->history_for(definition.id).set_capacity(impl_->config.max_recovery_samples);
    }
    for (const auto& coupling : durable.couplings) {
        if (impl_->domains.find(coupling.source) == nullptr ||
            impl_->domains.find(coupling.destination) == nullptr) {
            return fail(ThermalErrorCode::PERSISTENCE_CORRUPT,
                        "persisted coupling references an unknown thermal domain");
        }
        impl_->couplings.add(coupling);
    }
    impl_->history = durable.transitions;
    for (const auto& action : durable.actions) {
        ThermalAction restored = action;
        if (!is_terminal(restored.lifecycle)) {
            // Conservative restart semantics: an unresolved action is not
            // assumed to have taken effect, nor assumed to have failed.
            restored.lifecycle = ActionLifecycle::OUTCOME_UNKNOWN;
            restored.detail = "coordinator restarted before the action outcome was proven";
        }
        impl_->actions[restored.id.value()] = restored;
    }
    impl_->verifications = durable.verifications;

    // A restart advances the coordinator epoch. Live thermal authority does
    // not survive it.
    Impl::advance(impl_->epoch);
    impl_->action_baselines.clear();

    // Dynamic thermal evidence is deliberately discarded: no observation
    // silently becomes fresh because the coordinator restarted.
    for (const auto& [key, record] : impl_->evidence) {
        (void)record;
        impl_->invalidated_evidence.insert(key);
    }
    impl_->evidence.clear();
    for (auto& [key, recovery_history] : impl_->recovery_histories) {
        (void)key;
        recovery_history.clear();
    }
    impl_->explicit_recovery.clear();
    impl_->evaluations.clear();
    Impl::advance(impl_->topology_generation);

    impl_->recompute_all_domains();
    impl_->publish_locked();
    return Status::success();
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

Status ThermalGovernor::shutdown() {
    if (impl_->stopping.exchange(true)) {
        return Status::success();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        // Stop new mitigation dispatch and classify in-flight actions
        // honestly rather than optimistically.
        for (auto& [key, action] : impl_->actions) {
            (void)key;
            if (is_terminal(action.lifecycle)) {
                continue;
            }
            switch (action.lifecycle) {
                case ActionLifecycle::PLANNED:
                case ActionLifecycle::AUTHORIZED:
                    action.lifecycle = ActionLifecycle::CANCELLED;
                    action.detail = "runtime shut down before dispatch";
                    break;
                case ActionLifecycle::DISPATCHED:
                case ActionLifecycle::ACKNOWLEDGED:
                case ActionLifecycle::VERIFYING:
                    action.lifecycle = ActionLifecycle::OUTCOME_UNKNOWN;
                    action.detail = "runtime shut down before the outcome was verified";
                    break;
                default:
                    break;
            }
            action.terminal_at = impl_->clock->now();
        }
        impl_->publish_locked();
    }

    Status result = Status::success();
    if (!impl_->config.durable_path.empty()) {
        result = persist();
    }
    SocketRuntime::release();
    return result;
}

}  // namespace thermal_governor
