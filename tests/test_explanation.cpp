// Thermal Governor — deterministic explanation tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <string>
#include <vector>

#include "framework.hpp"

#include "thermal_governor/explanation.hpp"

#include "thermal_governor/capability.hpp"
#include "thermal_governor/decision.hpp"
#include "thermal_governor/evidence.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/policy.hpp"
#include "thermal_governor/provenance.hpp"
#include "thermal_governor/quantity.hpp"
#include "thermal_governor/recovery.hpp"
#include "thermal_governor/thermal_state.hpp"
#include "thermal_governor/time.hpp"
#include "thermal_governor/workload.hpp"

using thermal_governor::AdmissionDecision;
using thermal_governor::CapabilityGeneration;
using thermal_governor::CapabilityState;
using thermal_governor::CoordinatorEpoch;
using thermal_governor::DegreesCelsius;
using thermal_governor::DeratingLevel;
using thermal_governor::DeviceGeneration;
using thermal_governor::DeviceId;
using thermal_governor::EvaluationInput;
using thermal_governor::EvidenceId;
using thermal_governor::ExecutionClass;
using thermal_governor::Explanation;
using thermal_governor::ExplanationSection;
using thermal_governor::FreshnessState;
using thermal_governor::MeasurementSource;
using thermal_governor::Milliseconds;
using thermal_governor::MitigationIntent;
using thermal_governor::Percent;
using thermal_governor::Provenance;
using thermal_governor::RecoveryAssessment;
using thermal_governor::RejectedAlternative;
using thermal_governor::StrongId;
using thermal_governor::SubjectRef;
using thermal_governor::TelemetryGeneration;
using thermal_governor::TemperatureDelta;
using thermal_governor::ThermalAdmissionResult;
using thermal_governor::ThermalDecision;
using thermal_governor::ThermalDomainGeneration;
using thermal_governor::ThermalDomainId;
using thermal_governor::ThermalDomainType;
using thermal_governor::ThermalEnvelope;
using thermal_governor::ThermalEvaluation;
using thermal_governor::ThermalEvidence;
using thermal_governor::ThermalPolicy;
using thermal_governor::ThermalPolicyGeneration;
using thermal_governor::ThermalReasonCode;
using thermal_governor::ThermalState;
using thermal_governor::TopologyGeneration;
using thermal_governor::WorkerBootId;
using thermal_governor::WorkerId;
using thermal_governor::WorkloadThermalProfile;
using thermal_governor::build_admission_explanation;
using thermal_governor::build_explanation;
using thermal_governor::build_recovery_explanation;
using thermal_governor::evaluate_thermal;

namespace {

[[nodiscard]] std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

/// Value of the first line rendered as "key: value".
[[nodiscard]] std::string line_value(const std::string& text, const std::string& key) {
    const std::string prefix = key + ": ";
    for (const auto& line : split_lines(text)) {
        if (line.rfind(prefix, 0) == 0) {
            return line.substr(prefix.size());
        }
    }
    return {};
}

/// Index of the first line that is exactly "key:", which is how a section
/// header is rendered.
[[nodiscard]] std::size_t line_index(const std::vector<std::string>& lines,
                                     const std::string& key) {
    const std::string exact = key + ":";
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index] == exact) {
            return index;
        }
    }
    return lines.size();
}

