// Thermal Governor — deterministic thermal evaluation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_EVALUATION_HPP
#define THERMAL_GOVERNOR_EVALUATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "thermal_governor/capability.hpp"
#include "thermal_governor/coupling.hpp"
#include "thermal_governor/decision.hpp"
#include "thermal_governor/domain.hpp"
#include "thermal_governor/evidence.hpp"
#include "thermal_governor/headroom.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/mitigation.hpp"
#include "thermal_governor/policy.hpp"
#include "thermal_governor/provenance.hpp"
#include "thermal_governor/quantity.hpp"
#include "thermal_governor/recovery.hpp"
#include "thermal_governor/thermal_state.hpp"
#include "thermal_governor/throttle.hpp"
#include "thermal_governor/workload.hpp"

namespace thermal_governor {

/// Thermal pressure arriving from a coupled domain.
struct CoupledPressure {
    ThermalDomainId domain{};
    ThermalDomainGeneration generation{};
    ThermalState state = ThermalState::UNKNOWN;
    DegreesCelsius temperature{};
    CouplingType coupling = CouplingType::UNKNOWN_COUPLING;
    PropagationClass propagation = PropagationClass::UNKNOWN;
    Provenance provenance = Provenance::UNKNOWN;
    std::uint32_t depth = 1;
    bool recovery_eligible = false;
};

/// An alternative that was considered and rejected, with the reason.
struct RejectedAlternative {
    DeratingLevel candidate = DeratingLevel::UNKNOWN;
    ThermalReasonCode reason = ThermalReasonCode::NONE;
};

/// Complete input to a deterministic thermal evaluation.
///
/// Evaluation is a pure function of this structure: equal inputs always
/// produce byte-identical explanations.
struct EvaluationInput {
    ThermalDomainId domain{};
    ThermalDomainGeneration domain_generation{};
    ThermalDomainType domain_type = ThermalDomainType::ACCELERATOR;

    ThermalPolicy policy;
    ThermalState previous_state = ThermalState::UNKNOWN;
    DeratingLevel previous_derating = DeratingLevel::UNKNOWN;

    const ThermalEvidence* evidence = nullptr;
    FreshnessState freshness = FreshnessState::UNKNOWN;
    bool evidence_conflict = false;

    CapabilityState temperature_capability = CapabilityState::UNKNOWN;
    CapabilityState throttle_capability = CapabilityState::UNKNOWN;
    CapabilityState limit_capability = CapabilityState::UNKNOWN;

    RecoveryInput recovery;

    std::vector<CoupledPressure> coupled;

    WorkloadThermalProfile workload_profile = WorkloadThermalProfile::UNKNOWN_PROFILE;

    CoordinatorEpoch coordinator_epoch{};
    ThermalPolicyGeneration policy_generation{};
    TopologyGeneration topology_generation{};
};

/// Complete, structured, deterministic evaluation outcome.
struct ThermalEvaluation {
    ThermalDomainId domain{};
    ThermalDomainGeneration domain_generation{};

    ThermalState state = ThermalState::UNKNOWN;
    DeratingLevel derating = DeratingLevel::UNKNOWN;
    ThermalDecision decision = ThermalDecision::UNKNOWN;

    HeadroomBreakdown headroom;
    ThrottleClass throttle_class = ThrottleClass::THROTTLE_UNKNOWN;
    Provenance provenance = Provenance::UNKNOWN;

    Percent permitted_concurrency{0.0};
    ExecutionClass permitted_execution_class = ExecutionClass::BEST_EFFORT_THERMAL;
    std::optional<MegaHertz> permitted_clock_ceiling;
    double permitted_workload_intensity = 0.0;

    std::vector<MitigationIntent> prohibited_intents;
    MitigationIntentSet intents;
    ReasonCodeSet reasons;
    std::vector<RejectedAlternative> rejected;

    RecoveryAssessment recovery;

    ThermalPolicyGeneration policy_generation{};
    TelemetryGeneration telemetry_generation{};
    CapabilityGeneration capability_generation{};
    TopologyGeneration topology_generation{};
    CoordinatorEpoch coordinator_epoch{};

    /// True when the previous envelope was retained because recovery
    /// hysteresis forbids relaxation yet.
    bool retained_by_hysteresis = false;

    /// Hidden mutation guard: the derating level before the
    /// non-expansion clamp was applied.
    DeratingLevel unclamped_derating = DeratingLevel::UNKNOWN;
};

/// Evaluate a thermal domain. Pure and deterministic.
[[nodiscard]] ThermalEvaluation evaluate_thermal(const EvaluationInput& input);

/// Compute the thermal state implied by temperature, throttle evidence and
/// the previous state, honouring hysteresis.
[[nodiscard]] ThermalState next_thermal_state(const EvaluationInput& input,
                                              const RecoveryAssessment& recovery,
                                              TemperatureDelta& out_raw_headroom,
                                              ThrottleClass& out_throttle,
                                              ReasonCodeSet& out_reasons);

/// Derive the derating level implied by a state, a decision and coupling.
[[nodiscard]] DeratingLevel derating_for_state(ThermalState state,
                                               const ThermalPolicy& policy,
                                               bool workload_restricted);

/// Map a state and decision to the set of prohibited mitigation intents.
[[nodiscard]] std::vector<MitigationIntent> prohibited_intents_for(ThermalState state,
                                                                  const ThermalPolicy& policy);

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_EVALUATION_HPP
