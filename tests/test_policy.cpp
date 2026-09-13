// Thermal Governor — policy validation tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "framework.hpp"

#include "thermal_governor/error.hpp"
#include "thermal_governor/mitigation.hpp"
#include "thermal_governor/policy.hpp"
#include "thermal_governor/quantity.hpp"
#include "thermal_governor/thermal_state.hpp"
#include "thermal_governor/time.hpp"
#include "thermal_governor/workload.hpp"

using thermal_governor::DegreesCelsius;
using thermal_governor::ExecutionClass;
using thermal_governor::Milliseconds;
using thermal_governor::MitigationIntent;
using thermal_governor::Percent;
using thermal_governor::Status;
using thermal_governor::TemperatureDelta;
using thermal_governor::ThermalErrorCode;
using thermal_governor::ThermalPolicy;
using thermal_governor::ThermalState;
using thermal_governor::WorkloadRestriction;
using thermal_governor::WorkloadThermalProfile;
using thermal_governor::concurrency_ceiling;
using thermal_governor::policy_allows;
using thermal_governor::validate_policy;
using thermal_governor::workload_execution_class;
using thermal_governor::workload_headroom_demand;

namespace {

[[nodiscard]] ThermalPolicy defaulted() { return ThermalPolicy::make_default(); }

[[nodiscard]] WorkloadRestriction restriction_for(WorkloadThermalProfile profile) {
    WorkloadRestriction restriction;
    restriction.profile = profile;
    restriction.allowed = true;
    restriction.required_class = ExecutionClass::UNRESTRICTED;
    restriction.extra_headroom_demand = TemperatureDelta{0.0};
    return restriction;
}

}  // namespace

TG_CASE(policy, default_policy_is_accepted) {
    const ThermalPolicy policy = defaulted();
    TG_STATUS_OK(validate_policy(policy));
    TG_CHECK(policy.thresholds.warning.is_valid());
    TG_CHECK(policy.thresholds.derating.is_valid());
    TG_CHECK(policy.thresholds.critical.is_valid());
    TG_CHECK(policy.thresholds.recovery.is_valid());
}

TG_CASE(policy, default_policy_shape_is_coherent) {
    const ThermalPolicy policy = defaulted();

    TG_PHASE("threshold ordering");
    TG_CHECK(policy.thresholds.warning < policy.thresholds.derating);
    TG_CHECK(policy.thresholds.derating < policy.thresholds.critical);
    TG_CHECK(policy.thresholds.recovery < policy.thresholds.derating);
    TG_CHECK(policy.thresholds.warning <= policy.thresholds.near_limit_start());

    TG_PHASE("intent mask excludes self-authorised no-op");
    TG_CHECK(!policy_allows(policy, MitigationIntent::NO_ACTION));
    TG_CHECK(policy_allows(policy, MitigationIntent::REQUEST_CLOCK_REDUCTION));
    TG_CHECK(policy_allows(policy, MitigationIntent::REQUEST_CONCURRENCY_REDUCTION));
    TG_CHECK(policy_allows(policy, MitigationIntent::REQUEST_CONTAINMENT));

    TG_PHASE("workload lookups");
    TG_CHECK_EQ(policy.workload_restrictions.size(), std::size_t{6});
    ExecutionClass required = ExecutionClass::UNRESTRICTED;
    TG_CHECK(workload_execution_class(policy, WorkloadThermalProfile::HIGH_THERMAL_INTENSITY, required));
    TG_CHECK(required == ExecutionClass::REDUCED_CLOCK);
    TG_CHECK_EQ(workload_headroom_demand(policy, WorkloadThermalProfile::HIGH_THERMAL_INTENSITY).value(),
                1.0);
    ExecutionClass unlisted = ExecutionClass::UNRESTRICTED;
    TG_CHECK(!workload_execution_class(policy, static_cast<WorkloadThermalProfile>(42), unlisted));
    TG_CHECK_EQ(workload_headroom_demand(policy, static_cast<WorkloadThermalProfile>(42)).value(), 0.0);

    TG_PHASE("concurrency ceilings never rise with severity");
    TG_CHECK(concurrency_ceiling(policy.concurrency, ThermalState::NORMAL) >=
             concurrency_ceiling(policy.concurrency, ThermalState::NEAR_LIMIT));
    TG_CHECK(concurrency_ceiling(policy.concurrency, ThermalState::NEAR_LIMIT) >=
             concurrency_ceiling(policy.concurrency, ThermalState::DERATED));
    TG_CHECK(concurrency_ceiling(policy.concurrency, ThermalState::DERATED) >=
             concurrency_ceiling(policy.concurrency, ThermalState::CRITICAL));
    TG_CHECK_EQ(concurrency_ceiling(policy.concurrency, ThermalState::CRITICAL).value(), 0.0);
}