[[nodiscard]] ThermalEvidence critical_evidence() {
    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<thermal_governor::EvidenceIdTag>{1}};
    evidence.subject = SubjectRef::for_device(
        DeviceId{StrongId<thermal_governor::DeviceIdTag>{1}},
        DeviceGeneration{StrongId<thermal_governor::DeviceGenerationTag>{1}});
    evidence.domain = ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{1}};
    evidence.domain_generation =
        ThermalDomainGeneration{StrongId<thermal_governor::ThermalDomainGenerationTag>{1}};
    evidence.temperature = DegreesCelsius{95.25};
    evidence.source = MeasurementSource::NVML_GPU_TEMPERATURE;
    evidence.provenance = Provenance::REAL;
    evidence.measurement_sequence = 1;
    evidence.telemetry_generation =
        TelemetryGeneration{StrongId<thermal_governor::TelemetryGenerationTag>{1}};
    evidence.worker = WorkerId{StrongId<thermal_governor::WorkerIdTag>{1}};
    evidence.worker_boot = WorkerBootId{StrongId<thermal_governor::WorkerBootIdTag>{1}};
    evidence.coordinator_epoch =
        CoordinatorEpoch{StrongId<thermal_governor::CoordinatorEpochTag>{1}};
    evidence.capability_generation =
        CapabilityGeneration{StrongId<thermal_governor::CapabilityGenerationTag>{1}};
    evidence.topology_generation =
        TopologyGeneration{StrongId<thermal_governor::TopologyGenerationTag>{1}};
    return evidence;
}

[[nodiscard]] ThermalEvaluation critical_evaluation(const ThermalEvidence& evidence) {
    EvaluationInput input;
    input.domain = ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{1}};
    input.domain_generation =
        ThermalDomainGeneration{StrongId<thermal_governor::ThermalDomainGenerationTag>{1}};
    input.domain_type = ThermalDomainType::ACCELERATOR;
    input.policy = ThermalPolicy::make_default();
    input.temperature_capability = CapabilityState::SUPPORTED_REAL;
    input.throttle_capability = CapabilityState::SUPPORTED_REAL;
    input.limit_capability = CapabilityState::UNSUPPORTED;
    input.freshness = FreshnessState::FRESH;
    input.workload_profile = WorkloadThermalProfile::MODERATE_THERMAL_INTENSITY;
    input.coordinator_epoch = CoordinatorEpoch{StrongId<thermal_governor::CoordinatorEpochTag>{1}};
    input.policy_generation =
        ThermalPolicyGeneration{StrongId<thermal_governor::ThermalPolicyGenerationTag>{1}};
    input.topology_generation =
        TopologyGeneration{StrongId<thermal_governor::TopologyGenerationTag>{1}};
    input.evidence = &evidence;
    return evaluate_thermal(input);
}

[[nodiscard]] bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

TG_CASE(explanation, critical_evaluation_never_renders_allow) {
    const ThermalEvidence evidence = critical_evidence();
    const ThermalEvaluation evaluation = critical_evaluation(evidence);
    TG_CHECK(evaluation.state == ThermalState::CRITICAL);
    TG_CHECK(evaluation.derating == DeratingLevel::NO_SAFE_EXECUTION);

    const ThermalPolicy policy = ThermalPolicy::make_default();
    const ThermalEnvelope envelope;
    const Explanation explanation = build_explanation(evaluation, envelope, policy);
    const std::string text = explanation.render();

    TG_CHECK(!text.empty());
    TG_CHECK(contains(text, "CRITICAL"));
    TG_CHECK(contains(text, "State: CRITICAL"));
    TG_CHECK(contains(text, "NO_SAFE_EXECUTION"));

    const std::string decision = line_value(text, "Decision");
    TG_CHECK(!decision.empty());
    TG_CHECK(decision != "ALLOW");
    TG_CHECK_EQ(decision, std::string("DENY"));

    TG_PHASE("a critical envelope never claims full capability");
    TG_CHECK(evaluation.permitted_concurrency.value() < 100.0);
    TG_CHECK(contains(text, "Rejected:"));
    TG_CHECK(contains(text, "CRITICAL_THRESHOLD_EXCEEDED"));
}

TG_CASE(explanation, render_is_byte_identical_for_equal_inputs) {
    const ThermalEvidence first_evidence = critical_evidence();
    ThermalEvidence second_evidence = critical_evidence();
    const ThermalEvaluation first = critical_evaluation(first_evidence);
    const ThermalEvaluation second = critical_evaluation(second_evidence);
    const ThermalPolicy policy = ThermalPolicy::make_default();
    const ThermalEnvelope envelope;

    const Explanation first_explanation = build_explanation(first, envelope, policy);
    const Explanation second_explanation = build_explanation(second, envelope, policy);
    TG_CHECK(first_explanation == second_explanation);

    const std::string first_text = first_explanation.render();
    const std::string second_text = second_explanation.render();
    TG_CHECK(!first_text.empty());
    TG_CHECK_EQ(first_text, second_text);
    TG_CHECK_EQ(first_text, first_explanation.render());
    TG_CHECK_EQ(first_text, build_explanation(first, envelope, policy).render());

    TG_PHASE("the text is not vacuous");
    TG_CHECK(first_text.size() > 256U);
}

