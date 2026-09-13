// Thermal Governor - example: recovery gating.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Recovery is never implicit. This example shows a derated domain being
// refused recovery while the policy requirements are unmet, and then being
// granted recovery once the consecutive-sample count and the minimum
// evidence span are satisfied. Duration-based stability uses recorded
// sample timestamps on a ManualClock rather than sleeping.

#include "thermal_governor/thermal_governor.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
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

void print_block(const std::string& title, const std::string& body) {
    std::cout << "--- " << title << " ---\n" << body;
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
    evidence.confidence = 1.0;
    return evidence;
}

}  // namespace

int main() {
    std::cout << "== Thermal Governor example: recovery gating ==\n";

    auto clock = std::make_shared<ManualClock>(SteadyTimePoint{std::chrono::seconds{500}});
    GovernorConfig config;
    config.clock = clock;
    auto created = ThermalGovernor::create(config);
    if (!created) {
        std::cerr << "fatal: governor creation failed: " << created.error().render() << '\n';
        return 1;
    }
    std::unique_ptr<ThermalGovernor> governor = std::move(created.value());

    const DeviceId device{StrongId<DeviceIdTag>{3}};
    const DeviceGeneration device_generation{StrongId<DeviceGenerationTag>{1}};
    const ThermalDomainId domain{StrongId<ThermalDomainIdTag>{3}};
    const ThermalDomainGeneration domain_generation{StrongId<ThermalDomainGenerationTag>{1}};

    DeviceRegistration registration;
    registration.device = device;
    registration.generation = device_generation;
    registration.label = Label{"synthetic-accelerator-2"};
    registration.provenance = Provenance::SYNTHETIC;
    registration.capabilities.set(ThermalCapability::TEMPERATURE,
                                  CapabilityState::SUPPORTED_SYNTHETIC);
    if (!governor->register_device(registration).ok()) {
        std::cerr << "fatal: register_device failed\n";
        return 1;
    }

    ThermalDomainDefinition definition;
    definition.id = domain;
    definition.generation = domain_generation;
    definition.type = ThermalDomainType::ACCELERATOR;
    definition.label = Label{"accelerator-2"};
    definition.provenance = Provenance::SYNTHETIC;
    definition.members.push_back(
        DomainMember{SubjectRef::for_device(device, device_generation), 1.0});
    if (!governor->register_thermal_domain(definition).ok()) {
        std::cerr << "fatal: register_thermal_domain failed\n";
        return 1;
    }

    std::uint64_t sequence = 0;
    const auto publish = [&](double temperature, std::int64_t advance_ms) {
        clock->advance(Milliseconds{advance_ms});
        ++sequence;
        auto receipt = governor->publish_temperature(make_sample(
            device, device_generation, domain, domain_generation, temperature, sequence,
            clock->now(), clock->wall_now(), governor->epoch()));
        if (!receipt) {
            std::cout << "[FAIL] publish at " << number(temperature) << " C: "
                      << receipt.error().render() << '\n';
            ++g_failures;
        }
    };

    // The assessment below is read from a live evaluation. The stored
    // assessment returned by evaluate_recovery(domain) only advances when the
    // evaluated state, derating level or decision changes, so it can lag
    // while hysteresis retains the previous envelope.
    const auto probe = [&](const std::string& title) -> std::optional<RecoveryAssessment> {
        auto evaluation = governor->evaluate_domain(domain);
        if (!evaluation) {
            std::cout << "[FAIL] evaluate_domain: " << evaluation.error().render() << '\n';
            ++g_failures;
            return std::nullopt;
        }
        std::cout << "       state=" << to_string(evaluation.value().state) << '\n';
        if (!title.empty()) {
            print_block(title, evaluation.value().recovery.render());
        }
        return evaluation.value().recovery;
    };

    std::cout << "step 1: drive the domain above the derating threshold\n";
    publish(84.0, 0);
    const auto hot = probe("recovery while the temperature is above the recovery gate");
    if (hot.has_value()) {
        report(!hot->allowed, "recovery is forbidden while hot");
        report(hot->blocking_reasons.contains(ThermalReasonCode::RECOVERY_THRESHOLD_EXCEEDED),
               "the blocking reason names the exceeded recovery threshold");
    }

    std::cout << "step 2: first sample below the recovery threshold\n";
    publish(65.0, 1000);
    const auto one = probe("recovery after one qualifying sample");
    if (one.has_value()) {
        report(!one->allowed, "recovery is forbidden after one sample");
        report(one->temperature_below_threshold,
               "the temperature predicate alone is not sufficient");
        report(one->qualifying_samples == 1, "exactly one sample qualifies");
        report(one->blocking_reasons.contains(ThermalReasonCode::RECOVERY_SAMPLES_INSUFFICIENT),
               "the blocking reason names the insufficient sample count");
        report(one->blocking_reasons.contains(
                   ThermalReasonCode::RECOVERY_EVIDENCE_SPAN_INSUFFICIENT),
               "the blocking reason names the insufficient evidence span");
    }

    std::cout << "step 3: second consecutive qualifying sample\n";
    publish(65.0, 1000);
    const auto two = probe("recovery after two qualifying samples");
    if (two.has_value()) {
        report(!two->allowed, "recovery is still forbidden after two samples");
        report(two->qualifying_samples == 2, "two samples qualify but three are required");
    }

    std::cout << "step 4: third consecutive qualifying sample completes the span\n";
    publish(65.0, 1000);
    const auto three = probe("recovery after three qualifying samples");
    if (three.has_value()) {
        report(three->allowed, "recovery is permitted once policy is satisfied");
        report(three->qualifying_samples >= three->required_samples,
               "the consecutive-sample requirement is met");
        report(three->qualifying_span >= three->required_span,
               "the evidence-span requirement is met");
    }

    std::cout << "step 5: the released envelope\n";
    auto state = governor->query_state(domain);
    report(state.has_value() && state.value() == ThermalState::NORMAL,
           "the domain returned to NORMAL");
    auto envelope = governor->query_envelope(domain);
    if (envelope) {
        std::cout << "       state=" << to_string(envelope.value().state)
                  << " decision=" << to_string(envelope.value().decision)
                  << " derating=" << to_string(envelope.value().derating)
                  << " permitted_concurrency="
                  << number(envelope.value().permitted_concurrency.value()) << '\n';
        report(envelope.value().permits_full_capability(),
               "the released envelope permits full capability");
    }

    auto stored = governor->evaluate_recovery(domain);
    report(stored.has_value() && stored.value().allowed,
           "the stored assessment agrees once the state has settled");
    std::cout << "       note: the stored assessment advances only when state, derating or"
              << " decision changes\n";

    if (!governor->shutdown().ok()) {
        std::cerr << "fatal: shutdown failed\n";
        return 1;
    }
    std::cout << (g_failures == 0 ? "RESULT: SUCCESS\n" : "RESULT: FAILURE\n");
    return g_failures == 0 ? 0 : 1;
}
