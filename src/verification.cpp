// Thermal Governor — post-action verification.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/verification.hpp"

#include <algorithm>
#include <cmath>

#include "format.hpp"

namespace thermal_governor {

Result<ActionVerification> verify_action(const VerificationInput& input) {
    if (input.action == nullptr) {
        return ThermalError{ThermalErrorCode::INVALID_ARGUMENT,
                            "verification requires an action record"};
    }
    const ThermalAction& action = *input.action;

    if (!input.fresh_evidence) {
        return ThermalError{ThermalErrorCode::EVIDENCE_STALE,
                            "post-action verification requires fresh thermal evidence"};
    }
    if (input.freshness != FreshnessState::FRESH) {
        return ThermalError{ThermalErrorCode::EVIDENCE_STALE,
                            "post-action verification evidence is not fresh"};
    }
    if (!(input.current.coordinator_epoch == action.authority.coordinator_epoch)) {
        return ThermalError{ThermalErrorCode::STALE_EPOCH,
                            "coordinator epoch advanced before verification"};
    }
    if (action.authority.worker.is_valid() &&
        !(input.current.worker_boot == action.authority.worker_boot)) {
        return ThermalError{ThermalErrorCode::STALE_WORKER,
                            "worker boot identity changed before verification"};
    }
    if (action.authority.device.is_valid() &&
        !(input.current.device_generation == action.authority.device_generation)) {
        return ThermalError{ThermalErrorCode::STALE_DEVICE_GENERATION,
                            "device generation advanced before verification"};
    }
    if (!(input.current.domain_generation == action.authority.domain_generation)) {
        return ThermalError{ThermalErrorCode::STALE_DOMAIN_GENERATION,
                            "thermal domain generation advanced before verification"};
    }
    if (input.current.telemetry_generation.value() <
        action.authority.telemetry_generation.value()) {
        return ThermalError{ThermalErrorCode::STALE_TELEMETRY,
                            "verification telemetry predates the action authority"};
    }
    if (input.current.action_generation.value() < action.generation.value()) {
        return ThermalError{ThermalErrorCode::STALE_ACTION,
                            "verification refers to a superseded action generation"};
    }

    ActionVerification out;
    out.action_id = action.id;
    out.temperature_before = input.before.headroom.current_temperature;
    out.temperature_after = input.after.headroom.current_temperature;
    out.delta = out.temperature_after - out.temperature_before;
    out.headroom_before = input.before.headroom.effective_headroom;
    out.headroom_after = input.after.headroom.effective_headroom;
    out.state_before = input.before.state;
    out.state_after = input.after.state;
    out.derating_before = input.before.derating;
    out.derating_after = input.after.derating;
    out.telemetry_generation = input.current.telemetry_generation;
    out.policy_generation = input.verification_policy_generation;
    out.coordinator_epoch = input.current.coordinator_epoch;

    const double epsilon = 0.5;
    const bool improved = out.delta.value() < -epsilon;
    const bool worsened = out.delta.value() > epsilon;

    const bool constraint_violated = input.after.state == ThermalState::DERATED ||
                                     input.after.state == ThermalState::THROTTLING ||
                                     input.after.state == ThermalState::CRITICAL;
    out.hard_constraint_still_violated = constraint_violated;
    out.derating_still_required = input.after.derating != DeratingLevel::FULL_CAPABILITY;
    out.recovery_conditions_satisfied = input.after.recovery.allowed;

    const bool derating_relaxed =
        restriction_rank(input.after.derating) < restriction_rank(input.before.derating);
    const bool derating_tightened =
        restriction_rank(input.after.derating) > restriction_rank(input.before.derating);

    if (input.secondary_hard_violation) {
        out.outcome = VerificationOutcome::SECONDARY_VIOLATION_CREATED;
        out.resulting_lifecycle = ActionLifecycle::WORSENED;
        out.reasons.add(ThermalReasonCode::EVIDENCE_CONFLICT);
    } else if (derating_tightened || worsened) {
        out.outcome = VerificationOutcome::THERMAL_STATE_WORSENED;
        out.resulting_lifecycle = ActionLifecycle::WORSENED;
    } else if (!improved && !derating_relaxed && constraint_violated) {
        out.outcome = VerificationOutcome::DERATING_INEFFECTIVE;
        out.resulting_lifecycle = ActionLifecycle::INEFFECTIVE;
    } else if (improved && constraint_violated) {
        out.outcome = VerificationOutcome::DERATING_PARTIALLY_EFFECTIVE;
        out.resulting_lifecycle = ActionLifecycle::PARTIALLY_EFFECTIVE;
    } else if (derating_relaxed) {
        out.outcome = VerificationOutcome::DERATING_EFFECTIVE;
        out.resulting_lifecycle = ActionLifecycle::EFFECTIVE;
    } else if (improved) {
        out.outcome = VerificationOutcome::THERMAL_STATE_IMPROVED;
        out.resulting_lifecycle = ActionLifecycle::EFFECTIVE;
    } else {
        out.outcome = VerificationOutcome::THERMAL_STATE_UNCHANGED;
        // No proven change is not proof of effect.
        out.resulting_lifecycle = ActionLifecycle::INEFFECTIVE;
    }

    // A restore intent is verified against recovery gating, not against
    // temperature alone.
    if (is_restoring_intent(action.intent)) {
        if (out.recovery_conditions_satisfied) {
            out.outcome = VerificationOutcome::RECOVERY_ALLOWED;
            out.resulting_lifecycle = ActionLifecycle::EFFECTIVE;
        } else {
            out.outcome = VerificationOutcome::RECOVERY_FORBIDDEN;
            out.resulting_lifecycle = ActionLifecycle::INEFFECTIVE;
        }
    }

    if (out.recovery_conditions_satisfied) {
        out.reasons.add(ThermalReasonCode::RECOVERY_AUTHORIZED);
    } else {
        out.reasons.add(ThermalReasonCode::RECOVERY_FORBIDDEN_BY_POLICY);
    }
    if (improved) {
        out.reasons.add(ThermalReasonCode::EFFECTIVE_HEADROOM_SUFFICIENT);
    }
    if (constraint_violated) {
        out.reasons.add(ThermalReasonCode::DERATING_THRESHOLD_EXCEEDED);
    }
    // A change of policy generation supersedes the verification even though
    // the observation itself is admissible.
    if (input.verification_policy_generation.is_valid() &&
        !(input.verification_policy_generation == action.authority.policy_generation)) {
        out.outcome = VerificationOutcome::OUTCOME_UNKNOWN;
        out.resulting_lifecycle = ActionLifecycle::SUPERSEDED;
        out.reasons.add(ThermalReasonCode::NONE);
        out.reasons = ReasonCodeSet{};
        out.reasons.add(ThermalReasonCode::EVIDENCE_GENERATION_INVALIDATED);
        out.recovery_conditions_satisfied = false;
    }

    out.reasons.canonicalise();
    return out;
}

std::string render_verification(const ActionVerification& verification) {
    std::string out;
    out += "ActionId: " + detail::unsigned_decimal(verification.action_id.value()) + "\n";
    out += "VerificationGeneration: " +
           detail::unsigned_decimal(verification.generation.value()) + "\n";
    out += "Outcome: ";
    out += to_string(verification.outcome);
    out += "\n";
    out += "ResultingLifecycle: ";
    out += to_string(verification.resulting_lifecycle);
    out += "\n";
    out += "TemperatureBefore: " +
           detail::temperature(verification.temperature_before.value()) + "\n";
    out += "TemperatureAfter: " + detail::temperature(verification.temperature_after.value()) +
           "\n";
    out += "Delta: " + detail::signed_number(verification.delta.value()) + "\n";
    out += "HeadroomBefore: " + detail::number(verification.headroom_before.value()) + "\n";
    out += "HeadroomAfter: " + detail::number(verification.headroom_after.value()) + "\n";
    out += "StateBefore: ";
    out += to_string(verification.state_before);
    out += "\n";
    out += "StateAfter: ";
    out += to_string(verification.state_after);
    out += "\n";
    out += "DeratingBefore: ";
    out += to_string(verification.derating_before);
    out += "\n";
    out += "DeratingAfter: ";
    out += to_string(verification.derating_after);
    out += "\n";
    out += "HardConstraintStillViolated: ";
    out += verification.hard_constraint_still_violated ? "true" : "false";
    out += "\n";
    out += "DeratingStillRequired: ";
    out += verification.derating_still_required ? "true" : "false";
    out += "\n";
    out += "RecoveryConditionsSatisfied: ";
    out += verification.recovery_conditions_satisfied ? "true" : "false";
    out += "\n";
    out += "Reasons: ";
    out += verification.reasons.render();
    out += "\n";
    return out;
}

std::string ActionVerification::render() const { return render_verification(*this); }

}  // namespace thermal_governor
