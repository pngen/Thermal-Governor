// Thermal Governor — recovery hysteresis and gating.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/recovery.hpp"

#include <algorithm>

#include "format.hpp"

namespace thermal_governor {

void RecoveryHistory::push(const RecoverySample& sample) {
    samples_.push_back(sample);
    if (samples_.size() > capacity_) {
        samples_.erase(samples_.begin(),
                        samples_.begin() + static_cast<std::ptrdiff_t>(samples_.size() - capacity_));
    }
}

void RecoveryHistory::set_capacity(std::size_t capacity) {
    capacity_ = capacity == 0 ? 1 : capacity;
    if (samples_.size() > capacity_) {
        samples_.erase(samples_.begin(),
                       samples_.begin() + static_cast<std::ptrdiff_t>(samples_.size() - capacity_));
    }
}

void RecoveryHistory::invalidate_for_boot(WorkerBootId boot) {
    samples_.erase(std::remove_if(samples_.begin(), samples_.end(),
                                  [boot](const RecoverySample& s) {
                                      return !(s.worker_boot == boot);
                                  }),
                   samples_.end());
}

void RecoveryHistory::invalidate_for_epoch(CoordinatorEpoch epoch) {
    samples_.erase(std::remove_if(samples_.begin(), samples_.end(),
                                  [epoch](const RecoverySample& s) {
                                      return !(s.coordinator_epoch == epoch);
                                  }),
                   samples_.end());
}

void RecoveryHistory::invalidate_for_device_generation(DeviceGeneration generation) {
    samples_.erase(std::remove_if(samples_.begin(), samples_.end(),
                                  [generation](const RecoverySample& s) {
                                      return !(s.device_generation == generation);
                                  }),
                   samples_.end());
}

RecoveryAssessment evaluate_recovery(const RecoveryInput& input) {
    RecoveryAssessment out;
    out.recovery_threshold = input.recovery_threshold;
    out.recovery_margin = input.recovery_margin;
    out.current_temperature = input.headroom.current_temperature;
    out.effective_headroom = input.headroom.effective_headroom;

    if (input.policy == nullptr) {
        out.blocking_reasons.add(ThermalReasonCode::RECOVERY_FORBIDDEN_BY_POLICY);
        return out;
    }
    const ThermalPolicy& policy = *input.policy;

    out.required_samples = policy.freshness.required_consecutive_samples;
    out.required_span = policy.freshness.min_evidence_span;
    out.required_witnesses = policy.freshness.min_distinct_witnesses;

    if (input.evidence_provenance == Provenance::UNSUPPORTED &&
        policy.recovery.forbid_recovery_after_unsupported_evidence) {
        out.blocking_reasons.add(ThermalReasonCode::RECOVERY_FORBIDDEN_BY_POLICY);
        return out;
    }

    // Threshold predicate: strictly below recovery threshold + margin.
    out.temperature_below_threshold = input.headroom.distance_above_recovery_gate.value() < 0.0;
    out.blocking_reasons.add(out.temperature_below_threshold
                                 ? ThermalReasonCode::RECOVERY_THRESHOLD_SATISFIED
                                 : ThermalReasonCode::RECOVERY_THRESHOLD_EXCEEDED);

    // Headroom predicate.
    if (policy.recovery.require_headroom_above_recovery_margin) {
        out.headroom_requirement_satisfied = input.headroom.effective_headroom.value() > 0.0;
        out.blocking_reasons.add(out.headroom_requirement_satisfied
                                     ? ThermalReasonCode::EFFECTIVE_HEADROOM_SUFFICIENT
                                     : ThermalReasonCode::EFFECTIVE_HEADROOM_BELOW_DEMAND);
    } else {
        out.headroom_requirement_satisfied = true;
    }

    // Walk the retained history backwards and count the contiguous run of
    // qualifying samples that ends at the newest observation.
    std::vector<WorkerBootId> witnesses;
    std::optional<SteadyTimePoint> newest;
    std::optional<SteadyTimePoint> oldest;
    bool throttle_clear = true;
    bool generation_binding_ok = true;
    std::uint32_t qualifying = 0;

    if (input.history != nullptr) {
        const auto& samples = input.history->samples();
        for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
            const RecoverySample& sample = *it;
            if (!sample.admissible) {
                break;
            }
            if (policy.recovery.require_same_worker_boot &&
                !(sample.worker_boot == input.current_boot)) {
                generation_binding_ok = false;
                break;
            }
            if (policy.recovery.require_same_coordinator_epoch &&
                !(sample.coordinator_epoch == input.current_epoch)) {
                generation_binding_ok = false;
                break;
            }
            if (policy.recovery.require_same_device_generation &&
                !(sample.device_generation == input.current_device_generation)) {
                generation_binding_ok = false;
                break;
            }
            if (sample.temperature.value() >=
                input.recovery_threshold.value() + input.recovery_margin.value()) {
                break;
            }
            if (policy.recovery.require_no_thermal_throttle &&
                sample.throttle_class == ThrottleClass::THERMAL_THROTTLE_OBSERVED) {
                throttle_clear = false;
                break;
            }
            ++qualifying;
            if (!newest.has_value()) {
                newest = sample.at;
            }
            oldest = sample.at;
            if (std::find(witnesses.begin(), witnesses.end(), sample.worker_boot) ==
                witnesses.end()) {
                witnesses.push_back(sample.worker_boot);
            }
        }
    }

