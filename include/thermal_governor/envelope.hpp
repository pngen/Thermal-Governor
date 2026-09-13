// Thermal Governor — queryable, generation-bound thermal envelope.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_ENVELOPE_HPP
#define THERMAL_GOVERNOR_ENVELOPE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "thermal_governor/decision.hpp"
#include "thermal_governor/evidence.hpp"
#include "thermal_governor/headroom.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/mitigation.hpp"
#include "thermal_governor/policy.hpp"
#include "thermal_governor/provenance.hpp"
#include "thermal_governor/quantity.hpp"
#include "thermal_governor/thermal_state.hpp"
#include "thermal_governor/throttle.hpp"
#include "thermal_governor/workload.hpp"

namespace thermal_governor {

/// The thermal operating envelope of a governed resource or domain.
///
/// Everything here is generation-bound and queryable. A caller can always
/// answer: under exactly which generations is this envelope valid?
struct ThermalEnvelope {
    ThermalDomainId domain{};
    ThermalDomainGeneration domain_generation{};

    SubjectKind subject_kind = SubjectKind::DEVICE;
    DeviceId device{};
    DeviceGeneration device_generation{};
    NodeId node{};
    NodeGeneration node_generation{};

    ThermalState state = ThermalState::UNKNOWN;
    DeratingLevel derating = DeratingLevel::UNKNOWN;
    ThermalDecision decision = ThermalDecision::UNKNOWN;

    DegreesCelsius maximum_legal_temperature{};
    std::optional<DegreesCelsius> vendor_temperature_limit{};

    ThermalThresholds thresholds;
    HeadroomBreakdown headroom;

    Percent permitted_concurrency{0.0};
    ExecutionClass permitted_execution_class = ExecutionClass::BEST_EFFORT_THERMAL;
    std::optional<MegaHertz> permitted_clock_ceiling;
    double permitted_workload_intensity = 0.0;

    std::vector<MitigationIntent> prohibited_intents;
    MitigationIntentSet required_mitigation;

    ReasonCodeSet reasons;

    Provenance provenance = Provenance::UNKNOWN;
    ThrottleClass throttle_class = ThrottleClass::THROTTLE_UNKNOWN;

    CoordinatorEpoch coordinator_epoch{};
    ThermalPolicyGeneration policy_generation{};
    TelemetryGeneration telemetry_generation{};
    CapabilityGeneration capability_generation{};
    TopologyGeneration topology_generation{};
    RecoveryGeneration recovery_generation{};

    [[nodiscard]] bool permits_full_capability() const noexcept {
        return derating == DeratingLevel::FULL_CAPABILITY &&
               state != ThermalState::DERATED && state != ThermalState::THROTTLING &&
               state != ThermalState::CRITICAL;
    }
};

/// Canonical rendering of an envelope. Field order is stable.
[[nodiscard]] std::string render_envelope(const ThermalEnvelope& envelope);

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_ENVELOPE_HPP
