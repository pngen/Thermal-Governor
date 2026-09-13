// Thermal Governor — deterministic thermal policy.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_POLICY_HPP
#define THERMAL_GOVERNOR_POLICY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "thermal_governor/error.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/mitigation.hpp"
#include "thermal_governor/quantity.hpp"
#include "thermal_governor/thermal_state.hpp"
#include "thermal_governor/time.hpp"
#include "thermal_governor/workload.hpp"

namespace thermal_governor {

/// Policy response when thermal evidence is absent or unreadable.
///
/// The default surfaces UNKNOWN. UNKNOWN never silently becomes SAFE.
enum class UnknownBehavior : std::uint8_t {
    RETURN_UNKNOWN = 0,
    REVALIDATION_REQUIRED,
    DEFER,
    DENY,
    ALLOW_DERATED,
};

[[nodiscard]] constexpr std::string_view to_string(UnknownBehavior b) noexcept {
    switch (b) {
        case UnknownBehavior::RETURN_UNKNOWN: return "RETURN_UNKNOWN";
        case UnknownBehavior::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
        case UnknownBehavior::DEFER: return "DEFER";
        case UnknownBehavior::DENY: return "DENY";
        case UnknownBehavior::ALLOW_DERATED: return "ALLOW_DERATED";
    }
    return "UNRECOGNISED_UNKNOWN_BEHAVIOR";
}

/// Policy response when a required capability is genuinely unsupported.
enum class UnsupportedBehavior : std::uint8_t {
    RETURN_UNSUPPORTED = 0,
    REVALIDATION_REQUIRED,
    DEFER,
    DENY,
};

[[nodiscard]] constexpr std::string_view to_string(UnsupportedBehavior b) noexcept {
    switch (b) {
        case UnsupportedBehavior::RETURN_UNSUPPORTED: return "RETURN_UNSUPPORTED";
        case UnsupportedBehavior::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
        case UnsupportedBehavior::DEFER: return "DEFER";
        case UnsupportedBehavior::DENY: return "DENY";
    }
    return "UNRECOGNISED_UNSUPPORTED_BEHAVIOR";
}

/// Ordered temperature thresholds governing a domain.
struct ThermalThresholds {
    DegreesCelsius warning{75.0};
    DegreesCelsius derating{80.0};
    DegreesCelsius critical{88.0};
    DegreesCelsius recovery{72.0};
    /// Width of the NEAR_LIMIT band immediately below the derating threshold.
    TemperatureDelta near_limit_band{3.0};