    out.qualifying_samples = qualifying;
    out.distinct_witnesses = witnesses.size();
    if (newest.has_value() && oldest.has_value()) {
        out.qualifying_span =
            std::chrono::duration_cast<Milliseconds>(*newest - *oldest);
    }

    out.throttle_clear = throttle_clear;
    if (policy.recovery.require_no_thermal_throttle && !throttle_clear) {
        out.blocking_reasons.add(ThermalReasonCode::RECOVERY_BLOCKED_BY_THROTTLE_EVIDENCE);
    }
    out.generation_binding_ok = generation_binding_ok;
    if (!generation_binding_ok) {
        out.blocking_reasons.add(ThermalReasonCode::EVIDENCE_GENERATION_INVALIDATED);
    }

    if (qualifying < out.required_samples) {
        out.blocking_reasons.add(ThermalReasonCode::RECOVERY_SAMPLES_INSUFFICIENT);
    }
    if (out.qualifying_span < out.required_span) {
        out.blocking_reasons.add(ThermalReasonCode::RECOVERY_EVIDENCE_SPAN_INSUFFICIENT);
    }
    if (out.distinct_witnesses < out.required_witnesses) {
        out.blocking_reasons.add(ThermalReasonCode::RECOVERY_WITNESSES_INSUFFICIENT);
    }

    out.coupled_domains_satisfied = input.coupled_domains_recovered;
    if (policy.recovery.require_coupled_domains_recovered && input.coupled_domains_checked &&
        !input.coupled_domains_recovered) {
        out.blocking_reasons.add(ThermalReasonCode::RECOVERY_BLOCKED_BY_COUPLED_DOMAIN);
    }

    out.explicit_authorization_present = input.explicit_authorization;
    if (policy.recovery.require_explicit_authorization && !input.explicit_authorization) {
        out.blocking_reasons.add(ThermalReasonCode::RECOVERY_REQUIRES_EXPLICIT_AUTHORIZATION);
    }

    // Generation binding is enforced by the walk itself: the qualifying run
    // stops at the first witness from a superseded boot, epoch or device
    // generation. It is therefore reported as a diagnostic, not used as a
    // second gate.
    out.allowed = out.temperature_below_threshold && out.headroom_requirement_satisfied &&
                  out.throttle_clear &&
                  qualifying >= out.required_samples && out.qualifying_span >= out.required_span &&
                  out.distinct_witnesses >= out.required_witnesses &&
                  out.coupled_domains_satisfied &&
                  (!policy.recovery.require_explicit_authorization || input.explicit_authorization) &&
                  !(input.evidence_provenance == Provenance::UNSUPPORTED &&
                    policy.recovery.forbid_recovery_after_unsupported_evidence);

    if (out.allowed) {
        out.satisfied_reasons.add(ThermalReasonCode::RECOVERY_AUTHORIZED);
        out.blocking_reasons = ReasonCodeSet{};
        out.blocking_reasons.add(ThermalReasonCode::RECOVERY_AUTHORIZED);
    }
    out.blocking_reasons.canonicalise();
    out.satisfied_reasons.canonicalise();
    return out;
}

std::string RecoveryAssessment::render() const {
    std::string out;
    out += "Allowed: ";
    out += allowed ? "true" : "false";
    out += "\n";
    out += "RecoveryThreshold: " + detail::number(recovery_threshold.value()) + "\n";
    out += "RecoveryMargin: " + detail::number(recovery_margin.value()) + "\n";
    out += "CurrentTemperature: " + detail::number(current_temperature.value()) + "\n";
    out += "EffectiveHeadroom: " + detail::number(effective_headroom.value()) + "\n";
    out += "QualifyingSamples: " + detail::unsigned_decimal(qualifying_samples) + "/" +
           detail::unsigned_decimal(required_samples) + "\n";
    out += "QualifyingSpanMs: " + detail::millis(qualifying_span.count()) + "/" +
           detail::millis(required_span.count()) + "\n";
    out += "DistinctWitnesses: " + detail::unsigned_decimal(distinct_witnesses) + "/" +
           detail::unsigned_decimal(required_witnesses) + "\n";
    out += "TemperatureBelowThreshold: ";
    out += temperature_below_threshold ? "true" : "false";
    out += "\n";
    out += "HeadroomSatisfied: ";
    out += headroom_requirement_satisfied ? "true" : "false";
    out += "\n";
    out += "ThrottleClear: ";
    out += throttle_clear ? "true" : "false";
    out += "\n";
    out += "GenerationBindingOk: ";
    out += generation_binding_ok ? "true" : "false";
    out += "\n";
    out += "CoupledDomainsSatisfied: ";
    out += coupled_domains_satisfied ? "true" : "false";
    out += "\n";
    out += "ExplicitAuthorization: ";
    out += explicit_authorization_present ? "true" : "false";
    out += "\n";
    out += "Reasons: ";
    out += blocking_reasons.render();
    out += "\n";
    return out;
}

}  // namespace thermal_governor
