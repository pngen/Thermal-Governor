// Thermal Governor — action authority and lifecycle.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/action.hpp"

#include "format.hpp"

namespace thermal_governor {
namespace {

[[nodiscard]] Status stale(ThermalErrorCode code, const char* what) {
    return Status::failure(code, what);
}

}  // namespace

Status revalidate_action_authority(const ActionAuthority& authority,
                                   const AuthoritySnapshot& current) {
    if (!authority.action_id.is_valid()) {
        return Status::failure(ThermalErrorCode::INVALID_ARGUMENT, "action id must be non-zero");
    }
    if (!(current.coordinator_epoch == authority.coordinator_epoch)) {
        return stale(ThermalErrorCode::STALE_EPOCH,
                     "coordinator epoch advanced since the action was authorised");
    }
    if (authority.worker.is_valid() && !(current.worker_boot == authority.worker_boot)) {
        return stale(ThermalErrorCode::STALE_WORKER,
                     "worker boot identity changed since the action was authorised");
    }
    if (authority.device.is_valid() && !(current.device_generation == authority.device_generation)) {
        return stale(ThermalErrorCode::STALE_DEVICE_GENERATION,
                     "device generation advanced since the action was authorised");
    }
    if (!(current.domain_generation == authority.domain_generation)) {
        return stale(ThermalErrorCode::STALE_DOMAIN_GENERATION,
                     "thermal domain generation advanced since the action was authorised");
    }
    if (!(current.policy_generation == authority.policy_generation)) {
        return stale(ThermalErrorCode::STALE_POLICY,
                     "thermal policy generation advanced since the action was authorised");
    }
    if (current.telemetry_generation.value() < authority.telemetry_generation.value()) {
        return stale(ThermalErrorCode::STALE_TELEMETRY,
                     "current telemetry generation predates the action authority");
    }
    if (authority.topology_generation.is_valid() &&
        !(current.topology_generation == authority.topology_generation)) {
        return stale(ThermalErrorCode::STALE_TOPOLOGY,
                     "topology generation advanced since the action was authorised");
    }
    if (authority.capability_generation.is_valid() &&
        !(current.capability_generation == authority.capability_generation)) {
        return stale(ThermalErrorCode::STALE_CAPABILITY,
                     "capability generation advanced since the action was authorised");
    }
    if (current.action_generation.value() < authority.action_generation.value()) {
        return stale(ThermalErrorCode::STALE_ACTION,
                     "action generation has been superseded");
    }
    return Status::success();
}

std::string render_action(const ThermalAction& action) {
    std::string out;
    out += "ActionId: " + detail::unsigned_decimal(action.id.value()) + "\n";
    out += "ActionGeneration: " + detail::unsigned_decimal(action.generation.value()) + "\n";
    out += "Intent: ";
    out += to_string(action.intent);
    out += "\n";
    out += "Lifecycle: ";
    out += to_string(action.lifecycle);
    out += "\n";
    out += "ThermalDomainId: " + detail::unsigned_decimal(action.authority.domain.value()) + "\n";
    out += "ThermalDomainGeneration: " +
           detail::unsigned_decimal(action.authority.domain_generation.value()) + "\n";
    out += "CoordinatorEpoch: " +
           detail::unsigned_decimal(action.authority.coordinator_epoch.value()) + "\n";
    out += "ThermalPolicyGeneration: " +
           detail::unsigned_decimal(action.authority.policy_generation.value()) + "\n";
    out += "TelemetryGeneration: " +
           detail::unsigned_decimal(action.authority.telemetry_generation.value()) + "\n";
    out += "TopologyGeneration: " +
           detail::unsigned_decimal(action.authority.topology_generation.value()) + "\n";
    out += "CapabilityGeneration: " +
           detail::unsigned_decimal(action.authority.capability_generation.value()) + "\n";
    out += "WorkerId: " + detail::unsigned_decimal(action.authority.worker.value()) + "\n";
    out += "WorkerBootId: " + detail::unsigned_decimal(action.authority.worker_boot.value()) +
           "\n";
    out += "DeviceId: " + detail::unsigned_decimal(action.authority.device.value()) + "\n";
    out += "DeviceGeneration: " +
           detail::unsigned_decimal(action.authority.device_generation.value()) + "\n";
    if (action.requested_clock_ceiling.has_value()) {
        out += "RequestedClockCeilingMHz: " +
               detail::unsigned_decimal(action.requested_clock_ceiling->value()) + "\n";
    }
    if (action.requested_concurrency.has_value()) {
        out += "RequestedConcurrency: " +
               detail::percent(action.requested_concurrency->value()) + "\n";
    }
    if (action.requested_admission.has_value()) {
        out += "RequestedAdmission: " + detail::percent(action.requested_admission->value()) +
               "\n";
    }
    out += "Provenance: ";
    out += to_string(action.provenance);
    out += "\n";
    out += "Reasons: ";
    out += action.reasons.render();
    out += "\n";
    if (!action.detail.empty()) {
        out += "Detail: ";
        out += action.detail;
        out += "\n";
    }
    if (action.superseded_by.has_value()) {
        out += "SupersededBy: " + detail::unsigned_decimal(action.superseded_by->value()) + "\n";
    }
    return out;
}

}  // namespace thermal_governor
