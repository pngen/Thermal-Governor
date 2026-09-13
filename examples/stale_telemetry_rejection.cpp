// Thermal Governor - example: stale and conflicting telemetry rejection.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Untrusted telemetry reaches the runtime through publish_temperature. Every
// rejection is a typed error code rather than a bool: a stale coordinator
// epoch, a stale worker boot identity, a stale device generation and a
// conflicting duplicate each produce their own machine-readable code.
//
// Fence order inside the runtime is fixed and is printed below.

#include "thermal_governor/thermal_governor.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

namespace {

using namespace thermal_governor;

int g_failures = 0;

void report(bool condition, const std::string& what) {
    std::cout << (condition ? "[ ok ] " : "[FAIL] ") << what << '\n';
    if (!condition) {
        ++g_failures;
    }
}

ThermalEvidence make_sample(DeviceId device, DeviceGeneration device_generation,
                            ThermalDomainId domain, ThermalDomainGeneration domain_generation,
                            WorkerId worker, WorkerBootId worker_boot, CoordinatorEpoch epoch,
                            std::uint64_t evidence_id, double temperature,
                            std::uint64_t measurement_sequence,
                            std::uint64_t telemetry_generation, SteadyTimePoint measured_at,
                            WallTimePoint measured_wall_at) {
    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{evidence_id}};
    evidence.subject = SubjectRef::for_device(device, device_generation);
    evidence.domain = domain;
    evidence.domain_generation = domain_generation;
    evidence.temperature = DegreesCelsius{temperature};
    evidence.source = MeasurementSource::SYNTHETIC_MODEL;
    evidence.provenance = Provenance::SYNTHETIC;
    evidence.measurement_sequence = measurement_sequence;
    evidence.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{telemetry_generation}};
    evidence.worker = worker;
    evidence.worker_boot = worker_boot;
    evidence.coordinator_epoch = epoch;
    evidence.measured_at = measured_at;
    evidence.measured_wall_at = measured_wall_at;
    evidence.integrity = IntegrityStatus::OK;
    return evidence;
}

void report_rejection(const Result<EvidenceReceipt>& result, ThermalErrorCode expected,
                      const std::string& what) {
    if (result) {
        std::cout << "[FAIL] " << what << ": expected a rejection but the evidence was accepted\n";
        ++g_failures;
        return;
    }
    const ThermalError& error = result.error();
    std::cout << "       rejected with code=" << to_string(error.code)
              << " detail=\"" << error.detail << "\"\n";
    report(error.code == expected,
           what + " produces " + std::string(to_string(expected)));
}

}  // namespace

