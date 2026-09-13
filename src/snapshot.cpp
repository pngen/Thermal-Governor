// Thermal Governor — snapshot queries and summary rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/snapshot.hpp"

#include "format.hpp"

namespace thermal_governor {

const ThermalDomainSnapshot* GovernanceSnapshot::find_domain(ThermalDomainId id) const {
    const auto it = domains.find(id.value());
    return it == domains.end() ? nullptr : &it->second;
}

const DeviceSnapshot* GovernanceSnapshot::find_device(DeviceId id) const {
    const auto it = devices.find(id.value());
    return it == devices.end() ? nullptr : &it->second;
}

const WorkerSnapshot* GovernanceSnapshot::find_worker(WorkerId id) const {
    const auto it = workers.find(id.value());
    return it == workers.end() ? nullptr : &it->second;
}

const ThermalAction* GovernanceSnapshot::find_action(ActionId id) const {
    const auto it = actions.find(id.value());
    return it == actions.end() ? nullptr : &it->second;
}

const ThermalPolicy* GovernanceSnapshot::find_policy(ThermalPolicyId id) const {
    const auto it = policies.find(id.value());
    return it == policies.end() ? nullptr : &it->second;
}

std::string GovernanceSnapshot::render_summary() const {
    std::string out;
    out += "CoordinatorId: " + detail::unsigned_decimal(coordinator.value()) + "\n";
    out += "CoordinatorEpoch: " + detail::unsigned_decimal(coordinator_epoch.value()) + "\n";
    out += "ThermalPolicyGeneration: " + detail::unsigned_decimal(policy_generation.value()) +
           "\n";
    out += "TopologyGeneration: " + detail::unsigned_decimal(topology_generation.value()) + "\n";
    out += "CapabilityGeneration: " + detail::unsigned_decimal(capability_generation.value()) +
           "\n";
    out += "Domains: " + detail::unsigned_decimal(domains.size()) + "\n";
    out += "Devices: " + detail::unsigned_decimal(devices.size()) + "\n";
    out += "Workers: " + detail::unsigned_decimal(workers.size()) + "\n";
    out += "Actions: " + detail::unsigned_decimal(actions.size()) + "\n";
    out += "HistoricalTransitions: " + detail::unsigned_decimal(historical_transitions) + "\n";
    out += "HistoricalActions: " + detail::unsigned_decimal(historical_actions) + "\n";
    out += "HistoricalVerifications: " + detail::unsigned_decimal(historical_verifications) +
           "\n";
    out += "Policies: " + detail::unsigned_decimal(policies.size()) + "\n";
    out += "Couplings: " + detail::unsigned_decimal(couplings.size()) + "\n";
    return out;
}

}  // namespace thermal_governor