TG_CASE(explanation, section_order_is_stable) {
    const ThermalEvidence evidence = critical_evidence();
    const ThermalEvaluation evaluation = critical_evaluation(evidence);
    const ThermalPolicy policy = ThermalPolicy::make_default();
    const ThermalEnvelope envelope;
    const Explanation explanation = build_explanation(evaluation, envelope, policy);

    const std::vector<std::string> expected_titles = {
        "Thermal domain", "Temperature", "Headroom",  "Throttle",  "Thermal state",
        "Restrictions",   "Rejected",    "Recovery",  "Reasons",   "Authority"};

    const auto& sections = explanation.sections();
    TG_CHECK_EQ(sections.size(), expected_titles.size());
    for (std::size_t index = 0; index < expected_titles.size(); ++index) {
        TG_CHECK_EQ(sections[index].title, expected_titles[index]);
    }

    TG_PHASE("the rendered text places the sections in the same order");
    const std::vector<std::string> lines = split_lines(explanation.render());
    std::size_t previous = 0;
    for (const auto& title : expected_titles) {
        const std::size_t position = line_index(lines, title);
        TG_CHECK(position < lines.size());
        TG_CHECK(position >= previous);
        previous = position;
    }

    TG_PHASE("every section header is followed by its own body");
    TG_CHECK_EQ(lines.front(), std::string("Thermal domain:"));
    TG_CHECK(lines.size() > expected_titles.size());
}

TG_CASE(explanation, rejected_section_is_ordered_by_candidate_rank) {
    ThermalEvaluation evaluation;
    evaluation.domain = ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{1}};
    evaluation.domain_generation =
        ThermalDomainGeneration{StrongId<thermal_governor::ThermalDomainGenerationTag>{1}};
    evaluation.state = ThermalState::DERATED;
    evaluation.derating = DeratingLevel::DERATED_CLOCK;
    evaluation.decision = ThermalDecision::ALLOW_DERATED;
    evaluation.headroom.current_temperature = DegreesCelsius{82.5};
    evaluation.headroom.governing_limit = DegreesCelsius{88.0};
    evaluation.headroom.raw_headroom = TemperatureDelta{5.5};
    evaluation.headroom.effective_headroom = TemperatureDelta{2.5};
    evaluation.permitted_concurrency = Percent{50.0};
    evaluation.permitted_execution_class = ExecutionClass::REDUCED_CLOCK;
    evaluation.reasons.add(ThermalReasonCode::DERATING_THRESHOLD_EXCEEDED);

    TG_PHASE("candidates are supplied deliberately out of order");
    evaluation.rejected.push_back(
        RejectedAlternative{DeratingLevel::NO_SAFE_EXECUTION,
                            ThermalReasonCode::CRITICAL_THRESHOLD_EXCEEDED});
    evaluation.rejected.push_back(
        RejectedAlternative{DeratingLevel::FULL_CAPABILITY,
                            ThermalReasonCode::DERATING_THRESHOLD_EXCEEDED});
    evaluation.rejected.push_back(
        RejectedAlternative{DeratingLevel::DERATED_ADMISSION,
                            ThermalReasonCode::EMERGENCY_EXECUTION_DENIED});
    evaluation.rejected.push_back(
        RejectedAlternative{DeratingLevel::DERATED_CLOCK,
                            ThermalReasonCode::TEMPERATURE_IN_WARNING_BAND});

    const ThermalPolicy policy = ThermalPolicy::make_default();
    const ThermalEnvelope envelope;
    const Explanation explanation = build_explanation(evaluation, envelope, policy);

    const ExplanationSection* rejected = nullptr;
    for (const auto& section : explanation.sections()) {
        if (section.title == "Rejected") {
            rejected = &section;
        }
    }
    TG_CHECK(rejected != nullptr);
    TG_CHECK_EQ(rejected->bullets.size(), std::size_t{4});
    TG_CHECK(rejected->bullets[0].rfind("FULL_CAPABILITY", 0) == 0);
    TG_CHECK(rejected->bullets[1].rfind("DERATED_CLOCK", 0) == 0);
    TG_CHECK(rejected->bullets[2].rfind("DERATED_ADMISSION", 0) == 0);
    TG_CHECK(rejected->bullets[3].rfind("NO_SAFE_EXECUTION", 0) == 0);

    TG_PHASE("the rendered bullets preserve that order");
    const std::vector<std::string> lines = split_lines(explanation.render());
    std::size_t previous = line_index(lines, "Rejected");
    TG_CHECK(previous < lines.size());
    const char* expected_order[] = {"FULL_CAPABILITY", "DERATED_CLOCK", "DERATED_ADMISSION",
                                    "NO_SAFE_EXECUTION"};
    for (const char* candidate : expected_order) {
        std::size_t position = lines.size();
        for (std::size_t index = previous + 1; index < lines.size(); ++index) {
            if (lines[index].rfind("  - ", 0) == 0 && lines[index].find(candidate) != std::string::npos) {
                position = index;
                break;
            }
        }
        TG_CHECK(position < lines.size());
        TG_CHECK(position >= previous);
        previous = position;
    }
}

