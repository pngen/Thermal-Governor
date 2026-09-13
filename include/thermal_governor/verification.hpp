// Thermal Governor — fresh post-action verification.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_VERIFICATION_HPP
#define THERMAL_GOVERNOR_VERIFICATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "thermal_governor/action.hpp"
#include "thermal_governor/decision.hpp"
#include "thermal_governor/evaluation.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/time.hpp"

namespace thermal_governor {

/// Outcome of post-action verification. Fresh evidence is mandatory.
enum class VerificationOutcome : std::uint8_t {
    OUTCOME_UNKNOWN = 0,
    THERMAL_STATE_IMPROVED,
    THERMAL_STATE_UNCHANGED,
    THERMAL_STATE_WORSENED,
    DERATING_EFFECTIVE,
    DERATING_PARTIALLY_EFFECTIVE,
    DERATING_INEFFECTIVE,
    SECONDARY_VIOLATION_CREATED,
    RECOVERY_ALLOWED,
    RECOVERY_FORBIDDEN,
};

[[nodiscard]] constexpr std::string_view to_string(VerificationOutcome o) noexcept {
    switch (o) {
        case VerificationOutcome::OUTCOME_UNKNOWN: return "OUTCOME_UNKNOWN";
        case VerificationOutcome::THERMAL_STATE_IMPROVED: return "THERMAL_STATE_IMPROVED";
        case VerificationOutcome::THERMAL_STATE_UNCHANGED: return "THERMAL_STATE_UNCHANGED";
        case VerificationOutcome::THERMAL_STATE_WORSENED: return "THERMAL_STATE_WORSENED";
        case VerificationOutcome::DERATING_EFFECTIVE: return "DERATING_EFFECTIVE";
        case VerificationOutcome::DERATING_PARTIALLY_EFFECTIVE: return "DERATING_PARTIALLY_EFFECTIVE";
        case VerificationOutcome::DERATING_INEFFECTIVE: return "DERATING_INEFFECTIVE";
        case VerificationOutcome::SECONDARY_VIOLATION_CREATED: return "SECONDARY_VIOLATION_CREATED";
        case VerificationOutcome::RECOVERY_ALLOWED: return "RECOVERY_ALLOWED";
        case VerificationOutcome::RECOVERY_FORBIDDEN: return "RECOVERY_FORBIDDEN";
    }
    return "UNRECOGNISED_VERIFICATION_OUTCOME";
}

/// Inputs to a post-action verification.
struct VerificationInput {
    /// The action as currently recorded by the runtime.
    const ThermalAction* action = nullptr;

    /// Evaluation captured when the action was planned.
    ThermalEvaluation before;
    /// Evaluation recomputed from fresh evidence.
    ThermalEvaluation after;

    /// True only when the recomputation consumed evidence that arrived after
    /// the dispatch instant and satisfies the policy freshness window.
    bool fresh_evidence = false;
    FreshnessState freshness = FreshnessState::UNKNOWN;

    /// Current authoritative generations, used to reject stale verification.
    AuthoritySnapshot current;
    /// Policy generation the verification is computed against.
    ThermalPolicyGeneration verification_policy_generation{};

    /// True when the post-action evaluation reports a hard constraint that
    /// was not violated before the action.
    bool secondary_hard_violation = false;

    /// Left-hand side of the "before" evaluation, allowing an exact
    /// improvement epsilon comparison.
    SteadyTimePoint now{};
};

/// The verified result of a mitigation action.
struct ActionVerification {
    ActionId action_id{};
    VerificationGeneration generation{};
    VerificationOutcome outcome = VerificationOutcome::OUTCOME_UNKNOWN;

    DegreesCelsius temperature_before{};
    DegreesCelsius temperature_after{};
    TemperatureDelta delta{};

    TemperatureDelta headroom_before{};
    TemperatureDelta headroom_after{};

    ThermalState state_before = ThermalState::UNKNOWN;
    ThermalState state_after = ThermalState::UNKNOWN;

    DeratingLevel derating_before = DeratingLevel::UNKNOWN;
    DeratingLevel derating_after = DeratingLevel::UNKNOWN;

    ActionLifecycle resulting_lifecycle = ActionLifecycle::OUTCOME_UNKNOWN;

    bool hard_constraint_still_violated = false;
    bool derating_still_required = false;
    bool recovery_conditions_satisfied = false;

    TelemetryGeneration telemetry_generation{};
    ThermalPolicyGeneration policy_generation{};
    CoordinatorEpoch coordinator_epoch{};

    ReasonCodeSet reasons;

    [[nodiscard]] std::string render() const;
};

/// Verify a mitigation action against fresh thermal evidence.
///
/// Rejects the verification outright when the action is stale, when the
/// evidence is not fresh, or when the claim would mutate superseded state.
[[nodiscard]] Result<ActionVerification> verify_action(const VerificationInput& input);

/// Canonical rendering of a verification record.
[[nodiscard]] std::string render_verification(const ActionVerification& verification);

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_VERIFICATION_HPP