TG_CASE(policy, rejects_unrepresentable_thresholds) {
    auto policy = defaulted();
    policy.thresholds.warning = DegreesCelsius{-400.0};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.thresholds.critical = DegreesCelsius{5000.0};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);
}

TG_CASE(policy, rejects_warning_at_or_above_derating) {
    auto policy = defaulted();
    policy.thresholds.warning = policy.thresholds.derating;
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.thresholds.warning = DegreesCelsius{85.0};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);
}

TG_CASE(policy, rejects_derating_at_or_above_critical) {
    auto policy = defaulted();
    policy.thresholds.critical = policy.thresholds.derating;
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.thresholds.critical = DegreesCelsius{79.0};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);
}

TG_CASE(policy, rejects_recovery_at_or_above_derating) {
    auto policy = defaulted();
    policy.thresholds.recovery = policy.thresholds.derating;
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.thresholds.recovery = DegreesCelsius{85.0};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);
}

TG_CASE(policy, rejects_non_positive_near_limit_band) {
    auto policy = defaulted();
    policy.thresholds.near_limit_band = TemperatureDelta{0.0};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.thresholds.near_limit_band = TemperatureDelta{-0.25};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);
}

TG_CASE(policy, rejects_near_limit_start_below_warning) {
    auto policy = defaulted();
    policy.thresholds.near_limit_band = TemperatureDelta{5.25};
    TG_CHECK(policy.thresholds.near_limit_start() < policy.thresholds.warning);
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.thresholds.warning = DegreesCelsius{79.0};
    TG_CHECK(policy.thresholds.near_limit_start() < policy.thresholds.warning);
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);
}

TG_CASE(policy, rejects_negative_margins) {
    auto policy = defaulted();
    policy.margins.policy_safety_margin = TemperatureDelta{-0.25};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.margins.uncertainty_margin = TemperatureDelta{-1.0};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.margins.recovery_margin = TemperatureDelta{-0.5};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.margins.policy_safety_margin = TemperatureDelta{0.0};
    policy.margins.uncertainty_margin = TemperatureDelta{0.0};
    policy.margins.recovery_margin = TemperatureDelta{0.0};
    TG_STATUS_OK(validate_policy(policy));
}

TG_CASE(policy, rejects_zero_required_consecutive_samples) {
    auto policy = defaulted();
    policy.freshness.required_consecutive_samples = 0;
    policy.freshness.min_distinct_witnesses = 0;
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);
}

TG_CASE(policy, rejects_negative_freshness_windows) {
    auto policy = defaulted();
    policy.freshness.max_age = Milliseconds{-1};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.freshness.min_evidence_span = Milliseconds{-1000};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.freshness.max_age = Milliseconds{0};
    policy.freshness.min_evidence_span = Milliseconds{0};
    TG_STATUS_OK(validate_policy(policy));
}

TG_CASE(policy, rejects_distinct_witnesses_above_required_samples) {
    auto policy = defaulted();
    policy.freshness.required_consecutive_samples = 3;
    policy.freshness.min_distinct_witnesses = 4;
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.freshness.required_consecutive_samples = 2;
    policy.freshness.min_distinct_witnesses = 2;
    TG_STATUS_OK(validate_policy(policy));
}