TG_CASE(explanation, evaluation_explanation_contains_expected_keys) {
    const ThermalEvidence evidence = critical_evidence();
    const ThermalEvaluation evaluation = critical_evaluation(evidence);
    const ThermalPolicy policy = ThermalPolicy::make_default();
    const ThermalEnvelope envelope;
    const Explanation explanation = build_explanation(evaluation, envelope, policy);
    const std::string text = explanation.render();

    TG_CHECK(!text.empty());
    const char* sections[] = {"Thermal domain:", "Temperature:", "Headroom:", "Throttle:",
                             "Thermal state:",  "Restrictions:", "Rejected:", "Recovery:",
                             "Reasons:",        "Authority:"};
    for (const char* key : sections) {
        TG_CHECK(contains(text, key));
    }

    const char* lines[] = {"Label",
                           "ThermalDomainId",
                           "ThermalDomainGeneration",
                           "ThermalDomainType",
                           "Current",
                           "Warning",
                           "NearLimitStart",
                           "Derating",
                           "Critical",
                           "Recovery",
                           "VendorLimit",
                           "GoverningLimit",
                           "RawHeadroom",
                           "PolicySafetyMargin",
                           "UncertaintyMargin",
                           "EffectiveHeadroom",
                           "RecoveryMargin",
                           "Classification",
                           "State",
                           "Decision",
                           "RetainedByHysteresis",
                           "PermittedConcurrency",
                           "PermittedExecutionClass",
                           "PermittedWorkloadIntensity",
                           "Allowed",
                           "RecoveryThreshold",
                           "QualifyingSamples",
                           "QualifyingSpanMs",
                           "TemperatureBelowThreshold",
                           "HeadroomSatisfied",
                           "ThrottleClear",
                           "GenerationBindingOk",
                           "CoupledDomainsSatisfied",
                           "Codes",
                           "Provenance",
                           "CoordinatorEpoch",
                           "ThermalPolicyGeneration",
                           "TelemetryGeneration",
                           "CapabilityGeneration",
                           "TopologyGeneration"};
    for (const char* key : lines) {
        TG_CHECK(!line_value(text, key).empty());
    }

    TG_PHASE("authority values are rendered from the evaluation");
    TG_CHECK_EQ(line_value(text, "ThermalDomainId"), std::string("1"));
    TG_CHECK_EQ(line_value(text, "CoordinatorEpoch"), std::string("1"));
    TG_CHECK_EQ(line_value(text, "GoverningLimit"), std::string("88"));
    TG_CHECK_EQ(line_value(text, "Current"), std::string("95.25"));
    TG_CHECK_EQ(line_value(text, "RetainedByHysteresis"), std::string("false"));
}

