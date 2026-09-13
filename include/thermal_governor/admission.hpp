// Thermal Governor — thermal admission eligibility.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_ADMISSION_HPP
#define THERMAL_GOVERNOR_ADMISSION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "thermal_governor/decision.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/mitigation.hpp"
#include "thermal_governor/quantity.hpp"
#include "thermal_governor/workload.hpp"

namespace thermal_governor {

/// Thermal admission outcome.
///
/// This is thermal eligibility only. It is a typed thermal constraint
/// returned to the caller, not a scheduler admission decision.
enum class AdmissionDecision : std::uint8_t {
    ADMIT = 0,
    ADMIT_DERATED,
    DEFER,
    DENY,
    REVALIDATION_REQUIRED,
    UNKNOWN,
    UNSUPPORTED,
};

[[nodiscard]] constexpr std::string_view to_string(AdmissionDecision d) noexcept {
    switch (d) {
        case AdmissionDecision::ADMIT: return "ADMIT";
        case AdmissionDecision::ADMIT_DERATED: return "ADMIT_DERATED";
        case AdmissionDecision::DEFER: return "DEFER";
        case AdmissionDecision::DENY: return "DENY";
        case AdmissionDecision::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
        case AdmissionDecision::UNKNOWN: return "UNKNOWN";
        case AdmissionDecision::UNSUPPORTED: return "UNSUPPORTED";
    }
    return "UNRECOGNISED_ADMISSION_DECISION";
}

/// Caller-supplied thermal admission request.
struct ThermalAdmissionRequest {
    ThermalDomainId domain{};
    ThermalDomainGeneration expected_domain_generation{};

    WorkloadId workload{};
    WorkloadThermalProfile profile = WorkloadThermalProfile::UNKNOWN_PROFILE;
    ExecutionClass requested_class = ExecutionClass::UNRESTRICTED;

    std::optional<Percent> requested_concurrency;
    std::optional<MegaHertz> requested_clock;

    /// Generation fence: the caller states which policy generation it is
    /// reasoning under. A mismatch is a stale-policy rejection.
    std::optional<ThermalPolicyGeneration> expected_policy_generation;
    std::optional<CoordinatorEpoch> expected_coordinator_epoch;
};

/// Typed thermal constraint returned to the caller.
struct ThermalAdmissionResult {
    AdmissionDecision decision = AdmissionDecision::UNKNOWN;

    ThermalDomainId domain{};
    ThermalDomainGeneration domain_generation{};
    WorkloadId workload{};
    WorkloadThermalProfile profile = WorkloadThermalProfile::UNKNOWN_PROFILE;

    ExecutionClass permitted_class = ExecutionClass::UNRESTRICTED;
    Percent permitted_concurrency{0.0};
    std::optional<MegaHertz> permitted_clock_ceiling;

    DegreesCelsius governing_limit{};
    DegreesCelsius current_temperature{};
    TemperatureDelta effective_headroom{};
    TemperatureDelta required_headroom{};

    DeratingLevel derating = DeratingLevel::UNKNOWN;
    ThermalState state = ThermalState::UNKNOWN;

    ReasonCodeSet reasons;
    MitigationIntentSet constraints;

    // Authority binding of the answer itself.
    CoordinatorEpoch coordinator_epoch{};
    ThermalPolicyGeneration policy_generation{};
    TelemetryGeneration telemetry_generation{};
    CapabilityGeneration capability_generation{};
    TopologyGeneration topology_generation{};

    [[nodiscard]] bool admissible() const noexcept {
        return decision == AdmissionDecision::ADMIT ||
               decision == AdmissionDecision::ADMIT_DERATED;
    }
};

/// Canonical rendering of an admission result. Field order is stable.
[[nodiscard]] std::string render_admission(const ThermalAdmissionResult& result);

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_ADMISSION_HPP
