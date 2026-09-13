// Thermal Governor — deterministic structured explanations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/explanation.hpp"

#include <algorithm>

#include "format.hpp"

namespace thermal_governor {
namespace {

void append(std::string& out, const std::string& text) {
    if (out.size() + text.size() > Explanation::kMaxRenderedBytes) {
        return;
    }
    out += text;
}

[[nodiscard]] ExplanationSection domain_section(const ThermalEvaluation& evaluation,
                                                const ThermalDomainDefinition* definition) {
    ExplanationSection section;
    section.title = "Thermal domain";
    section.lines.push_back({"Label", definition != nullptr && !definition->label.empty()
                                         ? definition->label.text()
                                         : std::string("<unlabelled>")});
    section.lines.push_back(
        {"ThermalDomainId", detail::unsigned_decimal(evaluation.domain.value())});
    section.lines.push_back({"ThermalDomainGeneration",
                             detail::unsigned_decimal(evaluation.domain_generation.value())});
    section.lines.push_back({"ThermalDomainType",
                             definition != nullptr ? std::string(to_string(definition->type))
                                                   : std::string("<unknown>")});
    if (definition != nullptr && !definition->evidence_source.empty()) {
        section.lines.push_back({"EvidenceSource", definition->evidence_source});
    }
    return section;
}

[[nodiscard]] ExplanationSection temperature_section(const ThermalEvaluation& evaluation,
                                                     const ThermalPolicy& policy) {
    ExplanationSection section;
    section.title = "Temperature";
    const auto& headroom = evaluation.headroom;
    section.lines.push_back({"Current", detail::temperature(headroom.current_temperature.value())});
    section.lines.push_back({"Warning", detail::temperature(policy.thresholds.warning.value())});
    section.lines.push_back(
        {"NearLimitStart", detail::temperature(policy.thresholds.near_limit_start().value())});
    section.lines.push_back({"Derating", detail::temperature(policy.thresholds.derating.value())});
    section.lines.push_back({"Critical", detail::temperature(policy.thresholds.critical.value())});
    section.lines.push_back({"Recovery", detail::temperature(policy.thresholds.recovery.value())});
    if (headroom.vendor_limit.has_value()) {
        section.lines.push_back(
            {"VendorLimit", detail::temperature(headroom.vendor_limit->value())});
    } else {
        section.lines.push_back({"VendorLimit", "<unsupported>"});
    }
    section.lines.push_back(
        {"GoverningLimit", detail::temperature(headroom.governing_limit.value())});
    return section;
}

[[nodiscard]] ExplanationSection headroom_section(const ThermalEvaluation& evaluation) {
    ExplanationSection section;
    section.title = "Headroom";
    const auto& headroom = evaluation.headroom;
    section.lines.push_back({"RawHeadroom", detail::number(headroom.raw_headroom.value())});
    section.lines.push_back(
        {"PolicySafetyMargin", detail::number(headroom.policy_safety_margin.value())});
    section.lines.push_back(
        {"UncertaintyMargin", detail::number(headroom.uncertainty_margin.value())});
    section.lines.push_back(
        {"EffectiveHeadroom", detail::number(headroom.effective_headroom.value())});
    section.lines.push_back({"RecoveryMargin", detail::number(headroom.recovery_margin.value())});
    return section;
}

[[nodiscard]] ExplanationSection state_section(const ThermalEvaluation& evaluation) {
    ExplanationSection section;
    section.title = "Thermal state";
    section.lines.push_back({"State", std::string(to_string(evaluation.state))});
    section.lines.push_back({"Derating", std::string(to_string(evaluation.derating))});
    section.lines.push_back({"Decision", std::string(to_string(evaluation.decision))});
    section.lines.push_back(
        {"RetainedByHysteresis", evaluation.retained_by_hysteresis ? "true" : "false"});
    return section;
}

[[nodiscard]] ExplanationSection throttle_section(const ThermalEvaluation& evaluation) {
    ExplanationSection section;
    section.title = "Throttle";
    section.lines.push_back({"Classification", std::string(to_string(evaluation.throttle_class))});
    return section;
}

[[nodiscard]] ExplanationSection restrictions_section(const ThermalEvaluation& evaluation) {
    ExplanationSection section;
    section.title = "Restrictions";
    section.lines.push_back(
        {"PermittedConcurrency", detail::percent(evaluation.permitted_concurrency.value())});
    section.lines.push_back(
        {"PermittedExecutionClass", std::string(to_string(evaluation.permitted_execution_class))});
    section.lines.push_back({"PermittedWorkloadIntensity",
                             detail::number(evaluation.permitted_workload_intensity)});
    if (evaluation.permitted_clock_ceiling.has_value()) {
        section.lines.push_back({"PermittedClockCeilingMHz",
                                 detail::unsigned_decimal(
                                     evaluation.permitted_clock_ceiling->value())});
    }
    for (const auto& record : evaluation.intents.records()) {
        std::string bullet(to_string(record.intent));
        if (!record.rationale.empty()) {
            bullet += " - ";
            bullet += record.rationale;
        }
        section.bullets.push_back(std::move(bullet));
    }
    return section;
}

[[nodiscard]] ExplanationSection rejected_section(const ThermalEvaluation& evaluation) {
    ExplanationSection section;
    section.title = "Rejected";
    std::vector<std::pair<std::uint32_t, std::string>> ordered;
    ordered.reserve(evaluation.rejected.size());
    for (const auto& rejected : evaluation.rejected) {
        std::string text(to_string(rejected.candidate));
        text += " - ";
        text += to_string(rejected.reason);
        ordered.emplace_back(static_cast<std::uint32_t>(rejected.candidate), std::move(text));
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [rank, text] : ordered) {
        (void)rank;
        section.bullets.push_back(std::move(text));
    }
    return section;
}

[[nodiscard]] ExplanationSection recovery_section(const ThermalEvaluation& evaluation) {
    ExplanationSection section;
    section.title = "Recovery";
    const auto& recovery = evaluation.recovery;
    section.lines.push_back({"Allowed", recovery.allowed ? "true" : "false"});
    section.lines.push_back({"RecoveryThreshold",
                             detail::temperature(recovery.recovery_threshold.value())});
    section.lines.push_back(
        {"QualifyingSamples", detail::unsigned_decimal(recovery.qualifying_samples) + "/" +
                                  detail::unsigned_decimal(recovery.required_samples)});
    section.lines.push_back({"QualifyingSpanMs", detail::millis(recovery.qualifying_span.count()) +
                                                      "/" +
                                                      detail::millis(recovery.required_span.count())});
    section.lines.push_back({"TemperatureBelowThreshold",
                             recovery.temperature_below_threshold ? "true" : "false"});
    section.lines.push_back({"HeadroomSatisfied",
                             recovery.headroom_requirement_satisfied ? "true" : "false"});
    section.lines.push_back({"ThrottleClear", recovery.throttle_clear ? "true" : "false"});
    section.lines.push_back({"GenerationBindingOk",
                             recovery.generation_binding_ok ? "true" : "false"});
    section.lines.push_back({"CoupledDomainsSatisfied",
                             recovery.coupled_domains_satisfied ? "true" : "false"});
    section.lines.push_back({"Reasons", recovery.blocking_reasons.render()});
    return section;
}

[[nodiscard]] ExplanationSection authority_section(const ThermalEvaluation& evaluation) {
    ExplanationSection section;
    section.title = "Authority";
    section.lines.push_back(
        {"CoordinatorEpoch", detail::unsigned_decimal(evaluation.coordinator_epoch.value())});
    section.lines.push_back({"ThermalPolicyGeneration",
                             detail::unsigned_decimal(evaluation.policy_generation.value())});
    section.lines.push_back({"TelemetryGeneration",
                             detail::unsigned_decimal(evaluation.telemetry_generation.value())});
    section.lines.push_back({"ThermalDomainGeneration",
                             detail::unsigned_decimal(evaluation.domain_generation.value())});
    section.lines.push_back({"CapabilityGeneration",
                             detail::unsigned_decimal(evaluation.capability_generation.value())});
    section.lines.push_back(
        {"TopologyGeneration", detail::unsigned_decimal(evaluation.topology_generation.value())});
    return section;
}

}  // namespace

std::string Explanation::render() const {
    std::string out;
    for (std::size_t i = 0; i < sections_.size(); ++i) {
        if (i != 0) {
            append(out, "\n");
        }
        append(out, sections_[i].title);
        append(out, ":\n");
        for (const auto& line : sections_[i].lines) {
            append(out, line.key);
            append(out, ": ");
            append(out, line.value);
            append(out, "\n");
        }
        for (const auto& bullet : sections_[i].bullets) {
            append(out, "  - ");
            append(out, bullet);
            append(out, "\n");
        }
    }
    return out;
}

Explanation build_explanation(const ThermalEvaluation& evaluation, const ThermalEnvelope& envelope,
                             const ThermalPolicy& policy) {
    (void)envelope;
    Explanation explanation;
    explanation.add_section(domain_section(evaluation, nullptr));
    explanation.add_section(temperature_section(evaluation, policy));
    explanation.add_section(headroom_section(evaluation));
    explanation.add_section(throttle_section(evaluation));
    explanation.add_section(state_section(evaluation));
    explanation.add_section(restrictions_section(evaluation));
    explanation.add_section(rejected_section(evaluation));
    explanation.add_section(recovery_section(evaluation));
    ExplanationSection reasons;
    reasons.title = "Reasons";
    reasons.lines.push_back({"Codes", evaluation.reasons.render()});
    reasons.lines.push_back({"Provenance", std::string(to_string(evaluation.provenance))});
    explanation.add_section(std::move(reasons));
    explanation.add_section(authority_section(evaluation));
    return explanation;
}

Explanation build_admission_explanation(const ThermalAdmissionResult& result,
                                        const ThermalPolicy& policy) {
    (void)policy;
    Explanation explanation;

    ExplanationSection subject;
    subject.title = "Thermal admission";
    subject.lines.push_back({"Decision", std::string(to_string(result.decision))});
    subject.lines.push_back(
        {"ThermalDomainId", detail::unsigned_decimal(result.domain.value())});
    subject.lines.push_back({"ThermalDomainGeneration",
                             detail::unsigned_decimal(result.domain_generation.value())});
    subject.lines.push_back(
        {"WorkloadProfile", std::string(to_string(result.profile))});
    explanation.add_section(std::move(subject));

    ExplanationSection state;
    state.title = "Thermal state";
    state.lines.push_back({"State", std::string(to_string(result.state))});
    state.lines.push_back({"Derating", std::string(to_string(result.derating))});
    explanation.add_section(std::move(state));

    ExplanationSection headroom;
    headroom.title = "Headroom";
    headroom.lines.push_back({"GoverningLimit", detail::temperature(result.governing_limit.value())});
    headroom.lines.push_back(
        {"CurrentTemperature", detail::temperature(result.current_temperature.value())});
    headroom.lines.push_back({"EffectiveHeadroom",
                              detail::number(result.effective_headroom.value())});
    headroom.lines.push_back({"RequiredHeadroom",
                              detail::number(result.required_headroom.value())});
    explanation.add_section(std::move(headroom));

    ExplanationSection restrictions;
    restrictions.title = "Restrictions";
    restrictions.lines.push_back(
        {"PermittedConcurrency", detail::percent(result.permitted_concurrency.value())});
    restrictions.lines.push_back(
        {"PermittedExecutionClass", std::string(to_string(result.permitted_class))});
    if (result.permitted_clock_ceiling.has_value()) {
        restrictions.lines.push_back({"PermittedClockCeilingMHz",
                                      detail::unsigned_decimal(
                                          result.permitted_clock_ceiling->value())});
    } else {
        restrictions.lines.push_back({"PermittedClockCeilingMHz", "<unconstrained>"});
    }
    for (const auto& record : result.constraints.records()) {
        std::string bullet(to_string(record.intent));
        if (!record.rationale.empty()) {
            bullet += " - ";
            bullet += record.rationale;
        }
        restrictions.bullets.push_back(std::move(bullet));
    }
    explanation.add_section(std::move(restrictions));

    ExplanationSection reasons;
    reasons.title = "Reasons";
    reasons.lines.push_back({"Codes", result.reasons.render()});
    explanation.add_section(std::move(reasons));

    ExplanationSection authority;
    authority.title = "Authority";
    authority.lines.push_back(
        {"CoordinatorEpoch", detail::unsigned_decimal(result.coordinator_epoch.value())});
    authority.lines.push_back({"ThermalPolicyGeneration",
                               detail::unsigned_decimal(result.policy_generation.value())});
    authority.lines.push_back({"TelemetryGeneration",
                               detail::unsigned_decimal(result.telemetry_generation.value())});
    authority.lines.push_back({"CapabilityGeneration",
                               detail::unsigned_decimal(result.capability_generation.value())});
    authority.lines.push_back({"TopologyGeneration",
                               detail::unsigned_decimal(result.topology_generation.value())});
    explanation.add_section(std::move(authority));
    return explanation;
}

Explanation build_recovery_explanation(const RecoveryAssessment& assessment,
                                       const ThermalPolicy& policy) {
    (void)policy;
    Explanation explanation;
    ExplanationSection section;
    section.title = "Recovery";
    section.lines.push_back({"Allowed", assessment.allowed ? "true" : "false"});
    section.lines.push_back(
        {"RecoveryThreshold", detail::temperature(assessment.recovery_threshold.value())});
    section.lines.push_back(
        {"RecoveryMargin", detail::number(assessment.recovery_margin.value())});
    section.lines.push_back(
        {"CurrentTemperature", detail::temperature(assessment.current_temperature.value())});
    section.lines.push_back(
        {"EffectiveHeadroom", detail::number(assessment.effective_headroom.value())});
    section.lines.push_back({"QualifyingSamples",
                             detail::unsigned_decimal(assessment.qualifying_samples) + "/" +
                                 detail::unsigned_decimal(assessment.required_samples)});
    section.lines.push_back({"QualifyingSpanMs",
                             detail::millis(assessment.qualifying_span.count()) + "/" +
                                 detail::millis(assessment.required_span.count())});
    section.lines.push_back({"DistinctWitnesses",
                             detail::unsigned_decimal(assessment.distinct_witnesses) + "/" +
                                 detail::unsigned_decimal(assessment.required_witnesses)});
    section.lines.push_back({"TemperatureBelowThreshold",
                             assessment.temperature_below_threshold ? "true" : "false"});
    section.lines.push_back({"HeadroomSatisfied",
                             assessment.headroom_requirement_satisfied ? "true" : "false"});
    section.lines.push_back({"ThrottleClear", assessment.throttle_clear ? "true" : "false"});
    section.lines.push_back(
        {"GenerationBindingOk", assessment.generation_binding_ok ? "true" : "false"});
    section.lines.push_back({"CoupledDomainsSatisfied",
                             assessment.coupled_domains_satisfied ? "true" : "false"});
    section.lines.push_back({"ExplicitAuthorization",
                             assessment.explicit_authorization_present ? "true" : "false"});
    section.lines.push_back({"Reasons", assessment.blocking_reasons.render()});
    explanation.add_section(std::move(section));
    return explanation;
}

}  // namespace thermal_governor
