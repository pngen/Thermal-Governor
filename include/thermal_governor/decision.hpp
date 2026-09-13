// Thermal Governor — typed thermal decisions and reason codes.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_DECISION_HPP
#define THERMAL_GOVERNOR_DECISION_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "thermal_governor/identity.hpp"
#include "thermal_governor/thermal_state.hpp"

namespace thermal_governor {

/// Possible deterministic thermal decisions. Never a bool.
enum class ThermalDecision : std::uint8_t {
    ALLOW = 0,
    ALLOW_DERATED,
    DEFER,
    DENY,
    REVALIDATION_REQUIRED,
    UNKNOWN,
    UNSUPPORTED,
};

[[nodiscard]] constexpr std::string_view to_string(ThermalDecision d) noexcept {
    switch (d) {
        case ThermalDecision::ALLOW: return "ALLOW";
        case ThermalDecision::ALLOW_DERATED: return "ALLOW_DERATED";
        case ThermalDecision::DEFER: return "DEFER";
        case ThermalDecision::DENY: return "DENY";
        case ThermalDecision::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
        case ThermalDecision::UNKNOWN: return "UNKNOWN";
        case ThermalDecision::UNSUPPORTED: return "UNSUPPORTED";
    }
    return "UNRECOGNISED_DECISION";
}

/// Explicit derating forms. DERATED and THROTTLING are not collapsed, and
/// a derating decision can never expand authority.
enum class DeratingLevel : std::uint8_t {
    FULL_CAPABILITY = 0,
    DERATED_CLOCK,
    DERATED_ADMISSION,
    DERATED_CONCURRENCY,
    DERATED_WORKLOAD_CLASS,
    DERATED_COMBINED,
    NO_SAFE_EXECUTION,
    REVALIDATION_REQUIRED,
    UNKNOWN,
    UNSUPPORTED,
};

[[nodiscard]] constexpr std::string_view to_string(DeratingLevel l) noexcept {
    switch (l) {
        case DeratingLevel::FULL_CAPABILITY: return "FULL_CAPABILITY";
        case DeratingLevel::DERATED_CLOCK: return "DERATED_CLOCK";
        case DeratingLevel::DERATED_ADMISSION: return "DERATED_ADMISSION";
        case DeratingLevel::DERATED_CONCURRENCY: return "DERATED_CONCURRENCY";
        case DeratingLevel::DERATED_WORKLOAD_CLASS: return "DERATED_WORKLOAD_CLASS";
        case DeratingLevel::DERATED_COMBINED: return "DERATED_COMBINED";
        case DeratingLevel::NO_SAFE_EXECUTION: return "NO_SAFE_EXECUTION";
        case DeratingLevel::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
        case DeratingLevel::UNKNOWN: return "UNKNOWN";
        case DeratingLevel::UNSUPPORTED: return "UNSUPPORTED";
    }
    return "UNRECOGNISED_DERATING_LEVEL";
}

/// Restriction severity rank. Higher means more restrictive.
[[nodiscard]] constexpr std::uint32_t restriction_rank(DeratingLevel l) noexcept {
    switch (l) {
        case DeratingLevel::FULL_CAPABILITY: return 0;
        case DeratingLevel::DERATED_WORKLOAD_CLASS: return 1;
        case DeratingLevel::DERATED_CLOCK: return 2;
        case DeratingLevel::DERATED_ADMISSION: return 3;
        case DeratingLevel::DERATED_CONCURRENCY: return 4;
        case DeratingLevel::DERATED_COMBINED: return 5;
        case DeratingLevel::NO_SAFE_EXECUTION: return 6;
        case DeratingLevel::REVALIDATION_REQUIRED: return 7;
        case DeratingLevel::UNKNOWN: return 8;
        case DeratingLevel::UNSUPPORTED: return 9;
    }
    return 10;
}

/// True when the candidate level is at most as permissive as the previous
/// level. Derating cannot expand capability.
[[nodiscard]] constexpr bool does_not_expand(DeratingLevel candidate,
                                             DeratingLevel previous) noexcept {
    return restriction_rank(candidate) >= restriction_rank(previous);
}

/// The more restrictive of two levels.
[[nodiscard]] constexpr DeratingLevel most_restrictive(DeratingLevel a,
                                                       DeratingLevel b) noexcept {
    return restriction_rank(a) >= restriction_rank(b) ? a : b;
}

/// Stable, machine-readable reason codes attached to every decision.
enum class ThermalReasonCode : std::uint16_t {
    NONE = 0,