int main() {
    std::cout << "== Thermal Governor example: stale telemetry rejection ==\n";

    auto clock = std::make_shared<ManualClock>(SteadyTimePoint{std::chrono::seconds{400}});
    GovernorConfig config;
    config.clock = clock;
    config.initial_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{5}};
    auto created = ThermalGovernor::create(config);
    if (!created) {
        std::cerr << "fatal: governor creation failed: " << created.error().render() << '\n';
        return 1;
    }
    std::unique_ptr<ThermalGovernor> governor = std::move(created.value());

    const DeviceId device{StrongId<DeviceIdTag>{1}};
    const DeviceGeneration device_generation{StrongId<DeviceGenerationTag>{2}};
    const DeviceGeneration previous_device_generation{StrongId<DeviceGenerationTag>{1}};
    const ThermalDomainId domain{StrongId<ThermalDomainIdTag>{1}};
    const ThermalDomainGeneration domain_generation{StrongId<ThermalDomainGenerationTag>{1}};
    const WorkerId worker{StrongId<WorkerIdTag>{7}};
    const WorkerBootId boot{StrongId<WorkerBootIdTag>{2}};
    const WorkerBootId previous_boot{StrongId<WorkerBootIdTag>{1}};
    const CoordinatorEpoch epoch = governor->epoch();
    const CoordinatorEpoch previous_epoch{StrongId<CoordinatorEpochTag>{4}};

    std::cout << "registered authority: coordinator_epoch=" << epoch.value() << " worker="
              << worker.value() << " boot=" << boot.value() << " device=" << device.value()
              << " device_generation=" << device_generation.value() << '\n';
    std::cout << "fence order: structure -> coordinator epoch -> worker boot -> device"
              << " generation -> domain generation -> duplicate comparison\n";

    DeviceRegistration registration;
    registration.device = device;
    registration.generation = device_generation;
    registration.label = Label{"synthetic-accelerator-fenced"};
    registration.provenance = Provenance::SYNTHETIC;
    registration.capabilities.set(ThermalCapability::TEMPERATURE,
                                  CapabilityState::SUPPORTED_SYNTHETIC);
    if (!governor->register_device(registration).ok()) {
        std::cerr << "fatal: register_device failed\n";
        return 1;
    }
    if (!governor->register_worker(worker, boot, Label{"synthetic-worker"}).ok()) {
        std::cerr << "fatal: register_worker failed\n";
        return 1;
    }

    ThermalDomainDefinition definition;
    definition.id = domain;
    definition.generation = domain_generation;
    definition.type = ThermalDomainType::ACCELERATOR;
    definition.label = Label{"accelerator-fenced"};
    definition.provenance = Provenance::SYNTHETIC;
    definition.members.push_back(
        DomainMember{SubjectRef::for_device(device, device_generation), 1.0});
    if (!governor->register_thermal_domain(definition).ok()) {
        std::cerr << "fatal: register_thermal_domain failed\n";
        return 1;
    }

    std::cout << "step 1: accept one authoritative observation\n";
    auto first = governor->publish_temperature(make_sample(
        device, device_generation, domain, domain_generation, worker, boot, epoch, 1, 60.0, 1, 1,
        clock->now(), clock->wall_now()));
    report(first.has_value(), "the first observation is accepted");
    if (first) {
        std::cout << "       comparison=" << to_string(first.value().comparison)
                  << " accepted=" << (first.value().accepted ? "true" : "false") << '\n';
    }

    std::cout << "step 2: coordinator epoch predates the running coordinator\n";
    auto stale_epoch = governor->publish_temperature(make_sample(
        device, device_generation, domain, domain_generation, worker, boot, previous_epoch, 2, 61.0,
        2, 2, clock->now(), clock->wall_now()));
    report_rejection(stale_epoch, ThermalErrorCode::STALE_EPOCH, "a stale coordinator epoch");

    std::cout << "step 3: worker boot identity was superseded\n";
    auto stale_worker = governor->publish_temperature(make_sample(
        device, device_generation, domain, domain_generation, worker, previous_boot, epoch, 3, 61.0,
        3, 3, clock->now(), clock->wall_now()));
    report_rejection(stale_worker, ThermalErrorCode::STALE_WORKER, "a stale worker boot identity");

    std::cout << "step 4: device generation was superseded\n";
    auto stale_device = governor->publish_temperature(make_sample(
        device, previous_device_generation, domain, domain_generation, worker, boot, epoch, 4, 61.0,
        4, 4, clock->now(), clock->wall_now()));
    report_rejection(stale_device, ThermalErrorCode::STALE_DEVICE_GENERATION,
                     "a stale device generation");

    std::cout << "step 5: accept a second authoritative observation\n";
    const ThermalEvidence second = make_sample(device, device_generation, domain, domain_generation,
                                               worker, boot, epoch, 10, 61.0, 2, 2, clock->now(),
                                               clock->wall_now());
    auto second_receipt = governor->publish_temperature(second);
    report(second_receipt.has_value(), "the second observation is accepted");

    std::cout << "step 6: the same measurement position claims divergent content\n";
    const ThermalEvidence conflict = make_sample(device, device_generation, domain,
                                                 domain_generation, worker, boot, epoch, 10, 99.0,
                                                 2, 2, clock->now(), clock->wall_now());
    std::cout << "       comparison=" << to_string(compare_evidence(second, conflict)) << '\n';
    auto conflict_result = governor->publish_temperature(conflict);
    report_rejection(conflict_result, ThermalErrorCode::DUPLICATE_CONFLICT,
                     "a conflicting duplicate");

    std::cout << "step 7: telemetry position predates the observation already held\n";
    auto superseded = governor->publish_temperature(make_sample(
        device, device_generation, domain, domain_generation, worker, boot, epoch, 11, 61.0, 1, 1,
        clock->now(), clock->wall_now()));
    report_rejection(superseded, ThermalErrorCode::STALE_TELEMETRY, "superseded telemetry");

    std::cout << "step 8: the held evidence is unchanged by every rejection\n";
    auto evaluation = governor->evaluate_domain(domain);
    report(evaluation.has_value(), "the domain still evaluates after the rejections");
    if (evaluation) {
        std::cout << "       state=" << to_string(evaluation.value().state)
                  << " temperature=" << evaluation.value().headroom.current_temperature.render()
                  << " decision=" << to_string(evaluation.value().decision) << '\n';
        report(evaluation.value().headroom.current_temperature.value() == 61.0,
               "the rejected observations did not replace the held evidence");
    }

    if (!governor->shutdown().ok()) {
        std::cerr << "fatal: shutdown failed\n";
        return 1;
    }
    std::cout << (g_failures == 0 ? "RESULT: SUCCESS\n" : "RESULT: FAILURE\n");
    return g_failures == 0 ? 0 : 1;
}
