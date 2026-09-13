// Thermal Governor — decision artefacts and rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/decision.hpp"

#include <algorithm>

#include "format.hpp"
#include "thermal_governor/admission.hpp"
#include "thermal_governor/envelope.hpp"
#include "thermal_governor/mitigation.hpp"

namespace thermal_governor {

std::string ReasonCodeSet::render() const {
    if (codes_.empty()) {
        return "<none>";
    }
    std::vector<std::string> parts;
    parts.reserve(codes_.size());
    for (const auto code : codes_) {
        parts.emplace_back(to_string(code));
    }
    return detail::join(parts, ",");
}

void MitigationIntentSet::canonicalise() {
    std::sort(records_.begin(), records_.end(),
              [](const MitigationIntentRecord& a, const MitigationIntentRecord& b) {
                  if (a.intent != b.intent) {
                      return static_cast<std::uint8_t>(a.intent) <
                             static_cast<std::uint8_t>(b.intent);
                  }
                  if (!(a.domain == b.domain)) {
                      return a.domain < b.domain;
                  }
                  return a.rationale < b.rationale;
              });
    records_.erase(std::unique(records_.begin(), records_.end(),
                               [](const MitigationIntentRecord& a,
                                  const MitigationIntentRecord& b) {
                                   return a.intent == b.intent && a.domain == b.domain &&
                                          a.domain_generation == b.domain_generation &&
                                          a.rationale == b.rationale &&
                                          a.clock_ceiling == b.clock_ceiling &&
                                          a.concurrency_ceiling == b.concurrency_ceiling &&
                                          a.admission_ceiling == b.admission_ceiling;
                               }),
                    records_.end());
}

bool MitigationIntentSet::contains(MitigationIntent intent) const noexcept {
    return std::any_of(records_.begin(), records_.end(),
                       [intent](const MitigationIntentRecord& r) { return r.intent == intent; });
}

std::string render_admission(const ThermalAdmissionResult& result) {
    std::string out;
    out += "Admission: ";
    out += to_string(result.decision);
    out += "\n";
    out += "ThermalDomainId: " + detail::unsigned_decimal(result.domain.value()) + "\n";
    out += "ThermalDomainGeneration: " +
           detail::unsigned_decimal(result.domain_generation.value()) + "\n";
    out += "WorkloadId: " + detail::unsigned_decimal(result.workload.value()) + "\n";
    out += "WorkloadProfile: ";
    out += to_string(result.profile);
    out += "\n";
    out += "ThermalState: ";
    out += to_string(result.state);
    out += "\n";
    out += "Derating: ";
    out += to_string(result.derating);
    out += "\n";
    out += "PermittedExecutionClass: ";
    out += to_string(result.permitted_class);
    out += "\n";
    out += "PermittedConcurrency: " + detail::percent(result.permitted_concurrency.value()) + "\n";
    if (result.permitted_clock_ceiling.has_value()) {
        out += "PermittedClockCeilingMHz: " +
               detail::unsigned_decimal(result.permitted_clock_ceiling->value()) + "\n";
    } else {
        out += "PermittedClockCeilingMHz: <unconstrained>\n";
    }
    out += "GoverningLimit: " + detail::temperature(result.governing_limit.value()) + "\n";
    out += "CurrentTemperature: " + detail::temperature(result.current_temperature.value()) + "\n";
    out += "EffectiveHeadroom: " + detail::number(result.effective_headroom.value()) + "\n";
    out += "RequiredHeadroom: " + detail::number(result.required_headroom.value()) + "\n";
    out += "Reasons: ";
    out += result.reasons.render();
    out += "\n";
    out += "Constraints:";
    if (result.constraints.empty()) {
        out += " <none>";
    }
    out += "\n";
    for (const auto& record : result.constraints.records()) {
        out += "  ";
        out += to_string(record.intent);
        if (!record.rationale.empty()) {
            out += " - ";
            out += record.rationale;
        }
        out += "\n";
    }
    out += "CoordinatorEpoch: " + detail::unsigned_decimal(result.coordinator_epoch.value()) + "\n";
    out += "ThermalPolicyGeneration: " +
           detail::unsigned_decimal(result.policy_generation.value()) + "\n";
    out += "TelemetryGeneration: " +
           detail::unsigned_decimal(result.telemetry_generation.value()) + "\n";
    out += "CapabilityGeneration: " +
           detail::unsigned_decimal(result.capability_generation.value()) + "\n";
    out += "TopologyGeneration: " +
           detail::unsigned_decimal(result.topology_generation.value()) + "\n";
    return out;
}

std::string render_envelope(const ThermalEnvelope& envelope) {
    std::string out;
    out += "ThermalDomainId: " + detail::unsigned_decimal(envelope.domain.value()) + "\n";
    out += "ThermalDomainGeneration: " +
           detail::unsigned_decimal(envelope.domain_generation.value()) + "\n";
    out += "SubjectKind: ";
    out += to_string(envelope.subject_kind);
    out += "\n";
    out += "ThermalState: ";
    out += to_string(envelope.state);
    out += "\n";
    out += "Derating: ";
    out += to_string(envelope.derating);
    out += "\n";
    out += "Decision: ";
    out += to_string(envelope.decision);
    out += "\n";
    out += "MaximumLegalTemperature: " +
           detail::temperature(envelope.maximum_legal_temperature.value()) + "\n";
    if (envelope.vendor_temperature_limit.has_value()) {
        out += "VendorTemperatureLimit: " +
               detail::temperature(envelope.vendor_temperature_limit->value()) + "\n";
    } else {
        out += "VendorTemperatureLimit: <unsupported>\n";
    }
    out += "EffectiveHeadroom: " + detail::number(envelope.headroom.effective_headroom.value()) +
           "\n";
    out += "PermittedConcurrency: " + detail::percent(envelope.permitted_concurrency.value()) +
           "\n";
    out += "PermittedExecutionClass: ";
    out += to_string(envelope.permitted_execution_class);
    out += "\n";
    if (envelope.permitted_clock_ceiling.has_value()) {
        out += "PermittedClockCeilingMHz: " +
               detail::unsigned_decimal(envelope.permitted_clock_ceiling->value()) + "\n";
    } else {
        out += "PermittedClockCeilingMHz: <unconstrained>\n";
    }
    out += "PermittedWorkloadIntensity: " +
           detail::number(envelope.permitted_workload_intensity) + "\n";
    out += "ThrottleClass: ";
    out += to_string(envelope.throttle_class);
    out += "\n";
    out += "Provenance: ";
    out += to_string(envelope.provenance);
    out += "\n";
    out += "ProhibitedIntents:";
    if (envelope.prohibited_intents.empty()) {
        out += " <none>";
    }
    out += "\n";
    for (const auto intent : envelope.prohibited_intents) {
        out += "  ";
        out += to_string(intent);
        out += "\n";
    }
    out += "RequiredMitigation:";
    if (envelope.required_mitigation.empty()) {
        out += " <none>";
    }
    out += "\n";
    for (const auto& record : envelope.required_mitigation.records()) {
        out += "  ";
        out += to_string(record.intent);
        out += "\n";
    }
    out += "Reasons: ";
    out += envelope.reasons.render();
    out += "\n";
    out += "CoordinatorEpoch: " + detail::unsigned_decimal(envelope.coordinator_epoch.value()) +
           "\n";
    out += "ThermalPolicyGeneration: " +
           detail::unsigned_decimal(envelope.policy_generation.value()) + "\n";
    out += "TelemetryGeneration: " +
           detail::unsigned_decimal(envelope.telemetry_generation.value()) + "\n";
    out += "CapabilityGeneration: " +
           detail::unsigned_decimal(envelope.capability_generation.value()) + "\n";
    out += "TopologyGeneration: " +
           detail::unsigned_decimal(envelope.topology_generation.value()) + "\n";
    out += "RecoveryGeneration: " +
           detail::unsigned_decimal(envelope.recovery_generation.value()) + "\n";
    return out;
}

}  // namespace thermal_governor