TG_CASE(explanation, admission_explanation_contains_expected_keys) {
    ThermalAdmissionResult result;
    result.decision = AdmissionDecision::ADMIT_DERATED;
    result.domain = ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{4}};
    result.domain_generation =
        ThermalDomainGeneration{StrongId<thermal_governor::ThermalDomainGenerationTag>{2}};
    result.profile = WorkloadThermalProfile::HIGH_THERMAL_INTENSITY;
    result.permitted_class = ExecutionClass::REDUCED_CLOCK;
    result.permitted_concurrency = Percent{50.0};
    result.permitted_clock_ceiling = thermal_governor::MegaHertz{1200};
    result.governing_limit = DegreesCelsius{88.0};
    result.current_temperature = DegreesCelsius{80.5};
    result.effective_headroom = TemperatureDelta{4.25};
    result.required_headroom = TemperatureDelta{2.0};
    result.state = ThermalState::DERATED;
    result.derating = DeratingLevel::DERATED_CLOCK;
    result.reasons.add(ThermalReasonCode::CONCURRENCY_CEILING_APPLIED);
    result.constraints.add(MitigationIntent::REQUEST_CLOCK_REDUCTION, result.domain,
                           "thermal derating");
    result.coordinator_epoch = CoordinatorEpoch{StrongId<thermal_governor::CoordinatorEpochTag>{6}};
    result.policy_generation =
        ThermalPolicyGeneration{StrongId<thermal_governor::ThermalPolicyGenerationTag>{3}};
    result.telemetry_generation =
        TelemetryGeneration{StrongId<thermal_governor::TelemetryGenerationTag>{7}};
    result.capability_generation =
        CapabilityGeneration{StrongId<thermal_governor::CapabilityGenerationTag>{1}};
    result.topology_generation =
        TopologyGeneration{StrongId<thermal_governor::TopologyGenerationTag>{2}};

    const ThermalPolicy policy = ThermalPolicy::make_default();
    const Explanation explanation = build_admission_explanation(result, policy);
    const std::string text = explanation.render();

    TG_CHECK(!text.empty());
    const char* keys[] = {"Thermal admission:", "Headroom:",     "Restrictions:",
                          "Reasons:",           "Authority:",    "Decision",
                          "ThermalDomainId",    "ThermalDomainGeneration",
                          "WorkloadProfile",    "State",         "Derating",
                          "GoverningLimit",     "CurrentTemperature",
                          "EffectiveHeadroom",  "RequiredHeadroom",
                          "PermittedConcurrency", "PermittedExecutionClass",
                          "PermittedClockCeilingMHz", "Codes",   "CoordinatorEpoch",
                          "ThermalPolicyGeneration", "TelemetryGeneration",
                          "CapabilityGeneration", "TopologyGeneration"};
    for (const char* key : keys) {
        TG_CHECK(contains(text, key));
    }

    TG_CHECK_EQ(line_value(text, "Decision"), std::string("ADMIT_DERATED"));
    TG_CHECK_EQ(line_value(text, "WorkloadProfile"), std::string("HIGH_THERMAL_INTENSITY"));
    TG_CHECK_EQ(line_value(text, "PermittedConcurrency"), std::string("50"));
    TG_CHECK_EQ(line_value(text, "PermittedClockCeilingMHz"), std::string("1200"));
    TG_CHECK_EQ(line_value(text, "RequiredHeadroom"), std::string("2"));
    TG_CHECK_EQ(line_value(text, "EffectiveHeadroom"), std::string("4.25"));
    TG_CHECK_EQ(line_value(text, "CurrentTemperature"), std::string("80.5"));
    TG_CHECK(contains(text, "REQUEST_CLOCK_REDUCTION"));

    TG_PHASE("the admission explanation is deterministic");
    TG_CHECK_EQ(text, build_admission_explanation(result, policy).render());

    TG_PHASE("an unserialised clock ceiling renders explicitly");
    ThermalAdmissionResult unconstrained = result;
    unconstrained.permitted_clock_ceiling.reset();
    TG_CHECK(contains(build_admission_explanation(unconstrained, policy).render(),
                      "PermittedClockCeilingMHz: <unconstrained>"));
}