TG_CASE(policy, rejects_negative_improvement_epsilon) {
    auto policy = defaulted();
    policy.verification.improvement_epsilon = TemperatureDelta{-0.25};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.verification.improvement_epsilon = TemperatureDelta{0.0};
    TG_STATUS_OK(validate_policy(policy));
}

TG_CASE(policy, rejects_negative_admission_headroom) {
    auto policy = defaulted();
    policy.admission.min_effective_headroom = TemperatureDelta{-0.25};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.admission.min_effective_headroom = TemperatureDelta{0.0};
    TG_STATUS_OK(validate_policy(policy));
}

TG_CASE(policy, rejects_out_of_range_concurrency_percentages) {
    auto policy = defaulted();
    policy.concurrency.normal = Percent{100.25};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.concurrency.critical = Percent{-0.25};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.concurrency.derated = Percent{1000.0};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);
}

TG_CASE(policy, rejects_non_monotonic_concurrency_ceilings) {
    auto policy = defaulted();
    policy.concurrency.normal = Percent{60.0};
    policy.concurrency.warm = Percent{70.0};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.concurrency.near_limit = Percent{80.0};
    policy.concurrency.derated = Percent{90.0};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.concurrency.normal = Percent{100.0};
    policy.concurrency.warm = Percent{100.0};
    policy.concurrency.near_limit = Percent{100.0};
    policy.concurrency.derated = Percent{100.0};
    policy.concurrency.critical = Percent{100.0};
    TG_STATUS_OK(validate_policy(policy));
}

TG_CASE(policy, rejects_propagation_depth_above_bound) {
    auto policy = defaulted();
    policy.coupling.max_propagation_depth = 9;
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_LIMIT_EXCEEDED);

    policy = defaulted();
    policy.coupling.max_propagation_depth = 8;
    TG_STATUS_OK(validate_policy(policy));
}

TG_CASE(policy, rejects_undefined_intent_bits) {
    auto policy = defaulted();
    policy.allowed_intents |= (1U << 12);
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.allowed_intents |= (1U << 31);
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.allowed_intents = 0;
    TG_STATUS_OK(validate_policy(policy));
}

TG_CASE(policy, rejects_duplicate_workload_profiles) {
    auto policy = defaulted();
    policy.workload_restrictions.push_back(
        restriction_for(WorkloadThermalProfile::HIGH_THERMAL_INTENSITY));
    TG_CHECK(policy.workload_restrictions.size() > std::size_t{6});
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);
}

TG_CASE(policy, rejects_negative_workload_headroom_demand) {
    auto policy = defaulted();
    policy.workload_restrictions.front().extra_headroom_demand = TemperatureDelta{-1.0};
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_INVALID);

    policy = defaulted();
    policy.workload_restrictions.front().extra_headroom_demand = TemperatureDelta{0.0};
    TG_STATUS_OK(validate_policy(policy));
}

TG_CASE(policy, rejects_more_than_sixty_four_workload_restrictions) {
    const WorkloadThermalProfile profiles[] = {
        WorkloadThermalProfile::UNKNOWN_PROFILE,
        WorkloadThermalProfile::LOW_THERMAL_INTENSITY,
        WorkloadThermalProfile::MODERATE_THERMAL_INTENSITY,
        WorkloadThermalProfile::HIGH_THERMAL_INTENSITY,
        WorkloadThermalProfile::BURSTY_THERMAL_PROFILE,
        WorkloadThermalProfile::CUSTOM,
    };
    constexpr std::size_t kProfileCount = sizeof(profiles) / sizeof(profiles[0]);

    auto policy = defaulted();
    policy.workload_restrictions.clear();
    for (std::size_t index = 0; index < 65; ++index) {
        policy.workload_restrictions.push_back(restriction_for(profiles[index % kProfileCount]));
    }
    TG_CHECK(policy.workload_restrictions.size() > std::size_t{64});
    TG_STATUS_ERROR_CODE(validate_policy(policy), ThermalErrorCode::POLICY_LIMIT_EXCEEDED);
}
