// Thermal Governor — policy validation and rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/policy.hpp"

#include "format.hpp"
#include "thermal_governor/decision.hpp"

#include <algorithm>

namespace thermal_governor {
namespace {

constexpr std::uint32_t kAllIntentsMask = (1U << kMitigationIntentCount) - 1U;

[[nodiscard]] Status invalid(const char* detail) {
    return Status::failure(ThermalErrorCode::POLICY_INVALID, detail);
}

}  // namespace

ThermalPolicy ThermalPolicy::make_default() {
    ThermalPolicy policy;
    policy.label = Label{"default"};

    // Control-plane intents Thermal Governor may legitimately request.
    // Power reduce/restore intents are emitted only as typed requests to an
    // adjacent power owner; Thermal Governor never manages a power budget.
    policy.allowed_intents = kAllIntentsMask;
    policy.allowed_intents &= ~intent_bit(MitigationIntent::NO_ACTION);

    policy.workload_restrictions = {
        WorkloadRestriction{WorkloadThermalProfile::LOW_THERMAL_INTENSITY, true,
                            ExecutionClass::UNRESTRICTED, TemperatureDelta{0.0}},
        WorkloadRestriction{WorkloadThermalProfile::MODERATE_THERMAL_INTENSITY, true,
                            ExecutionClass::UNRESTRICTED, TemperatureDelta{0.0}},
        WorkloadRestriction{WorkloadThermalProfile::HIGH_THERMAL_INTENSITY, true,
                            ExecutionClass::REDUCED_CLOCK, TemperatureDelta{1.0}},
        WorkloadRestriction{WorkloadThermalProfile::BURSTY_THERMAL_PROFILE, true,
                            ExecutionClass::REDUCED_CONCURRENCY, TemperatureDelta{1.0}},
        WorkloadRestriction{WorkloadThermalProfile::UNKNOWN_PROFILE, true,
                            ExecutionClass::REDUCED_CONCURRENCY, TemperatureDelta{2.0}},
        WorkloadRestriction{WorkloadThermalProfile::CUSTOM, true,
                            ExecutionClass::UNRESTRICTED, TemperatureDelta{0.0}},
    };
    return policy;
}

Status validate_policy(const ThermalPolicy& policy) {
    const auto& thresholds = policy.thresholds;

    if (!thresholds.warning.is_valid() || !thresholds.derating.is_valid() ||
        !thresholds.critical.is_valid() || !thresholds.recovery.is_valid()) {
        return invalid("threshold values outside the representable range");
    }
    if (!(thresholds.warning.value() < thresholds.derating.value())) {
        return invalid("warning threshold must be strictly below the derating threshold");
    }
    if (!(thresholds.derating.value() < thresholds.critical.value())) {
        return invalid("derating threshold must be strictly below the critical threshold");
    }
    if (!(thresholds.recovery.value() < thresholds.derating.value())) {
        return invalid("recovery threshold must be strictly below the derating threshold");
    }
    if (!(thresholds.near_limit_band.value() > 0.0)) {
        return invalid("near-limit band must be strictly positive");
    }
    if (!(thresholds.near_limit_start().value() >= thresholds.warning.value())) {
        return invalid("near-limit band places NEAR_LIMIT below the warning threshold");
    }

    if (policy.margins.policy_safety_margin.value() < 0.0) {
        return invalid("policy safety margin cannot be negative");
    }
    if (policy.margins.uncertainty_margin.value() < 0.0) {
        return invalid("uncertainty margin cannot be negative");
    }
    if (policy.margins.recovery_margin.value() < 0.0) {
        return invalid("recovery margin cannot be negative");
    }

    if (policy.freshness.required_consecutive_samples == 0) {
        return invalid("at least one consecutive recovery sample must be required");
    }
    if (policy.freshness.max_age.count() < 0) {
        return invalid("evidence maximum age cannot be negative");
    }
    if (policy.freshness.min_evidence_span.count() < 0) {
        return invalid("minimum evidence span cannot be negative");
    }
    if (policy.freshness.min_distinct_witnesses > policy.freshness.required_consecutive_samples) {
        return invalid("required distinct witnesses cannot exceed the consecutive sample count");
    }

    if (policy.verification.improvement_epsilon.value() < 0.0) {
        return invalid("verification improvement epsilon cannot be negative");
    }
    if (policy.admission.min_effective_headroom.value() < 0.0) {
        return invalid("admission minimum effective headroom cannot be negative");
    }

    const auto& concurrency = policy.concurrency;
    const auto within = [](Percent p) {
        return p.value() >= 0.0 && p.value() <= 100.0;
    };
    if (!within(concurrency.normal) || !within(concurrency.warm) ||
        !within(concurrency.near_limit) || !within(concurrency.derated) ||
        !within(concurrency.critical)) {
        return invalid("concurrency ceilings must lie in [0, 100]");
    }
    if (!(concurrency.normal.value() >= concurrency.warm.value() &&
          concurrency.warm.value() >= concurrency.near_limit.value() &&
          concurrency.near_limit.value() >= concurrency.derated.value() &&
          concurrency.derated.value() >= concurrency.critical.value())) {
        return invalid("concurrency ceilings must be monotonically non-increasing with severity");
    }

    if (policy.coupling.max_propagation_depth > 8) {
        return Status::failure(ThermalErrorCode::POLICY_LIMIT_EXCEEDED,
                               "coupling propagation depth above the supported bound");
    }
    if ((policy.allowed_intents & ~kAllIntentsMask) != 0U) {
        return invalid("allowed intent mask contains undefined intent bits");
    }

    // Resource bounds are checked before content, so an oversized list is
    // reported as a limit breach rather than as whichever content defect
    // happens to appear first.
    if (policy.workload_restrictions.size() > 64) {
        return Status::failure(ThermalErrorCode::POLICY_LIMIT_EXCEEDED,
                               "too many workload restrictions");
    }

    // Duplicate workload profiles make the effective restriction ambiguous.
    std::vector<WorkloadThermalProfile> seen;
    for (const auto& restriction : policy.workload_restrictions) {
        if (std::find(seen.begin(), seen.end(), restriction.profile) != seen.end()) {
            return invalid("duplicate workload thermal profile restriction");
        }
        seen.push_back(restriction.profile);
        if (restriction.extra_headroom_demand.value() < 0.0) {
            return invalid("workload extra headroom demand cannot be negative");
        }
    }

    return Status::success();
}

TemperatureDelta workload_headroom_demand(const ThermalPolicy& policy,
                                          WorkloadThermalProfile profile) noexcept {
    for (const auto& restriction : policy.workload_restrictions) {
        if (restriction.profile == profile) {
            return restriction.extra_headroom_demand;
        }
    }
    return TemperatureDelta{0.0};
}

bool workload_execution_class(const ThermalPolicy& policy, WorkloadThermalProfile profile,
                              ExecutionClass& out_class) noexcept {
    for (const auto& restriction : policy.workload_restrictions) {
        if (restriction.profile == profile) {
            if (!restriction.allowed) {
                return false;
            }
            out_class = restriction.required_class;
            return true;
        }
    }
    // An unlisted profile is not silently unrestricted.
    return false;
}

std::string render_policy(const ThermalPolicy& policy) {
    std::string out;
    out += "Policy: ";
    out += policy.label.empty() ? std::string("<unlabelled>") : policy.label.text();
    out += "\n";
    out += "ThermalPolicyId: " + detail::unsigned_decimal(policy.id.value()) + "\n";
    out += "ThermalPolicyGeneration: " + detail::unsigned_decimal(policy.generation.value()) + "\n";
    out += "Warning: " + detail::temperature(policy.thresholds.warning.value()) + "\n";
    out += "NearLimitStart: " +
           detail::temperature(policy.thresholds.near_limit_start().value()) + "\n";
    out += "Derating: " + detail::temperature(policy.thresholds.derating.value()) + "\n";
    out += "Critical: " + detail::temperature(policy.thresholds.critical.value()) + "\n";
    out += "Recovery: " + detail::temperature(policy.thresholds.recovery.value()) + "\n";
    out += "HysteresisWidth: " +
           detail::number(policy.thresholds.hysteresis_width().value()) + "\n";
    out += "SafetyMargin: " + detail::number(policy.margins.policy_safety_margin.value()) + "\n";
    out += "UncertaintyMargin: " + detail::number(policy.margins.uncertainty_margin.value()) + "\n";
    out += "RecoveryMargin: " + detail::number(policy.margins.recovery_margin.value()) + "\n";
    out += "MaxEvidenceAgeMs: " + detail::millis(policy.freshness.max_age.count()) + "\n";
    out += "RequiredRecoverySamples: " +
           detail::unsigned_decimal(policy.freshness.required_consecutive_samples) + "\n";
    out += "MinEvidenceSpanMs: " + detail::millis(policy.freshness.min_evidence_span.count()) + "\n";
    out += "UnknownBehavior: ";
    out += to_string(policy.unknown_behavior);
    out += "\n";
    out += "UnsupportedBehavior: ";
    out += to_string(policy.unsupported_behavior);
    out += "\n";
    out += "CoDerateCoupledDomains: ";
    out += policy.coupling.co_derate_coupled_domains ? "true" : "false";
    out += "\n";
    out += "MaxPropagationDepth: " +
           detail::unsigned_decimal(policy.coupling.max_propagation_depth) + "\n";
    out += "RequireCoupledRecovery: ";
    out += policy.coupling.require_coupled_recovery ? "true" : "false";
    out += "\n";

    out += "AllowedIntents:";
    bool any = false;
    for (std::uint32_t i = 0; i < kMitigationIntentCount; ++i) {
        const auto intent = static_cast<MitigationIntent>(i);
        if (intent == MitigationIntent::NO_ACTION) {
            continue;
        }
        if ((policy.allowed_intents & intent_bit(intent)) != 0U) {
            out += " ";
            out += to_string(intent);
            any = true;
        }
    }
    if (!any) {
        out += " <none>";
    }
    out += "\n";

    out += "WorkloadRestrictions:";
    if (policy.workload_restrictions.empty()) {
        out += " <none>";
    }
    out += "\n";
    for (const auto& restriction : policy.workload_restrictions) {
        out += "  ";
        out += to_string(restriction.profile);
        out += " allowed=";
        out += restriction.allowed ? "true" : "false";
        out += " class=";
        out += to_string(restriction.required_class);
        out += " extraHeadroom=";
        out += detail::number(restriction.extra_headroom_demand.value());
        out += "\n";
    }
    return out;
}

}  // namespace thermal_governor