    /// Threshold at which NEAR_LIMIT begins.
    [[nodiscard]] DegreesCelsius near_limit_start() const noexcept {
        return derating - near_limit_band;
    }
    /// Hysteresis width between the derating entry point and the recovery
    /// exit point.
    [[nodiscard]] TemperatureDelta hysteresis_width() const noexcept {
        return derating - recovery;
    }
};

/// Explicit headroom margins. Usable headroom is never merely limit minus
/// current temperature.
struct ThermalMargins {
    TemperatureDelta policy_safety_margin{2.0};
    TemperatureDelta uncertainty_margin{1.0};
    /// Extra headroom that must remain above the recovery threshold before
    /// recovery may be authorised.
    TemperatureDelta recovery_margin{1.0};
};

/// Evidence freshness and recovery-sample requirements.
struct FreshnessRequirements {
    Milliseconds max_age{1000};
    /// Consecutive admissible samples required before recovery may proceed.
    std::uint32_t required_consecutive_samples{3};
    /// Minimum wall span that the qualifying samples must cover.
    Milliseconds min_evidence_span{2000};
    /// Distinct worker incarnations that must have contributed qualifying
    /// samples. Zero means the requirement does not apply.
    std::uint32_t min_distinct_witnesses{0};
};

/// Rules governing thermal coupling and propagation.
struct CouplingPolicy {
    bool co_derate_coupled_domains = true;
    /// How far thermal pressure may propagate along the coupling graph.
    std::uint32_t max_propagation_depth = 2;
    /// Coupled domains must independently satisfy their recovery policy
    /// before this domain may recover.
    bool require_coupled_recovery = true;
    /// Mirror the hottest coupled domain state instead of only derating.
    bool escalate_to_hottest_neighbour = false;
};

/// A per-workload-class restriction.
struct WorkloadRestriction {
    WorkloadThermalProfile profile = WorkloadThermalProfile::UNKNOWN_PROFILE;
    bool allowed = true;
    ExecutionClass required_class = ExecutionClass::UNRESTRICTED;
    /// Extra effective-headroom demand imposed on this workload class.
    TemperatureDelta extra_headroom_demand{0.0};
};

/// Thermal admission rules.
struct AdmissionPolicy {
    TemperatureDelta min_effective_headroom{2.0};
    bool allow_derated_admission = true;
    bool allow_defer = true;
};

/// Concurrency ceilings per thermal state, as percentages.
struct ConcurrencyPolicy {
    Percent normal{100.0};
    Percent warm{100.0};
    Percent near_limit{75.0};
    Percent derated{50.0};
    Percent critical{0.0};
};

/// Concurrency ceiling implied by a thermal state under a concurrency policy.
[[nodiscard]] constexpr Percent concurrency_ceiling(const ConcurrencyPolicy& policy,
                                                    ThermalState state) noexcept {
    switch (state) {
        case ThermalState::NORMAL: return policy.normal;
        case ThermalState::WARM: return policy.warm;
        case ThermalState::NEAR_LIMIT: return policy.near_limit;
        case ThermalState::DERATED: return policy.derated;
        case ThermalState::THROTTLING: return policy.derated;
        case ThermalState::CRITICAL: return policy.critical;
        case ThermalState::REVALIDATION_REQUIRED: return policy.critical;
        case ThermalState::UNKNOWN: return policy.critical;
        case ThermalState::UNSUPPORTED: return policy.critical;
    }
    return policy.critical;
}

/// Policy for thermal emergencies.
struct EmergencyPolicy {
    /// CRITICAL denies execution outright unless policy says otherwise.
    bool deny_all_execution = true;
    bool allow_containment_intent = true;
    bool allow_migration_intent = false;
    bool allow_cooling_intervention_intent = false;
};

/// Post-action verification requirements.
struct VerificationPolicy {
    bool required = true;
    /// Minimum temperature improvement, in degrees, for a mitigation to
    /// count as having improved the thermal state.
    TemperatureDelta improvement_epsilon{0.5};
    /// Additional samples required beyond the first verification reading.
    std::uint32_t required_samples{1};
};

/// Recovery gating requirements. Recovery is never implicit.
struct RecoveryPolicy {
    bool require_no_thermal_throttle = true;
    bool require_headroom_above_recovery_margin = true;
    bool require_coupled_domains_recovered = true;
    bool require_same_worker_boot = true;
    bool require_same_coordinator_epoch = true;
    bool require_same_device_generation = true;
    bool require_explicit_authorization = false;
    bool forbid_recovery_after_unsupported_evidence = true;
};

/// A complete, versioned thermal policy.
struct ThermalPolicy {
    ThermalPolicyId id{StrongId<ThermalPolicyIdTag>{1}};
    ThermalPolicyGeneration generation{StrongId<ThermalPolicyGenerationTag>{1}};
    Label label;

    ThermalThresholds thresholds;
    ThermalMargins margins;
    FreshnessRequirements freshness;
    RecoveryPolicy recovery;
    VerificationPolicy verification;
    AdmissionPolicy admission;
    ConcurrencyPolicy concurrency;
    EmergencyPolicy emergency;
    CouplingPolicy coupling;

    UnknownBehavior unknown_behavior = UnknownBehavior::RETURN_UNKNOWN;
    UnsupportedBehavior unsupported_behavior = UnsupportedBehavior::RETURN_UNSUPPORTED;

    /// Bitmask of permitted mitigation intents.
    std::uint32_t allowed_intents = 0;

    std::vector<WorkloadRestriction> workload_restrictions;

    /// Default policy: every intent except control-plane actions Thermal
    /// Governor must never take on its own authority.
    [[nodiscard]] static ThermalPolicy make_default();
};

/// Deterministic policy validation. Impossible threshold orderings,
/// non-finite margins and contradictory rules are rejected with a typed
/// error rather than silently clamped.
[[nodiscard]] Status validate_policy(const ThermalPolicy& policy);

/// True when the intent mask permits the given intent.
[[nodiscard]] constexpr bool policy_allows(const ThermalPolicy& p, MitigationIntent i) noexcept {
    return (p.allowed_intents & intent_bit(i)) != 0U;
}

/// Look up the effective headroom demand for a workload profile.
[[nodiscard]] TemperatureDelta workload_headroom_demand(
    const ThermalPolicy& policy, WorkloadThermalProfile profile) noexcept;

/// Look up the execution class required for a workload profile under a
/// policy. Returns false when the profile is excluded by policy.
[[nodiscard]] bool workload_execution_class(const ThermalPolicy& policy,
                                            WorkloadThermalProfile profile,
                                            ExecutionClass& out_class) noexcept;

/// Canonical rendering of a policy, used by the CLI and by snapshots.
[[nodiscard]] std::string render_policy(const ThermalPolicy& policy);

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_POLICY_HPP
