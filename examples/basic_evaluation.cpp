// Thermal Governor - example: basic synthetic device and domain evaluation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Registers one synthetic accelerator device and one accelerator thermal
// domain, publishes temperature evidence, evaluates the domain and prints the
// resulting state and decision. Exits 0 only when every step succeeded.

#include "thermal_governor/thermal_governor.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

using namespace thermal_governor;

int g_failures = 0;

std::string number(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

void report(bool condition, const std::string& what) {
    std::cout << (condition ? "[ ok ] " : "[FAIL] ") << what << '\n';
    if (!condition) {
        ++g_failures;
    }
}

void report_status(const Status& status, const std::string& what) {
    if (status.ok()) {
        std::cout << "[ ok ] " << what << '\n';
        return;
    }
    std::cout << "[FAIL] " << what << ": " << status.error().render() << '\n';
    ++g_failures;
}

ThermalEvidence make_sample(DeviceId device, DeviceGeneration device_generation,
                            ThermalDomainId domain, ThermalDomainGeneration domain_generation,
                            double temperature, std::uint64_t sequence,
                            SteadyTimePoint measured_at, WallTimePoint measured_wall_at,
                            CoordinatorEpoch epoch) {
    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{sequence}};
    evidence.subject = SubjectRef::for_device(device, device_generation);
    evidence.domain = domain;
    evidence.domain_generation = domain_generation;
    evidence.temperature = DegreesCelsius{temperature};
    evidence.source = MeasurementSource::SYNTHETIC_MODEL;
    evidence.provenance = Provenance::SYNTHETIC;
    evidence.measurement_sequence = sequence;
    evidence.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{sequence}};
    evidence.coordinator_epoch = epoch;
    evidence.measured_at = measured_at;
    evidence.measured_wall_at = measured_wall_at;
    evidence.integrity = IntegrityStatus::OK;
    evidence.confidence = 0.95;
    return evidence;
}

}  // namespace

int main() {
    std::cout << "== Thermal Governor example: basic evaluation ==\n";

    auto clock = std::make_shared<ManualClock>(SteadyTimePoint{std::chrono::seconds{100}});
    GovernorConfig config;
    config.clock = clock;

    auto created = ThermalGovernor::create(config);
    if (!created) {
        std::cerr << "fatal: governor creation failed: " << created.error().render() << '\n';
        return 1;
    }
    std::unique_ptr<ThermalGovernor> governor = std::move(created.value());

    const DeviceId device{StrongId<DeviceIdTag>{1}};
    const DeviceGeneration device_generation{StrongId<DeviceGenerationTag>{1}};
    const ThermalDomainId domain{StrongId<ThermalDomainIdTag>{1}};
    const ThermalDomainGeneration domain_generation{StrongId<ThermalDomainGenerationTag>{1}};

    DeviceRegistration registration;
    registration.device = device;
    registration.generation = device_generation;
    registration.label = Label{"synthetic-accelerator-0"};
    registration.provenance = Provenance::SYNTHETIC;
    registration.capabilities.set(ThermalCapability::TEMPERATURE,
                                  CapabilityState::SUPPORTED_SYNTHETIC,
                                  "synthetic temperature model");
    registration.capabilities.set(ThermalCapability::THROTTLE_REASONS,
                                  CapabilityState::SUPPORTED_SYNTHETIC,
                                  "synthetic throttle model");
    report_status(governor->register_device(registration), "register_device");

    ThermalDomainDefinition definition;
    definition.id = domain;
    definition.generation = domain_generation;
    definition.type = ThermalDomainType::ACCELERATOR;
    definition.label = Label{"accelerator-0"};
    definition.provenance = Provenance::SYNTHETIC;
    definition.evidence_source = "synthetic temperature model";
    definition.aggregation = AggregationRule::HOTTEST_MEMBER_GOVERNS;
    definition.members.push_back(
        DomainMember{SubjectRef::for_device(device, device_generation), 1.0});
    report_status(governor->register_thermal_domain(definition), "register_thermal_domain");

    auto cool = governor->publish_temperature(
        make_sample(device, device_generation, domain, domain_generation, 62.0, 1, clock->now(),
                    clock->wall_now(), governor->epoch()));
    report(cool.has_value(), "publish_temperature accepted 62.00 C");
    if (cool) {
        report(cool.value().accepted, "receipt reports the observation was accepted");
        std::cout << "       comparison=" << to_string(cool.value().comparison)
                  << " telemetry_generation=" << cool.value().telemetry_generation.value()
                  << " affected_domains=" << cool.value().affected_domains.size() << '\n';
    }

    auto cool_evaluation = governor->evaluate_domain(domain);
    report(cool_evaluation.has_value(), "evaluate_domain at 62.00 C");
    if (cool_evaluation) {
        const ThermalEvaluation& evaluation = cool_evaluation.value();
        std::cout << "       state=" << to_string(evaluation.state)
                  << " decision=" << to_string(evaluation.decision)
                  << " derating=" << to_string(evaluation.derating) << '\n';
        std::cout << "       temperature=" << evaluation.headroom.current_temperature.render()
                  << " governing_limit=" << evaluation.headroom.governing_limit.render()
                  << " effective_headroom="
                  << number(evaluation.headroom.effective_headroom.value()) << '\n';
        std::cout << "       permitted_concurrency="
                  << number(evaluation.permitted_concurrency.value())
                  << " permitted_class=" << to_string(evaluation.permitted_execution_class)
                  << '\n';
        std::cout << "       reasons=" << evaluation.reasons.render() << '\n';
        report(evaluation.state == ThermalState::NORMAL, "62.00 C evaluates to NORMAL");
        report(evaluation.decision == ThermalDecision::ALLOW, "62.00 C decides ALLOW");
    }

    auto hot = governor->publish_temperature(
        make_sample(device, device_generation, domain, domain_generation, 85.0, 2, clock->now(),
                    clock->wall_now(), governor->epoch()));
    report(hot.has_value(), "publish_temperature accepted 85.00 C");

    auto hot_evaluation = governor->evaluate_domain(domain);
    report(hot_evaluation.has_value(), "evaluate_domain at 85.00 C");
    if (hot_evaluation) {
        const ThermalEvaluation& evaluation = hot_evaluation.value();
        std::cout << "       state=" << to_string(evaluation.state)
                  << " decision=" << to_string(evaluation.decision)
                  << " derating=" << to_string(evaluation.derating) << '\n';
        std::cout << "       temperature=" << evaluation.headroom.current_temperature.render()
                  << " raw_headroom=" << number(evaluation.headroom.raw_headroom.value())
                  << " effective_headroom="
                  << number(evaluation.headroom.effective_headroom.value()) << '\n';
        std::cout << "       permitted_concurrency="
                  << number(evaluation.permitted_concurrency.value())
                  << " reasons=" << evaluation.reasons.render() << '\n';
        report(evaluation.state == ThermalState::DERATED, "85.00 C evaluates to DERATED");
        report(evaluation.decision == ThermalDecision::ALLOW_DERATED,
               "85.00 C decides ALLOW_DERATED");
    }

    report_status(governor->shutdown(), "shutdown");
    std::cout << (g_failures == 0 ? "RESULT: SUCCESS\n" : "RESULT: FAILURE\n");
    return g_failures == 0 ? 0 : 1;
}