TG_CASE(explanation, recovery_explanation_contains_expected_keys) {
    RecoveryAssessment assessment;
    assessment.allowed = false;
    assessment.recovery_threshold = DegreesCelsius{72.0};
    assessment.recovery_margin = TemperatureDelta{1.25};
    assessment.current_temperature = DegreesCelsius{74.5};
    assessment.effective_headroom = TemperatureDelta{3.5};
    assessment.qualifying_samples = 2;
    assessment.required_samples = 3;
    assessment.qualifying_span = Milliseconds{1500};
    assessment.required_span = Milliseconds{2000};
    assessment.distinct_witnesses = 1;
    assessment.required_witnesses = 2;
    assessment.temperature_below_threshold = false;
    assessment.headroom_requirement_satisfied = true;
    assessment.throttle_clear = true;
    assessment.generation_binding_ok = true;
    assessment.coupled_domains_satisfied = false;
    assessment.explicit_authorization_present = false;
    assessment.blocking_reasons.add(ThermalReasonCode::RECOVERY_SAMPLES_INSUFFICIENT);
    assessment.blocking_reasons.add(ThermalReasonCode::RECOVERY_WITNESSES_INSUFFICIENT);

    const ThermalPolicy policy = ThermalPolicy::make_default();
    const Explanation explanation = build_recovery_explanation(assessment, policy);
    const std::string text = explanation.render();

    TG_CHECK(!text.empty());
    const char* keys[] = {"Recovery:", "Allowed",        "RecoveryThreshold",
                          "RecoveryMargin", "CurrentTemperature", "EffectiveHeadroom",
                          "QualifyingSamples", "QualifyingSpanMs", "DistinctWitnesses",
                          "TemperatureBelowThreshold", "HeadroomSatisfied",
                          "ThrottleClear", "GenerationBindingOk", "CoupledDomainsSatisfied",
                          "ExplicitAuthorization", "Reasons"};
    for (const char* key : keys) {
        TG_CHECK(contains(text, key));
    }

    TG_CHECK_EQ(line_value(text, "Allowed"), std::string("false"));
    TG_CHECK_EQ(line_value(text, "QualifyingSamples"), std::string("2/3"));
    TG_CHECK_EQ(line_value(text, "QualifyingSpanMs"), std::string("1500/2000"));
    TG_CHECK_EQ(line_value(text, "DistinctWitnesses"), std::string("1/2"));
    TG_CHECK_EQ(line_value(text, "RecoveryThreshold"), std::string("72"));
    TG_CHECK_EQ(line_value(text, "RecoveryMargin"), std::string("1.25"));
    TG_CHECK_EQ(line_value(text, "HeadroomSatisfied"), std::string("true"));
    TG_CHECK_EQ(line_value(text, "CoupledDomainsSatisfied"), std::string("false"));
    TG_CHECK(contains(text, "RECOVERY_SAMPLES_INSUFFICIENT"));
    TG_CHECK(contains(text, "RECOVERY_WITNESSES_INSUFFICIENT"));

    TG_PHASE("an allowed recovery renders true and stays deterministic");
    RecoveryAssessment allowed = assessment;
    allowed.allowed = true;
    allowed.blocking_reasons = thermal_governor::ReasonCodeSet{};
    const std::string allowed_text = build_recovery_explanation(allowed, policy).render();
    TG_CHECK_EQ(line_value(allowed_text, "Allowed"), std::string("true"));
    TG_CHECK_EQ(allowed_text, build_recovery_explanation(allowed, policy).render());
    TG_CHECK(allowed_text != text);
}
