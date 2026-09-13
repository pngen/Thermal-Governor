// Thermal Governor — deterministic thermal evaluation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/evaluation.hpp"

#include <algorithm>

namespace thermal_governor {
namespace {

[[nodiscard]] bool is_healthy_state(ThermalState state) noexcept {
    return state == ThermalState::NORMAL || state == ThermalState::WARM ||
           state == ThermalState::NEAR_LIMIT;
}

/// The restriction retained when recovery hysteresis forbids relaxation.
[[nodiscard]] ThermalState retention_state(ThermalState previous) noexcept {
    switch (previous) {
        case ThermalState::THROTTLING:
        case ThermalState::CRITICAL:
        case ThermalState::DERATED:
            return ThermalState::DERATED;
        default:
            return previous;
    }
}

[[nodiscard]] ThermalDecision decision_for_unknown(UnknownBehavior behavior) noexcept {
    switch (behavior) {
        case UnknownBehavior::RETURN_UNKNOWN: return ThermalDecision::UNKNOWN;
        case UnknownBehavior::REVALIDATION_REQUIRED: return ThermalDecision::REVALIDATION_REQUIRED;
        case UnknownBehavior::DEFER: return ThermalDecision::DEFER;
        case UnknownBehavior::DENY: return ThermalDecision::DENY;
        case UnknownBehavior::ALLOW_DERATED: return ThermalDecision::ALLOW_DERATED;
    }
    return ThermalDecision::UNKNOWN;
}

[[nodiscard]] ThermalDecision decision_for_unsupported(UnsupportedBehavior behavior) noexcept {
    switch (behavior) {
        case UnsupportedBehavior::RETURN_UNSUPPORTED: return ThermalDecision::UNSUPPORTED;
        case UnsupportedBehavior::REVALIDATION_REQUIRED: return ThermalDecision::REVALIDATION_REQUIRED;
        case UnsupportedBehavior::DEFER: return ThermalDecision::DEFER;
        case UnsupportedBehavior::DENY: return ThermalDecision::DENY;
    }
    return ThermalDecision::UNSUPPORTED;
}

[[nodiscard]] DeratingLevel derating_for_unsupported() noexcept {
    return DeratingLevel::UNSUPPORTED;
}

}  // namespace

ThermalState next_thermal_state(const EvaluationInput& input, const RecoveryAssessment& recovery,
                                TemperatureDelta& out_raw_headroom, ThrottleClass& out_throttle,
                                ReasonCodeSet& out_reasons) {
    out_raw_headroom = TemperatureDelta{0.0};

    const ThermalEvidence& evidence = *input.evidence;
    const ThermalPolicy& policy = input.policy;
    const auto& thresholds = policy.thresholds;

    out_raw_headroom = DegreesCelsius{thresholds.critical.value()} - evidence.temperature;

    // Throttle classification. Absence of evidence is not evidence of
    // absence: an unsupported throttle source never becomes NO_THROTTLE.
    if (input.throttle_capability == CapabilityState::UNSUPPORTED) {
        out_throttle = ThrottleClass::THROTTLE_UNSUPPORTED;
        out_reasons.add(ThermalReasonCode::THROTTLE_EVIDENCE_UNSUPPORTED);
    } else if (evidence.throttle.has_value()) {
        out_throttle = evidence.throttle->classification;
        switch (out_throttle) {
            case ThrottleClass::THERMAL_THROTTLE_OBSERVED:
                out_reasons.add(ThermalReasonCode::THERMAL_THROTTLE_OBSERVED);
                break;
            case ThrottleClass::NON_THERMAL_THROTTLE_OBSERVED:
                out_reasons.add(ThermalReasonCode::NON_THERMAL_THROTTLE_OBSERVED);
                break;
            case ThrottleClass::NO_THROTTLE_OBSERVED:
                out_reasons.add(ThermalReasonCode::NO_THROTTLE_OBSERVED);
                break;
            case ThrottleClass::THROTTLE_UNSUPPORTED:
                out_reasons.add(ThermalReasonCode::THROTTLE_EVIDENCE_UNSUPPORTED);
                break;
            case ThrottleClass::THROTTLE_UNKNOWN:
                out_reasons.add(ThermalReasonCode::THROTTLE_EVIDENCE_UNKNOWN);
                break;
        }
    } else if (input.throttle_capability == CapabilityState::UNKNOWN) {
        out_throttle = ThrottleClass::THROTTLE_UNKNOWN;
        out_reasons.add(ThermalReasonCode::THROTTLE_EVIDENCE_UNKNOWN);
    } else {
        out_throttle = ThrottleClass::THROTTLE_UNKNOWN;
        out_reasons.add(ThermalReasonCode::THROTTLE_EVIDENCE_UNKNOWN);
    }

    // Temperature band. Hard predicates are evaluated before anything else
    // and in severity order: critical dominates throttle evidence, which
    // dominates the derating threshold.
    ThermalState candidate = ThermalState::NORMAL;
    if (evidence.temperature.value() >= thresholds.critical.value()) {
        candidate = ThermalState::CRITICAL;
        out_reasons.add(ThermalReasonCode::CRITICAL_THRESHOLD_EXCEEDED);
    } else if (out_throttle == ThrottleClass::THERMAL_THROTTLE_OBSERVED) {
        candidate = ThermalState::THROTTLING;
    } else if (evidence.temperature.value() >= thresholds.derating.value()) {
        candidate = ThermalState::DERATED;
        out_reasons.add(ThermalReasonCode::DERATING_THRESHOLD_EXCEEDED);
    } else if (evidence.temperature.value() >= thresholds.near_limit_start().value()) {
        candidate = ThermalState::NEAR_LIMIT;
        out_reasons.add(ThermalReasonCode::TEMPERATURE_IN_NEAR_LIMIT_BAND);
    } else if (evidence.temperature.value() >= thresholds.warning.value()) {
        candidate = ThermalState::WARM;
        out_reasons.add(ThermalReasonCode::TEMPERATURE_IN_WARNING_BAND);
    } else {
        candidate = ThermalState::NORMAL;
        out_reasons.add(ThermalReasonCode::TEMPERATURE_BELOW_WARNING);
    }

    ReasonCodeSet coupling_reasons;
    bool coupled_pressure = false;
    for (const auto& pressure : input.coupled) {
        if (!policy.coupling.co_derate_coupled_domains) {
            break;
        }
        const bool restrictive = pressure.state == ThermalState::DERATED ||
                                 pressure.state == ThermalState::THROTTLING ||
                                 pressure.state == ThermalState::CRITICAL;
        const bool unresolved = pressure.state == ThermalState::UNKNOWN ||
                                pressure.state == ThermalState::UNSUPPORTED ||
                                pressure.state == ThermalState::REVALIDATION_REQUIRED;
        if (restrictive && severity(pressure.state) >= severity(ThermalState::DERATED)) {
            coupled_pressure = true;
            if (policy.coupling.escalate_to_hottest_neighbour &&
                severity(pressure.state) > severity(candidate)) {
                candidate = pressure.state;
            }
        } else if (unresolved && policy.coupling.escalate_to_hottest_neighbour) {
            coupled_pressure = true;
            if (severity(pressure.state) > severity(candidate)) {
                candidate = pressure.state;
            }
        }
        if (pressure.provenance == Provenance::SYNTHETIC) {
            coupling_reasons.add(ThermalReasonCode::COUPLING_PROVENANCE_SYNTHETIC);
        }
    }
    if (coupled_pressure) {
        coupling_reasons.add(ThermalReasonCode::COUPLED_DOMAIN_PRESSURE);
        // Coupled pressure may only tighten the candidate state.
        if (is_healthy_state(candidate)) {
            candidate = ThermalState::DERATED;
        }
    }
    for (const auto code : coupling_reasons.codes()) {
        out_reasons.add(code);
    }

    if (!is_healthy_state(candidate)) {
        return candidate;
    }

    // Hysteresis gate. Leaving a restrictive state requires positive fresh
    // evidence, never the mere absence of a violation.
    if (!requires_recovery_gate(input.previous_state)) {
        return candidate;
    }
    if (input.previous_state == ThermalState::REVALIDATION_REQUIRED) {
        // Fresh admissible evidence is what clears a revalidation demand.
        out_reasons.add(ThermalReasonCode::AUTHORITY_REVALIDATED);
        return candidate;
    }

    if (recovery.allowed) {
        out_reasons.add(ThermalReasonCode::RECOVERY_AUTHORIZED);
        return candidate;
    }

    out_reasons.add(ThermalReasonCode::STATE_RETAINED_BY_HYSTERESIS);
    for (const auto code : recovery.blocking_reasons.codes()) {
        out_reasons.add(code);
    }
    return retention_state(input.previous_state);
}

DeratingLevel derating_for_state(ThermalState state, const ThermalPolicy& policy,
                                 bool workload_restricted) {
    const Percent ceiling = concurrency_ceiling(policy.concurrency, state);
    switch (state) {
        case ThermalState::NORMAL:
            return DeratingLevel::FULL_CAPABILITY;
        case ThermalState::WARM:
            return workload_restricted ? DeratingLevel::DERATED_WORKLOAD_CLASS
                                       : DeratingLevel::FULL_CAPABILITY;
        case ThermalState::NEAR_LIMIT:
            return workload_restricted ? DeratingLevel::DERATED_COMBINED
                                       : DeratingLevel::DERATED_CONCURRENCY;
        case ThermalState::DERATED:
            return ceiling.value() < 100.0 ? DeratingLevel::DERATED_COMBINED
                                           : DeratingLevel::DERATED_CLOCK;
        case ThermalState::THROTTLING:
            return DeratingLevel::DERATED_COMBINED;
        case ThermalState::CRITICAL:
            return DeratingLevel::NO_SAFE_EXECUTION;
        case ThermalState::REVALIDATION_REQUIRED:
            return DeratingLevel::REVALIDATION_REQUIRED;
        case ThermalState::UNKNOWN:
            return DeratingLevel::UNKNOWN;
        case ThermalState::UNSUPPORTED:
            return DeratingLevel::UNSUPPORTED;
    }
    return DeratingLevel::UNKNOWN;
}

std::vector<MitigationIntent> prohibited_intents_for(ThermalState state,
                                                     const ThermalPolicy& policy) {
    std::vector<MitigationIntent> out;
    const bool restricted = state == ThermalState::DERATED ||
                            state == ThermalState::THROTTLING ||
                            state == ThermalState::CRITICAL;
    const bool unresolved = state == ThermalState::UNKNOWN ||
                            state == ThermalState::UNSUPPORTED ||
                            state == ThermalState::REVALIDATION_REQUIRED;
    if (restricted || unresolved) {
        // Capability restoration is prohibited while a restriction or an
        // unresolved thermal condition stands.
        out.push_back(MitigationIntent::REQUEST_CLOCK_RESTORE);
        out.push_back(MitigationIntent::REQUEST_POWER_RESTORE);
        out.push_back(MitigationIntent::REQUEST_ADMISSION_RESTORE);
        out.push_back(MitigationIntent::REQUEST_CONCURRENCY_RESTORE);
    }
    if (state != ThermalState::CRITICAL) {
        // Containment is reserved for genuine thermal danger.
        out.push_back(MitigationIntent::REQUEST_CONTAINMENT);
    }
    if (!policy_allows(policy, MitigationIntent::REQUEST_WORKLOAD_MIGRATION)) {
        out.push_back(MitigationIntent::REQUEST_WORKLOAD_MIGRATION);
    }
    if (!policy_allows(policy, MitigationIntent::REQUEST_COOLING_INTERVENTION)) {
        out.push_back(MitigationIntent::REQUEST_COOLING_INTERVENTION);
    }
    std::sort(out.begin(), out.end(), [](MitigationIntent a, MitigationIntent b) {
        return static_cast<std::uint8_t>(a) < static_cast<std::uint8_t>(b);
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

ThermalEvaluation evaluate_thermal(const EvaluationInput& input) {
    ThermalEvaluation out;
    out.domain = input.domain;
    out.domain_generation = input.domain_generation;
    out.policy_generation = input.policy_generation;
    out.topology_generation = input.topology_generation;
    out.coordinator_epoch = input.coordinator_epoch;
    out.capability_generation = input.evidence != nullptr ? input.evidence->capability_generation
                                                          : CapabilityGeneration{};
    out.telemetry_generation =
        input.evidence != nullptr ? input.evidence->telemetry_generation : TelemetryGeneration{};
    out.provenance = input.evidence != nullptr ? input.evidence->provenance : Provenance::UNKNOWN;

    const ThermalPolicy& policy = input.policy;
    out.headroom = HeadroomBreakdown{};
    out.headroom.policy_critical = policy.thresholds.critical;
    out.headroom.recovery_threshold = policy.thresholds.recovery;
    out.headroom.recovery_margin = policy.margins.recovery_margin;
    out.headroom.policy_safety_margin = policy.margins.policy_safety_margin;

    // Hard predicate 1: capability resolution.
    if (input.temperature_capability == CapabilityState::UNSUPPORTED) {
        out.state = ThermalState::UNSUPPORTED;
        out.decision = decision_for_unsupported(policy.unsupported_behavior);
        out.derating = derating_for_unsupported();
        out.unclamped_derating = out.derating;
        out.reasons.add(ThermalReasonCode::CAPABILITY_UNSUPPORTED);
        out.reasons.add(ThermalReasonCode::POLICY_UNSUPPORTED_BEHAVIOR_APPLIED);
        out.permitted_concurrency = Percent{0.0};
        out.permitted_execution_class = ExecutionClass::BEST_EFFORT_THERMAL;
        out.rejected.push_back({DeratingLevel::FULL_CAPABILITY,
                                ThermalReasonCode::CAPABILITY_UNSUPPORTED});
        out.prohibited_intents = prohibited_intents_for(out.state, policy);
        out.reasons.canonicalise();
        return out;
    }

    // Hard predicate 2: capability resolution. An unresolved capability is
    // not silently treated as a working one merely because a reading exists.
    if (input.temperature_capability == CapabilityState::UNKNOWN) {
        out.state = ThermalState::UNKNOWN;
        out.decision = decision_for_unknown(policy.unknown_behavior);
        out.derating = DeratingLevel::UNKNOWN;
        out.unclamped_derating = out.derating;
        out.reasons.add(ThermalReasonCode::CAPABILITY_UNRESOLVED);
        out.reasons.add(ThermalReasonCode::POLICY_UNKNOWN_BEHAVIOR_APPLIED);
        out.permitted_concurrency = concurrency_ceiling(policy.concurrency, out.state);
        out.permitted_execution_class = ExecutionClass::BEST_EFFORT_THERMAL;
        out.rejected.push_back(
            {DeratingLevel::FULL_CAPABILITY, ThermalReasonCode::CAPABILITY_UNRESOLVED});
        out.prohibited_intents = prohibited_intents_for(out.state, policy);
        out.reasons.canonicalise();
        return out;
    }

    // Hard predicate 3: evidence availability.
    if (input.evidence == nullptr) {
        out.state = input.temperature_capability == CapabilityState::UNKNOWN
                        ? ThermalState::UNKNOWN
                        : ThermalState::REVALIDATION_REQUIRED;
        out.reasons.add(ThermalReasonCode::EVIDENCE_MISSING);
        if (out.state == ThermalState::UNKNOWN) {
            out.decision = decision_for_unknown(policy.unknown_behavior);
            out.reasons.add(ThermalReasonCode::POLICY_UNKNOWN_BEHAVIOR_APPLIED);
        } else {
            out.decision = ThermalDecision::REVALIDATION_REQUIRED;
        }
        out.derating = out.state == ThermalState::UNKNOWN ? DeratingLevel::UNKNOWN
                                                          : DeratingLevel::REVALIDATION_REQUIRED;
        out.unclamped_derating = out.derating;
        out.permitted_concurrency = concurrency_ceiling(policy.concurrency, out.state);
        out.permitted_execution_class = ExecutionClass::BEST_EFFORT_THERMAL;
        out.rejected.push_back({DeratingLevel::FULL_CAPABILITY, ThermalReasonCode::EVIDENCE_MISSING});
        out.prohibited_intents = prohibited_intents_for(out.state, policy);
        return out;
    }

    if (input.evidence_conflict) {
        out.state = ThermalState::REVALIDATION_REQUIRED;
        out.decision = ThermalDecision::REVALIDATION_REQUIRED;
        out.derating = DeratingLevel::REVALIDATION_REQUIRED;
        out.unclamped_derating = out.derating;
        out.reasons.add(ThermalReasonCode::EVIDENCE_CONFLICT);
        out.permitted_concurrency = concurrency_ceiling(policy.concurrency, out.state);
        out.permitted_execution_class = ExecutionClass::BEST_EFFORT_THERMAL;
        out.rejected.push_back(
            {DeratingLevel::FULL_CAPABILITY, ThermalReasonCode::EVIDENCE_CONFLICT});
        out.prohibited_intents = prohibited_intents_for(out.state, policy);
        return out;
    }

    // Hard predicate 3: evidence freshness. A stale generation cannot
    // authorise execution.
    if (input.freshness != FreshnessState::FRESH) {
        out.state = ThermalState::REVALIDATION_REQUIRED;
        out.decision = ThermalDecision::REVALIDATION_REQUIRED;
        out.derating = DeratingLevel::REVALIDATION_REQUIRED;
        out.unclamped_derating = out.derating;
        const ThermalReasonCode freshness_reason =
            input.freshness == FreshnessState::STALE
                ? ThermalReasonCode::EVIDENCE_STALE
                : (input.freshness == FreshnessState::GENERATION_INVALIDATED
                       ? ThermalReasonCode::EVIDENCE_GENERATION_INVALIDATED
                       : ThermalReasonCode::EVIDENCE_STALE);
        out.reasons.add(freshness_reason);
        out.permitted_concurrency = concurrency_ceiling(policy.concurrency, out.state);
        out.permitted_execution_class = ExecutionClass::BEST_EFFORT_THERMAL;
        out.rejected.push_back({DeratingLevel::FULL_CAPABILITY, freshness_reason});
        out.prohibited_intents = prohibited_intents_for(out.state, policy);
        return out;
    }

    const ThermalEvidence& evidence = *input.evidence;

    // Headroom. A REAL vendor limit may tighten the governing ceiling; a
    // synthetic one never can.
    HeadroomInput headroom_input;
    headroom_input.current_temperature = evidence.temperature;
    headroom_input.policy_critical = policy.thresholds.critical;
    headroom_input.policy_safety_margin = policy.margins.policy_safety_margin;
    headroom_input.recovery_threshold = policy.thresholds.recovery;
    headroom_input.recovery_margin = policy.margins.recovery_margin;
    if (input.limit_capability != CapabilityState::UNSUPPORTED &&
        evidence.temperature_limit.has_value()) {
        headroom_input.vendor_limit = evidence.temperature_limit;
        headroom_input.vendor_limit_provenance = evidence.provenance;
    }
    headroom_input.uncertainty_margin =
        uncertainty_from_confidence(evidence.confidence, policy.margins.uncertainty_margin);
    out.headroom = compute_headroom(headroom_input);

    // Recovery assessment, evaluated before the state machine so hysteresis
    // is governed by policy rather than by temperature alone.
    RecoveryInput recovery_input = input.recovery;
    recovery_input.policy = &policy;
    recovery_input.recovery_threshold = policy.thresholds.recovery;
    recovery_input.recovery_margin = policy.margins.recovery_margin;
    recovery_input.headroom = out.headroom;
    if (input.recovery.history == nullptr) {
        recovery_input.history = nullptr;
    }
    out.recovery = evaluate_recovery(recovery_input);

    ThrottleClass throttle = ThrottleClass::THROTTLE_UNKNOWN;
    TemperatureDelta raw = TemperatureDelta{0.0};
    out.state = next_thermal_state(input, out.recovery, raw, throttle, out.reasons);
    out.throttle_class = throttle;

    if (out.headroom.vendor_limit.has_value() &&
        out.headroom.governing_limit.value() == out.headroom.vendor_limit->value() &&
        out.headroom.vendor_limit->value() < policy.thresholds.critical.value()) {
        out.reasons.add(ThermalReasonCode::VENDOR_LIMIT_GOVERNS);
    } else {
        out.reasons.add(ThermalReasonCode::POLICY_LIMIT_GOVERNS);
    }

    if (out.headroom.raw_headroom.value() < 0.0) {
        out.reasons.add(ThermalReasonCode::RAW_HEADROOM_NEGATIVE);
    }

    // Decide.
    switch (out.state) {
        case ThermalState::NORMAL:
        case ThermalState::WARM:
        case ThermalState::NEAR_LIMIT:
            out.decision = ThermalDecision::ALLOW;
            break;
        case ThermalState::DERATED:
        case ThermalState::THROTTLING:
            out.decision = ThermalDecision::ALLOW_DERATED;
            break;
        case ThermalState::CRITICAL:
            out.decision = policy.emergency.deny_all_execution ? ThermalDecision::DENY
                                                               : ThermalDecision::ALLOW_DERATED;
            out.reasons.add(policy.emergency.deny_all_execution
                                ? ThermalReasonCode::EMERGENCY_EXECUTION_DENIED
                                : ThermalReasonCode::EMERGENCY_CONTAINMENT_REQUIRED);
            break;
        case ThermalState::REVALIDATION_REQUIRED:
            out.decision = ThermalDecision::REVALIDATION_REQUIRED;
            break;
        case ThermalState::UNKNOWN:
            out.decision = decision_for_unknown(policy.unknown_behavior);
            out.reasons.add(ThermalReasonCode::POLICY_UNKNOWN_BEHAVIOR_APPLIED);
            break;
        case ThermalState::UNSUPPORTED:
            out.decision = decision_for_unsupported(policy.unsupported_behavior);
            out.reasons.add(ThermalReasonCode::POLICY_UNSUPPORTED_BEHAVIOR_APPLIED);
            break;
    }

    // Workload-class restriction.
    ExecutionClass workload_class = ExecutionClass::UNRESTRICTED;
    const bool workload_listed =
        workload_execution_class(policy, input.workload_profile, workload_class);
    bool workload_restricted = false;
    if (!workload_listed) {
        out.reasons.add(ThermalReasonCode::WORKLOAD_PROFILE_EXCLUDED);
        workload_restricted = true;
        workload_class = ExecutionClass::BEST_EFFORT_THERMAL;
    } else {
        if (workload_class != ExecutionClass::UNRESTRICTED) {
            workload_restricted = true;
        }
        if (input.workload_profile == WorkloadThermalProfile::UNKNOWN_PROFILE) {
            out.reasons.add(ThermalReasonCode::WORKLOAD_PROFILE_UNKNOWN);
        }
    }
    const TemperatureDelta workload_demand =
        workload_headroom_demand(policy, input.workload_profile);
    if (workload_demand.value() > 0.0) {
        workload_restricted = true;
        out.reasons.add(ThermalReasonCode::WORKLOAD_REQUIRES_DERATING);
    }

    out.derating = derating_for_state(out.state, policy, workload_restricted);
    out.unclamped_derating = out.derating;

    // Derating cannot expand authority. When a previous restriction exists
    // and recovery has not been authorised, the level may only tighten.
    if (input.previous_derating != DeratingLevel::UNKNOWN &&
        !is_healthy_state(out.state) && out.state != ThermalState::REVALIDATION_REQUIRED &&
        !does_not_expand(out.derating, input.previous_derating)) {
        out.derating = most_restrictive(out.derating, input.previous_derating);
        out.reasons.add(ThermalReasonCode::DERATING_CANNOT_EXPAND);
    }

    out.permitted_concurrency = concurrency_ceiling(policy.concurrency, out.state);
    out.permitted_workload_intensity = out.permitted_concurrency.value() / 100.0;
    out.permitted_execution_class = workload_restricted ? workload_class
                                                        : ExecutionClass::UNRESTRICTED;
    if (out.state == ThermalState::CRITICAL) {
        out.permitted_execution_class = ExecutionClass::BEST_EFFORT_THERMAL;
    }

    // Admission headroom predicate.
    const TemperatureDelta demand{policy.admission.min_effective_headroom.value() +
                                  workload_demand.value()};
    if (out.headroom.meets_demand(demand)) {
        out.reasons.add(ThermalReasonCode::EFFECTIVE_HEADROOM_SUFFICIENT);
    } else {
        out.reasons.add(ThermalReasonCode::EFFECTIVE_HEADROOM_BELOW_DEMAND);
        if (out.decision == ThermalDecision::ALLOW) {
            out.decision = policy.admission.allow_derated_admission ? ThermalDecision::ALLOW_DERATED
                                                                    : ThermalDecision::DENY;
            out.reasons.add(policy.admission.allow_derated_admission
                                ? ThermalReasonCode::DERATED_ADMISSION_ALLOWED
                                : ThermalReasonCode::DERATED_ADMISSION_FORBIDDEN);
        }
    }
    if (out.permitted_concurrency.value() < 100.0) {
        out.reasons.add(ThermalReasonCode::CONCURRENCY_CEILING_APPLIED);
    }

    // Mitigation intents.
    const auto clamp_clock = [&]() -> std::optional<MegaHertz> {
        if (!evidence.current_clock.has_value() || !evidence.max_clock.has_value()) {
            return std::nullopt;
        }
        const double target = static_cast<double>(evidence.max_clock->value()) *
                              (out.permitted_concurrency.value() / 100.0);
        const auto ceiling = static_cast<std::uint32_t>(target);
        if (ceiling >= evidence.current_clock->value()) {
            return std::nullopt;
        }
        return MegaHertz{ceiling};
    };

    switch (out.state) {
        case ThermalState::NEAR_LIMIT:
            if (policy_allows(policy, MitigationIntent::REQUEST_CONCURRENCY_REDUCTION)) {
                out.intents.add(MitigationIntent::REQUEST_CONCURRENCY_REDUCTION, input.domain,
                                "near-limit temperature band");
            }
            break;
        case ThermalState::DERATED:
        case ThermalState::THROTTLING: {
            if (policy_allows(policy, MitigationIntent::REQUEST_CLOCK_REDUCTION)) {
                MitigationIntentRecord record;
                record.intent = MitigationIntent::REQUEST_CLOCK_REDUCTION;
                record.domain = input.domain;
                record.domain_generation = input.domain_generation;
                record.clock_ceiling = clamp_clock();
                record.rationale = out.state == ThermalState::THROTTLING
                                       ? "thermal throttle observed"
                                       : "derating threshold exceeded";
                out.intents.add(std::move(record));
            }
            if (policy_allows(policy, MitigationIntent::REQUEST_CONCURRENCY_REDUCTION)) {
                MitigationIntentRecord record;
                record.intent = MitigationIntent::REQUEST_CONCURRENCY_REDUCTION;
                record.domain = input.domain;
                record.domain_generation = input.domain_generation;
                record.concurrency_ceiling = out.permitted_concurrency;
                record.rationale = "thermal derating";
                out.intents.add(std::move(record));
            }
            if (out.state == ThermalState::THROTTLING &&
                policy_allows(policy, MitigationIntent::REQUEST_COOLING_INTERVENTION)) {
                out.intents.add(MitigationIntent::REQUEST_COOLING_INTERVENTION, input.domain,
                                "thermal throttling observed");
            }
            break;
        }
        case ThermalState::CRITICAL: {
            if (policy.emergency.allow_containment_intent &&
                policy_allows(policy, MitigationIntent::REQUEST_CONTAINMENT)) {
                out.intents.add(MitigationIntent::REQUEST_CONTAINMENT, input.domain,
                                "critical thermal threshold exceeded");
            }
            if (policy.emergency.allow_migration_intent &&
                policy_allows(policy, MitigationIntent::REQUEST_WORKLOAD_MIGRATION)) {
                out.intents.add(MitigationIntent::REQUEST_WORKLOAD_MIGRATION, input.domain,
                                "critical thermal threshold exceeded");
            }
            if (policy.emergency.allow_cooling_intervention_intent &&
                policy_allows(policy, MitigationIntent::REQUEST_COOLING_INTERVENTION)) {
                out.intents.add(MitigationIntent::REQUEST_COOLING_INTERVENTION, input.domain,
                                "critical thermal threshold exceeded");
            }
            break;
        }
        case ThermalState::WARM:
        case ThermalState::NORMAL: {
            // Restoration is only requested when recovery has actually been
            // authorised and the previous envelope was restricted.
            const bool was_restricted =
                restriction_rank(input.previous_derating) > restriction_rank(DeratingLevel::FULL_CAPABILITY);
            if (was_restricted && out.recovery.allowed) {
                if (policy_allows(policy, MitigationIntent::REQUEST_CLOCK_RESTORE)) {
                    out.intents.add(MitigationIntent::REQUEST_CLOCK_RESTORE, input.domain,
                                    "recovery authorised by fresh evidence");
                }
                if (policy_allows(policy, MitigationIntent::REQUEST_CONCURRENCY_RESTORE)) {
                    out.intents.add(MitigationIntent::REQUEST_CONCURRENCY_RESTORE, input.domain,
                                    "recovery authorised by fresh evidence");
                }
            }
            break;
        }
        case ThermalState::REVALIDATION_REQUIRED:
        case ThermalState::UNKNOWN:
        case ThermalState::UNSUPPORTED:
            break;
    }

    if (out.intents.empty()) {
        // NO_ACTION is the explicit representation of "nothing to request".
        MitigationIntentRecord record;
        record.intent = MitigationIntent::NO_ACTION;
        record.domain = input.domain;
        record.rationale = "no thermal mitigation required";
        out.intents.add(std::move(record));
    }
    out.intents.canonicalise();

    out.prohibited_intents = prohibited_intents_for(out.state, policy);

    // Rejected alternatives, in stable order.
    if (out.derating != DeratingLevel::FULL_CAPABILITY) {
        ThermalReasonCode reason = ThermalReasonCode::NONE;
        switch (out.state) {
            case ThermalState::CRITICAL: reason = ThermalReasonCode::CRITICAL_THRESHOLD_EXCEEDED; break;
            case ThermalState::THROTTLING: reason = ThermalReasonCode::THERMAL_THROTTLE_OBSERVED; break;
            case ThermalState::DERATED: reason = ThermalReasonCode::DERATING_THRESHOLD_EXCEEDED; break;
            case ThermalState::NEAR_LIMIT: reason = ThermalReasonCode::TEMPERATURE_IN_NEAR_LIMIT_BAND; break;
            case ThermalState::WARM: reason = ThermalReasonCode::TEMPERATURE_IN_WARNING_BAND; break;
            case ThermalState::REVALIDATION_REQUIRED: reason = ThermalReasonCode::EVIDENCE_STALE; break;
            case ThermalState::UNKNOWN: reason = ThermalReasonCode::EVIDENCE_MISSING; break;
            case ThermalState::UNSUPPORTED: reason = ThermalReasonCode::CAPABILITY_UNSUPPORTED; break;
            case ThermalState::NORMAL: reason = ThermalReasonCode::WORKLOAD_REQUIRES_DERATING; break;
        }
        out.rejected.push_back({DeratingLevel::FULL_CAPABILITY, reason});
    }
    if (out.derating != DeratingLevel::NO_SAFE_EXECUTION) {
        out.rejected.push_back({DeratingLevel::NO_SAFE_EXECUTION,
                                ThermalReasonCode::CRITICAL_THRESHOLD_EXCEEDED});
    }
    if (!out.intents.contains(MitigationIntent::REQUEST_CONTAINMENT) &&
        out.state != ThermalState::CRITICAL) {
        out.rejected.push_back(
            {DeratingLevel::NO_SAFE_EXECUTION, ThermalReasonCode::EMERGENCY_CONTAINMENT_REQUIRED});
    }

    out.reasons.canonicalise();
    out.retained_by_hysteresis =
        out.reasons.contains(ThermalReasonCode::STATE_RETAINED_BY_HYSTERESIS);
    return out;
}

}  // namespace thermal_governor
