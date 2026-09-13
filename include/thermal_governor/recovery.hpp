// Thermal Governor — recovery hysteresis and gating.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_RECOVERY_HPP
#define THERMAL_GOVERNOR_RECOVERY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "thermal_governor/decision.hpp"
#include "thermal_governor/headroom.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/policy.hpp"
#include "thermal_governor/provenance.hpp"
#include "thermal_governor/quantity.hpp"
#include "thermal_governor/throttle.hpp"
#include "thermal_governor/time.hpp"

namespace thermal_governor {

/// One observation retained for recovery hysteresis assessment.
struct RecoverySample {
    EvidenceId evidence_id{};
    DegreesCelsius temperature{};
    SteadyTimePoint at{};
    WorkerBootId worker_boot{};
    WorkerId worker{};
    CoordinatorEpoch coordinator_epoch{};
    DeviceGeneration device_generation{};
    ThermalDomainGeneration domain_generation{};
    TelemetryGeneration telemetry_generation{};
    ThrottleClass throttle_class = ThrottleClass::THROTTLE_UNKNOWN;
    Provenance provenance = Provenance::UNKNOWN;
    bool admissible = false;
};

/// Bounded rolling history of observations used to gate recovery.
///
/// Recovery requires more than one favourable instantaneous reading when
/// policy demands it. Duration-based stability uses the recorded sample
/// timestamps rather than sleeps.
class RecoveryHistory {
public:
    explicit RecoveryHistory(std::size_t capacity = 64) : capacity_(capacity == 0 ? 1 : capacity) {}

    void push(const RecoverySample& sample);
    void clear() noexcept { samples_.clear(); }
    void set_capacity(std::size_t capacity);

    [[nodiscard]] const std::vector<RecoverySample>& samples() const noexcept { return samples_; }
    [[nodiscard]] std::size_t size() const noexcept { return samples_.size(); }
    [[nodiscard]] bool empty() const noexcept { return samples_.empty(); }

    /// Drop every sample that cannot be part of a current recovery case:
    /// samples from a superseded worker boot, coordinator epoch or device
    /// generation, and generation-invalidated observations.
    void invalidate_for_boot(WorkerBootId boot);
    void invalidate_for_epoch(CoordinatorEpoch epoch);
    void invalidate_for_device_generation(DeviceGeneration generation);

private:
    std::size_t capacity_;
    std::vector<RecoverySample> samples_;
};

/// Inputs to a recovery assessment.
struct RecoveryInput {
    const ThermalPolicy* policy = nullptr;
    const RecoveryHistory* history = nullptr;
    DegreesCelsius recovery_threshold{};
    TemperatureDelta recovery_margin{0.0};
    HeadroomBreakdown headroom;
    Provenance evidence_provenance = Provenance::UNKNOWN;
    CoordinatorEpoch current_epoch{};
    WorkerBootId current_boot{};
    DeviceGeneration current_device_generation{};
    ThermalDomainGeneration current_domain_generation{};
    bool explicit_authorization = false;
    /// True when every coupled domain that policy requires to recover is
    /// itself recovery-eligible.
    bool coupled_domains_recovered = true;
    /// True when coupling policy actually requires this check.
    bool coupled_domains_checked = false;
    /// Independent witness boot ids observed among qualifying samples.
    std::size_t distinct_witnesses = 0;
};

/// The full, auditable outcome of a recovery assessment.
struct RecoveryAssessment {
    bool allowed = false;

    DegreesCelsius recovery_threshold{};
    TemperatureDelta recovery_margin{};
    DegreesCelsius current_temperature{};
    TemperatureDelta effective_headroom{};

    std::uint32_t qualifying_samples = 0;
    std::uint32_t required_samples = 0;
    Milliseconds qualifying_span{0};
    Milliseconds required_span{0};
    std::size_t distinct_witnesses = 0;
    std::size_t required_witnesses = 0;

    bool temperature_below_threshold = false;
    bool headroom_requirement_satisfied = false;
    bool throttle_clear = false;
    bool generation_binding_ok = false;
    bool coupled_domains_satisfied = false;
    bool explicit_authorization_present = false;

    ReasonCodeSet blocking_reasons;
    ReasonCodeSet satisfied_reasons;

    [[nodiscard]] std::string render() const;
};

/// Deterministically assess whether recovery is permitted.
[[nodiscard]] RecoveryAssessment evaluate_recovery(const RecoveryInput& input);

/// Explicit operator recovery authorization token, generation-bound.
struct RecoveryAuthorization {
    ThermalDomainId domain{};
    ThermalDomainGeneration domain_generation{};
    RecoveryGeneration recovery_generation{};
    CoordinatorEpoch coordinator_epoch{};
    ThermalPolicyGeneration policy_generation{};
    WorkerBootId worker_boot{};
    DeviceGeneration device_generation{};
    ThermalDomainGeneration revalidated_at_domain_generation{};
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_RECOVERY_HPP
