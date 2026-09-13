// Thermal Governor - example: deterministic explanation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Evaluation is a pure function of its inputs, so two explanations built over
// identical inputs must render byte-identical text. This example renders the
// same evaluation twice and asserts byte equality. A ManualClock is used so
// that the freshness computation cannot drift between the two renders.

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

void report(bool condition, const std::string& what) {
    std::cout << (condition ? "[ ok ] " : "[FAIL] ") << what << '\n';
    if (!condition) {
        ++g_failures;
    }
}

std::string number(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

std::uint64_t fnv1a(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(character));
        hash *= 1099511628211ULL;
    }
    return hash;
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
    evidence.confidence = 0.9;
    return evidence;
}

}  // namespace

int main() {
    std::cout << "== Thermal Governor example: deterministic explanation ==\n";

    auto clock = std::make_shared<ManualClock>(SteadyTimePoint{std::chrono::seconds{700}});
    GovernorConfig config;
    config.clock = clock;
    auto created = ThermalGovernor::create(config);
    if (!created) {
        std::cerr << "fatal: governor creation failed: " << created.error().render() << '\n';
        return 1;
    }
    std::unique_ptr<ThermalGovernor> governor = std::move(created.value());

    const DeviceId device{StrongId<DeviceIdTag>{5}};
    const DeviceGeneration device_generation{StrongId<DeviceGenerationTag>{1}};
    const ThermalDomainId domain{StrongId<ThermalDomainIdTag>{5}};
    const ThermalDomainGeneration domain_generation{StrongId<ThermalDomainGenerationTag>{1}};

    DeviceRegistration registration;
    registration.device = device;
    registration.generation = device_generation;
    registration.label = Label{"synthetic-accelerator-explain"};
    registration.provenance = Provenance::SYNTHETIC;
    registration.capabilities.set(ThermalCapability::TEMPERATURE,
                                  CapabilityState::SUPPORTED_SYNTHETIC);
    registration.capabilities.set(ThermalCapability::CURRENT_CLOCK,
                                  CapabilityState::SUPPORTED_SYNTHETIC);
    registration.capabilities.set(ThermalCapability::MAX_CLOCK,
                                  CapabilityState::SUPPORTED_SYNTHETIC);
    if (!governor->register_device(registration).ok()) {
        std::cerr << "fatal: register_device failed\n";
        return 1;
    }

    ThermalDomainDefinition definition;
    definition.id = domain;
    definition.generation = domain_generation;
    definition.type = ThermalDomainType::ACCELERATOR;
    definition.label = Label{"accelerator-explain"};
    definition.provenance = Provenance::SYNTHETIC;
    definition.evidence_source = "synthetic temperature model";
    definition.members.push_back(
        DomainMember{SubjectRef::for_device(device, device_generation), 1.0});
    if (!governor->register_thermal_domain(definition).ok()) {
        std::cerr << "fatal: register_thermal_domain failed\n";
        return 1;
    }

    auto primed = governor->publish_temperature(make_sample(
        device, device_generation, domain, domain_generation, 60.0, 1, clock->now(),
        clock->wall_now(), governor->epoch()));
    report(primed.has_value(), "publish_temperature accepted a 60.00 C priming sample");

    ThermalEvidence evidence = make_sample(device, device_generation, domain, domain_generation,
                                          84.0, 2, clock->now(), clock->wall_now(),
                                          governor->epoch());
    evidence.current_clock = MegaHertz{1800};
    evidence.max_clock = MegaHertz{2100};
    auto published = governor->publish_temperature(evidence);
    report(published.has_value(), "publish_temperature accepted 84.00 C with clock telemetry");

    std::cout << "step 1: first explanation\n";
    auto first = governor->explain(domain);
    report(first.has_value(), "explain returned the first explanation");

    std::cout << "step 2: second explanation over identical inputs\n";
    auto second = governor->explain(domain);
    report(second.has_value(), "explain returned the second explanation");

    if (!first || !second) {
        std::cout << "RESULT: FAILURE\n";
        return 1;
    }

    const std::string rendered_first = first.value().render();
    const std::string rendered_second = second.value().render();

    std::cout << "step 3: structural equality\n";
    report(first.value() == second.value(), "the two Explanation structures compare equal");
    report(first.value().sections().size() == second.value().sections().size(),
           "both explanations expose the same section count");
    std::cout << "       sections=" << first.value().sections().size() << '\n';

    std::cout << "step 4: byte equality of the canonical rendering\n";
    std::cout << "       first_bytes=" << rendered_first.size()
              << " second_bytes=" << rendered_second.size() << '\n';
    std::cout << "       first_fnv1a=" << fnv1a(rendered_first)
              << " second_fnv1a=" << fnv1a(rendered_second) << '\n';
    report(rendered_first == rendered_second,
           "the two renderings are byte-identical");
    report(!rendered_first.empty(), "the rendering is not empty");
    report(rendered_first.size() <= Explanation::kMaxRenderedBytes,
           "the rendering stays inside the documented byte bound");

    std::cout << "--- canonical rendering ---\n" << rendered_first;

    std::cout << "step 5: the governed temperature is unchanged by rendering\n";
    auto evaluation = governor->evaluate_domain(domain);
    if (evaluation) {
        std::cout << "       state=" << to_string(evaluation.value().state)
                  << " decision=" << to_string(evaluation.value().decision)
                  << " permitted_clock_ceiling=";
        if (evaluation.value().permitted_clock_ceiling.has_value()) {
            std::cout << evaluation.value().permitted_clock_ceiling->value() << " MHz";
        } else {
            std::cout << "<unconstrained>";
        }
        std::cout << '\n';
    }

    if (!governor->shutdown().ok()) {
        std::cerr << "fatal: shutdown failed\n";
        return 1;
    }
    std::cout << (g_failures == 0 ? "RESULT: SUCCESS\n" : "RESULT: FAILURE\n");
    return g_failures == 0 ? 0 : 1;
}