    TEMPERATURE_BELOW_WARNING = 1,
    TEMPERATURE_IN_WARNING_BAND = 2,
    TEMPERATURE_IN_NEAR_LIMIT_BAND = 3,
    DERATING_THRESHOLD_EXCEEDED = 4,
    CRITICAL_THRESHOLD_EXCEEDED = 5,

    THERMAL_THROTTLE_OBSERVED = 10,
    NON_THERMAL_THROTTLE_OBSERVED = 11,
    NO_THROTTLE_OBSERVED = 12,
    THROTTLE_EVIDENCE_UNSUPPORTED = 13,
    THROTTLE_EVIDENCE_UNKNOWN = 14,

    EVIDENCE_MISSING = 20,
    EVIDENCE_STALE = 21,
    EVIDENCE_GENERATION_INVALIDATED = 22,
    EVIDENCE_INTEGRITY_FAILED = 23,
    EVIDENCE_CONFLICT = 24,
    CAPABILITY_UNSUPPORTED = 25,
    POLICY_UNKNOWN_BEHAVIOR_APPLIED = 26,
    POLICY_UNSUPPORTED_BEHAVIOR_APPLIED = 27,
    CAPABILITY_UNRESOLVED = 28,

    EFFECTIVE_HEADROOM_SUFFICIENT = 30,
    EFFECTIVE_HEADROOM_BELOW_DEMAND = 31,
    RAW_HEADROOM_NEGATIVE = 32,
    VENDOR_LIMIT_GOVERNS = 33,
    POLICY_LIMIT_GOVERNS = 34,

    RECOVERY_THRESHOLD_SATISFIED = 40,
    RECOVERY_THRESHOLD_EXCEEDED = 41,
    RECOVERY_SAMPLES_INSUFFICIENT = 42,
    RECOVERY_EVIDENCE_SPAN_INSUFFICIENT = 43,
    RECOVERY_FORBIDDEN_BY_POLICY = 44,
    RECOVERY_REQUIRES_EXPLICIT_AUTHORIZATION = 45,
    RECOVERY_AUTHORIZED = 46,
    RECOVERY_WITNESSES_INSUFFICIENT = 47,
    RECOVERY_BLOCKED_BY_COUPLED_DOMAIN = 48,
    RECOVERY_BLOCKED_BY_THROTTLE_EVIDENCE = 49,

    COUPLED_DOMAIN_PRESSURE = 60,
    COUPLED_DOMAIN_RECOVERED = 61,
    COUPLING_PROVENANCE_SYNTHETIC = 62,

    WORKLOAD_PROFILE_EXCLUDED = 70,
    WORKLOAD_REQUIRES_DERATING = 71,
    WORKLOAD_HEADROOM_DEMAND_MET = 72,
    WORKLOAD_PROFILE_UNKNOWN = 73,

    EMERGENCY_CONTAINMENT_REQUIRED = 80,
    EMERGENCY_EXECUTION_DENIED = 81,

    ADMISSION_DEFERRED_BY_POLICY = 90,
    DERATED_ADMISSION_ALLOWED = 91,
    DERATED_ADMISSION_FORBIDDEN = 92,
    CONCURRENCY_CEILING_APPLIED = 93,

