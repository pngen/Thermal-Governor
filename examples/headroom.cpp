// Thermal Governor - example: explicit headroom breakdown.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Shows raw, safety, uncertainty and effective headroom together with the
// governing limit, first as a pure computation and then inside a live
// evaluation performed by the governor.

#include "thermal_governor/thermal_governor.hpp"

#include <chrono>
#include <cmath>
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

void print_block(const std::string& title, const std::string& body) {
    std::cout << "--- " << title << " ---\n" << body;
}

bool close_to(double left, double right) { return std::fabs(left - right) < 1e-9; }

ThermalEvidence make_sample(DeviceId device, DeviceGeneration device_generation,
                            ThermalDomainId domain, ThermalDomainGeneration domain_generation,
                            double temperature, double limit, std::uint64_t sequence,
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
    evidence.confidence = 0.5;
    evidence.temperature_limit = DegreesCelsius{limit};
    return evidence;
}

}  // namespace

int main() {
    std::cout << "== Thermal Governor example: headroom ==\n";

    // Case 1: a REAL vendor limit below the policy limit governs.
    HeadroomInput vendor_case;
    vendor_case.current_temperature = DegreesCelsius{78.0};
    vendor_case.policy_critical = DegreesCelsius{88.0};
    vendor_case.vendor_limit = DegreesCelsius{83.0};
    vendor_case.vendor_limit_provenance = Provenance::REAL;
    vendor_case.policy_safety_margin = TemperatureDelta{2.0};
    vendor_case.uncertainty_margin = uncertainty_from_confidence(0.5, TemperatureDelta{2.0});
    vendor_case.recovery_threshold = DegreesCelsius{72.0};
    vendor_case.recovery_margin = TemperatureDelta{1.0};

    const HeadroomBreakdown vendor_breakdown = compute_headroom(vendor_case);
    print_block("REAL vendor limit 83.00 C governs over policy critical 88.00 C",
                render_headroom(vendor_breakdown));
    report(close_to(vendor_breakdown.governing_limit.value(), 83.0),
           "governing limit is the lower REAL vendor limit (83.00 C)");
    report(close_to(vendor_breakdown.raw_headroom.value(), 5.0), "raw headroom is 83.00 - 78.00");
    report(close_to(vendor_breakdown.uncertainty_margin.value(), 1.0),
           "confidence 0.5 halves the configured 2.00 C uncertainty");
    report(close_to(vendor_breakdown.effective_headroom.value(), 2.0),
           "effective headroom is raw minus safety minus uncertainty");
    report(vendor_breakdown.has_positive_headroom(), "the effective headroom is positive");

    // Case 2: a synthetic limit must never tighten the policy ceiling.
    HeadroomInput synthetic_case = vendor_case;
    synthetic_case.vendor_limit = DegreesCelsius{70.0};
    synthetic_case.vendor_limit_provenance = Provenance::SYNTHETIC;
    const HeadroomBreakdown synthetic_breakdown = compute_headroom(synthetic_case);
    print_block("SYNTHETIC limit 70.00 C is reported but cannot override policy",
                render_headroom(synthetic_breakdown));
    report(close_to(synthetic_breakdown.governing_limit.value(), 88.0),
           "a SYNTHETIC limit never tightens the governing limit");
    report(synthetic_breakdown.vendor_limit.has_value(),
           "the reported vendor limit is still exposed honestly");

    // Case 3: honest deficit. Effective headroom is deliberately unclamped.
    HeadroomInput deficit_case = vendor_case;
    deficit_case.current_temperature = DegreesCelsius{87.0};
    deficit_case.uncertainty_margin = TemperatureDelta{0.0};
    const HeadroomBreakdown deficit_breakdown = compute_headroom(deficit_case);
    print_block("Temperature above the governing limit yields a negative deficit",
                render_headroom(deficit_breakdown));
    report(!deficit_breakdown.has_positive_headroom(),
           "a negative effective headroom is an honest deficit, not zero");
    report(deficit_breakdown.raw_headroom.value() < 0.0, "raw headroom is negative");
    report(!deficit_breakdown.recovery_headroom_satisfied(),
           "the temperature is above the recovery gate");

    // Live runtime: the same breakdown is produced by a real evaluation.
    auto clock = std::make_shared<ManualClock>(SteadyTimePoint{std::chrono::seconds{200}});
    GovernorConfig config;
    config.clock = clock;
    auto created = ThermalGovernor::create(config);
    if (!created) {
        std::cerr << "fatal: governor creation failed: " << created.error().render() << '\n';
        return 1;
    }
    std::unique_ptr<ThermalGovernor> governor = std::move(created.value());

    const DeviceId device{StrongId<DeviceIdTag>{7}};
    const DeviceGeneration device_generation{StrongId<DeviceGenerationTag>{1}};
    const ThermalDomainId domain{StrongId<ThermalDomainIdTag>{7}};
    const ThermalDomainGeneration domain_generation{StrongId<ThermalDomainGenerationTag>{1}};

    DeviceRegistration registration;
    registration.device = device;
    registration.generation = device_generation;
    registration.label = Label{"synthetic-accelerator-headroom"};
    registration.provenance = Provenance::SYNTHETIC;
    registration.capabilities.set(ThermalCapability::TEMPERATURE,
                                  CapabilityState::SUPPORTED_SYNTHETIC);
    registration.capabilities.set(ThermalCapability::TEMPERATURE_LIMIT,
                                  CapabilityState::SUPPORTED_SYNTHETIC);
    if (!governor->register_device(registration).ok()) {
        std::cerr << "fatal: register_device failed\n";
        return 1;
    }

    ThermalDomainDefinition definition;
    definition.id = domain;
    definition.generation = domain_generation;
    definition.type = ThermalDomainType::ACCELERATOR;
    definition.label = Label{"accelerator-headroom"};
    definition.provenance = Provenance::SYNTHETIC;
    definition.members.push_back(
        DomainMember{SubjectRef::for_device(device, device_generation), 1.0});
    if (!governor->register_thermal_domain(definition).ok()) {
        std::cerr << "fatal: register_thermal_domain failed\n";
        return 1;
    }

    auto published = governor->publish_temperature(make_sample(
        device, device_generation, domain, domain_generation, 78.0, 83.0, 1, clock->now(),
        clock->wall_now(), governor->epoch()));
    report(published.has_value(), "publish_temperature accepted 78.00 C with an 83.00 C limit");

    auto evaluation = governor->evaluate_domain(domain);
    report(evaluation.has_value(), "evaluate_domain at 78.00 C");
    if (evaluation) {
        const HeadroomBreakdown& breakdown = evaluation.value().headroom;
        print_block("Live evaluation headroom (policy safety 2.00 C, synthetic limit 83.00 C)",
                    render_headroom(breakdown));
        std::cout << "       state=" << to_string(evaluation.value().state)
                  << " decision=" << to_string(evaluation.value().decision) << '\n';
        report(close_to(breakdown.governing_limit.value(), 88.0),
               "the SYNTHETIC 83.00 C limit did not tighten the 88.00 C policy ceiling");
        report(close_to(breakdown.uncertainty_margin.value(), 0.5),
               "confidence 0.5 yielded a 0.50 C uncertainty margin");
        report(close_to(breakdown.effective_headroom.value(), 7.5),
               "effective headroom is 88.00 - 78.00 - 2.00 - 0.50");
    }

    auto queried = governor->query_headroom(domain);
    report(queried.has_value() && evaluation.has_value(),
           "query_headroom returned the evaluated breakdown");
    if (queried && evaluation) {
        report(close_to(queried.value().effective_headroom.value(),
                        evaluation.value().headroom.effective_headroom.value()),
               "the queried breakdown matches the evaluation");
    }

    if (!governor->shutdown().ok()) {
        std::cerr << "fatal: shutdown failed\n";
        return 1;
    }
    std::cout << (g_failures == 0 ? "RESULT: SUCCESS\n" : "RESULT: FAILURE\n");
    return g_failures == 0 ? 0 : 1;
}
