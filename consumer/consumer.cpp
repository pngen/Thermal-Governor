// Thermal Governor — independent downstream consumer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Uses only installed public headers and the exported CMake target.

#include <cstdio>
#include <memory>
#include <string>

#include <thermal_governor/thermal_governor.hpp>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("[ %s ] %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

}  // namespace

int main() {
    using namespace thermal_governor;

    std::printf("Thermal Governor consumer, linked against %s\n", build_identity().c_str());

    auto clock = std::make_shared<ManualClock>(SteadyTimePoint{});
    GovernorConfig config;
    config.coordinator = CoordinatorId{StrongId<CoordinatorIdTag>{1}};
    config.clock = clock;

    auto created = ThermalGovernor::create(std::move(config));
    check(created.has_value(), "ThermalGovernor::create through the installed package");
    if (!created.has_value()) {
        return 1;
    }
    std::unique_ptr<ThermalGovernor> governor = std::move(created.value());

    DeviceRegistration device;
    device.device = DeviceId{StrongId<DeviceIdTag>{1}};
    device.generation = DeviceGeneration{StrongId<DeviceGenerationTag>{1}};
    device.node = NodeId{StrongId<NodeIdTag>{1}};
    device.node_generation = NodeGeneration{StrongId<NodeGenerationTag>{1}};
    device.rack = RackId{StrongId<RackIdTag>{1}};
    device.rack_generation = RackGeneration{StrongId<RackGenerationTag>{1}};
    device.label = Label{"consumer-device"};
    device.provenance = Provenance::SYNTHETIC;
    device.capabilities.set(ThermalCapability::TEMPERATURE, CapabilityState::SUPPORTED_SYNTHETIC);
    check(governor->register_device(device).ok(), "register_device");

    ThermalDomainDefinition definition;
    definition.id = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    definition.generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    definition.type = ThermalDomainType::ACCELERATOR;
    definition.label = Label{"consumer-domain"};
    definition.provenance = Provenance::SYNTHETIC;
    definition.evidence_source = "consumer test device";
    definition.policy = ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}};
    DomainMember member;
    member.subject = SubjectRef::for_device(
        DeviceId{StrongId<DeviceIdTag>{1}}, DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    definition.members.push_back(member);
    check(governor->register_thermal_domain(definition).ok(), "register_thermal_domain");

    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{1}};
    evidence.subject = member.subject;
    evidence.domain = definition.id;
    evidence.domain_generation = definition.generation;
    evidence.temperature = DegreesCelsius{84.0};
    evidence.source = MeasurementSource::SYNTHETIC_MODEL;
    evidence.provenance = Provenance::SYNTHETIC;
    evidence.measurement_sequence = 1;
    evidence.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{1}};
    evidence.coordinator_epoch = governor->epoch();
    evidence.measured_at = clock->now();
    evidence.measured_wall_at = clock->wall_now();

    auto receipt = governor->publish_temperature(evidence);
    check(receipt.has_value() && receipt.value().accepted, "publish_temperature");

    auto evaluation = governor->evaluate_domain(definition.id);
    check(evaluation.has_value(), "evaluate_domain");
    if (evaluation.has_value()) {
        check(evaluation.value().state == ThermalState::DERATED,
              "84 C evaluates to DERATED under the default policy");
        check(evaluation.value().decision == ThermalDecision::ALLOW_DERATED,
              "a derated domain decides ALLOW_DERATED");
        check(evaluation.value().headroom.effective_headroom.value() > 0.0,
              "effective headroom is reported explicitly");
    }

    auto envelope = governor->query_envelope(definition.id);
    check(envelope.has_value(), "query_envelope");
    if (envelope.has_value()) {
        check(envelope.value().domain_generation.value() == 1U,
              "the envelope is generation-bound");
        check(!envelope.value().permits_full_capability(),
              "a derated envelope does not claim full capability");
    }

    auto explanation = governor->explain(definition.id);
    check(explanation.has_value() && !explanation.value().render().empty(),
          "explain produces a deterministic explanation");

    ThermalAdmissionRequest admission;
    admission.domain = definition.id;
    admission.workload = WorkloadId{StrongId<WorkloadIdTag>{7}};
    admission.profile = WorkloadThermalProfile::MODERATE_THERMAL_INTENSITY;
    auto admission_result = governor->evaluate_admission(admission);
    check(admission_result.has_value(), "evaluate_admission");
    if (admission_result.has_value()) {
        check(admission_result.value().decision != AdmissionDecision::UNKNOWN,
              "thermal admission returns a typed decision");
    }

    check(governor->shutdown().ok(), "shutdown");

    std::printf("consumer result: %s\n", g_failures == 0 ? "SUCCESS" : "FAILURE");
    return g_failures == 0 ? 0 : 1;
}