    STATE_RETAINED_BY_HYSTERESIS = 100,
    AUTHORITY_REVALIDATED = 101,
    DERATING_CANNOT_EXPAND = 102,
};

[[nodiscard]] constexpr std::string_view to_string(ThermalReasonCode c) noexcept {
    switch (c) {
        case ThermalReasonCode::NONE: return "NONE";
        case ThermalReasonCode::TEMPERATURE_BELOW_WARNING: return "TEMPERATURE_BELOW_WARNING";
        case ThermalReasonCode::TEMPERATURE_IN_WARNING_BAND: return "TEMPERATURE_IN_WARNING_BAND";
        case ThermalReasonCode::TEMPERATURE_IN_NEAR_LIMIT_BAND: return "TEMPERATURE_IN_NEAR_LIMIT_BAND";
        case ThermalReasonCode::DERATING_THRESHOLD_EXCEEDED: return "DERATING_THRESHOLD_EXCEEDED";
        case ThermalReasonCode::CRITICAL_THRESHOLD_EXCEEDED: return "CRITICAL_THRESHOLD_EXCEEDED";
        case ThermalReasonCode::THERMAL_THROTTLE_OBSERVED: return "THERMAL_THROTTLE_OBSERVED";
        case ThermalReasonCode::NON_THERMAL_THROTTLE_OBSERVED: return "NON_THERMAL_THROTTLE_OBSERVED";
        case ThermalReasonCode::NO_THROTTLE_OBSERVED: return "NO_THROTTLE_OBSERVED";
        case ThermalReasonCode::THROTTLE_EVIDENCE_UNSUPPORTED: return "THROTTLE_EVIDENCE_UNSUPPORTED";
        case ThermalReasonCode::THROTTLE_EVIDENCE_UNKNOWN: return "THROTTLE_EVIDENCE_UNKNOWN";
        case ThermalReasonCode::EVIDENCE_MISSING: return "EVIDENCE_MISSING";
        case ThermalReasonCode::EVIDENCE_STALE: return "EVIDENCE_STALE";
        case ThermalReasonCode::EVIDENCE_GENERATION_INVALIDATED: return "EVIDENCE_GENERATION_INVALIDATED";
        case ThermalReasonCode::EVIDENCE_INTEGRITY_FAILED: return "EVIDENCE_INTEGRITY_FAILED";
        case ThermalReasonCode::EVIDENCE_CONFLICT: return "EVIDENCE_CONFLICT";
        case ThermalReasonCode::CAPABILITY_UNSUPPORTED: return "CAPABILITY_UNSUPPORTED";
        case ThermalReasonCode::POLICY_UNKNOWN_BEHAVIOR_APPLIED: return "POLICY_UNKNOWN_BEHAVIOR_APPLIED";
        case ThermalReasonCode::POLICY_UNSUPPORTED_BEHAVIOR_APPLIED: return "POLICY_UNSUPPORTED_BEHAVIOR_APPLIED";
        case ThermalReasonCode::CAPABILITY_UNRESOLVED: return "CAPABILITY_UNRESOLVED";
        case ThermalReasonCode::EFFECTIVE_HEADROOM_SUFFICIENT: return "EFFECTIVE_HEADROOM_SUFFICIENT";
        case ThermalReasonCode::EFFECTIVE_HEADROOM_BELOW_DEMAND: return "EFFECTIVE_HEADROOM_BELOW_DEMAND";
        case ThermalReasonCode::RAW_HEADROOM_NEGATIVE: return "RAW_HEADROOM_NEGATIVE";
        case ThermalReasonCode::VENDOR_LIMIT_GOVERNS: return "VENDOR_LIMIT_GOVERNS";
        case ThermalReasonCode::POLICY_LIMIT_GOVERNS: return "POLICY_LIMIT_GOVERNS";
        case ThermalReasonCode::RECOVERY_THRESHOLD_SATISFIED: return "RECOVERY_THRESHOLD_SATISFIED";
        case ThermalReasonCode::RECOVERY_THRESHOLD_EXCEEDED: return "RECOVERY_THRESHOLD_EXCEEDED";
        case ThermalReasonCode::RECOVERY_SAMPLES_INSUFFICIENT: return "RECOVERY_SAMPLES_INSUFFICIENT";
        case ThermalReasonCode::RECOVERY_EVIDENCE_SPAN_INSUFFICIENT: return "RECOVERY_EVIDENCE_SPAN_INSUFFICIENT";
        case ThermalReasonCode::RECOVERY_FORBIDDEN_BY_POLICY: return "RECOVERY_FORBIDDEN_BY_POLICY";
        case ThermalReasonCode::RECOVERY_REQUIRES_EXPLICIT_AUTHORIZATION: return "RECOVERY_REQUIRES_EXPLICIT_AUTHORIZATION";
        case ThermalReasonCode::RECOVERY_AUTHORIZED: return "RECOVERY_AUTHORIZED";
        case ThermalReasonCode::RECOVERY_WITNESSES_INSUFFICIENT: return "RECOVERY_WITNESSES_INSUFFICIENT";
        case ThermalReasonCode::RECOVERY_BLOCKED_BY_COUPLED_DOMAIN: return "RECOVERY_BLOCKED_BY_COUPLED_DOMAIN";
        case ThermalReasonCode::RECOVERY_BLOCKED_BY_THROTTLE_EVIDENCE: return "RECOVERY_BLOCKED_BY_THROTTLE_EVIDENCE";
        case ThermalReasonCode::COUPLED_DOMAIN_PRESSURE: return "COUPLED_DOMAIN_PRESSURE";
        case ThermalReasonCode::COUPLED_DOMAIN_RECOVERED: return "COUPLED_DOMAIN_RECOVERED";
        case ThermalReasonCode::COUPLING_PROVENANCE_SYNTHETIC: return "COUPLING_PROVENANCE_SYNTHETIC";
        case ThermalReasonCode::WORKLOAD_PROFILE_EXCLUDED: return "WORKLOAD_PROFILE_EXCLUDED";
        case ThermalReasonCode::WORKLOAD_REQUIRES_DERATING: return "WORKLOAD_REQUIRES_DERATING";
        case ThermalReasonCode::WORKLOAD_HEADROOM_DEMAND_MET: return "WORKLOAD_HEADROOM_DEMAND_MET";
        case ThermalReasonCode::WORKLOAD_PROFILE_UNKNOWN: return "WORKLOAD_PROFILE_UNKNOWN";
        case ThermalReasonCode::EMERGENCY_CONTAINMENT_REQUIRED: return "EMERGENCY_CONTAINMENT_REQUIRED";
        case ThermalReasonCode::EMERGENCY_EXECUTION_DENIED: return "EMERGENCY_EXECUTION_DENIED";
        case ThermalReasonCode::ADMISSION_DEFERRED_BY_POLICY: return "ADMISSION_DEFERRED_BY_POLICY";
        case ThermalReasonCode::DERATED_ADMISSION_ALLOWED: return "DERATED_ADMISSION_ALLOWED";
        case ThermalReasonCode::DERATED_ADMISSION_FORBIDDEN: return "DERATED_ADMISSION_FORBIDDEN";
        case ThermalReasonCode::CONCURRENCY_CEILING_APPLIED: return "CONCURRENCY_CEILING_APPLIED";
        case ThermalReasonCode::STATE_RETAINED_BY_HYSTERESIS: return "STATE_RETAINED_BY_HYSTERESIS";
        case ThermalReasonCode::AUTHORITY_REVALIDATED: return "AUTHORITY_REVALIDATED";
        case ThermalReasonCode::DERATING_CANNOT_EXPAND: return "DERATING_CANNOT_EXPAND";
    }
    return "UNRECOGNISED_REASON_CODE";
}

/// Canonical, ordered reason-code set.
class ReasonCodeSet {
public:
    void add(ThermalReasonCode code) {
        if (code == ThermalReasonCode::NONE) {
            return;
        }
        for (const auto existing : codes_) {
            if (existing == code) {
                return;
            }
        }
        codes_.push_back(code);
    }

    [[nodiscard]] bool contains(ThermalReasonCode code) const noexcept {
        for (const auto existing : codes_) {
            if (existing == code) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const std::vector<ThermalReasonCode>& codes() const noexcept { return codes_; }
    [[nodiscard]] bool empty() const noexcept { return codes_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return codes_.size(); }

    /// Sort by reason-code value so ordering is independent of discovery
    /// order. Insertion sort keeps the implementation allocation-free.
    void canonicalise() {
        for (std::size_t i = 1; i < codes_.size(); ++i) {
            const auto key = codes_[i];
            std::size_t j = i;
            while (j > 0 && static_cast<std::uint16_t>(codes_[j - 1]) >
                                static_cast<std::uint16_t>(key)) {
                codes_[j] = codes_[j - 1];
                --j;
            }
            codes_[j] = key;
        }
    }

    [[nodiscard]] std::string render() const;

private:
    std::vector<ThermalReasonCode> codes_;
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_DECISION_HPP
