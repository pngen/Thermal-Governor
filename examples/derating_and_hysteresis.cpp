// Thermal Governor - example: derating and recovery hysteresis.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Drives one synthetic accelerator domain through the warning band, the
// near-limit band and the derating threshold, then cools it below the
// recovery threshold. The DERATED envelope is retained until the recovery
// policy - consecutive admissible samples over a minimum evidence span - is
// satisfied. A ManualClock is used so no wall-clock sleeping is involved.

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

void print_recovery(const std::string& prefix, const RecoveryAssessment& recovery) {
    std::cout << "       " << prefix << " allowed=" << (recovery.allowed ? "true" : "false")
              << " samples=" << recovery.qualifying_samples << "/" << recovery.required_samples
              << " span_ms=" << recovery.qualifying_span.count() << "/"
              << recovery.required_span.count()
              << " blocking=" << recovery.blocking_reasons.render() << '\n';
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
    std::cout << "== Thermal Governor example: derating and hysteresis ==\n";

    auto clock = std::make_shared<ManualClock>(SteadyTimePoint{std::chrono::seconds{1000}});
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
                                  CapabilityState::SUPPORTED_SYNTHETIC);
    registration.capabilities.set(ThermalCapability::THROTTLE_REASONS,
                                  CapabilityState::SUPPORTED_SYNTHETIC);
    if (!governor->register_device(registration).ok()) {
        std::cerr << "fatal: register_device failed\n";
        return 1;
    }

    ThermalDomainDefinition definition;
    definition.id = domain;
    definition.generation = domain_generation;
    definition.type = ThermalDomainType::ACCELERATOR;
    definition.label = Label{"accelerator-0"};
    definition.provenance = Provenance::SYNTHETIC;
    definition.evidence_source = "synthetic temperature model";
    definition.members.push_back(
        DomainMember{SubjectRef::for_device(device, device_generation), 1.0});
    if (!governor->register_thermal_domain(definition).ok()) {
        std::cerr << "fatal: register_thermal_domain failed\n";
        return 1;
    }

    std::cout << "policy thresholds: warning=75.00 derating=80.00 critical=88.00 recovery=72.00"
              << " recovery_margin=1.00 required_samples=3 min_span_ms=2000\n";

    std::uint64_t sequence = 0;
    const auto publish_and_evaluate = [&](double temperature, std::int64_t advance_ms) {
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
        return governor->evaluate_domain(domain);
    };

    const auto report_state = [&](const std::string& label, const Result<ThermalEvaluation>& result,
                                  ThermalState expected) {
        if (!result) {
            std::cout << "[FAIL] " << label << ": " << result.error().render() << '\n';
            ++g_failures;
            return;
        }
        const ThermalEvaluation& evaluation = result.value();
        std::cout << "       " << label << " state=" << to_string(evaluation.state)
                  << " decision=" << to_string(evaluation.decision)
                  << " derating=" << to_string(evaluation.derating)
                  << " retained_by_hysteresis="
                  << (evaluation.retained_by_hysteresis ? "true" : "false") << '\n';
        report(evaluation.state == expected,
               label + " evaluates to " + std::string(to_string(expected)));
    };

    std::cout << "step 1: warm the accelerator\n";
    const auto warm = publish_and_evaluate(76.0, 0);
    report_state("warning band 76.00 C", warm, ThermalState::WARM);

    std::cout << "step 2: enter the near-limit band\n";
    const auto near = publish_and_evaluate(78.0, 100);
    report_state("near-limit band 78.00 C", near, ThermalState::NEAR_LIMIT);

    std::cout << "step 3: cross the derating threshold\n";
    const auto hot = publish_and_evaluate(83.0, 100);
    report_state("derating band 83.00 C", hot, ThermalState::DERATED);
    if (hot) {
        report(hot.value().decision == ThermalDecision::ALLOW_DERATED,
               "the derated envelope decides ALLOW_DERATED");
        report(hot.value().permitted_concurrency.value() < 100.0,
               "the derated envelope reduces permitted concurrency");
        std::cout << "       permitted_concurrency="
                  << number(hot.value().permitted_concurrency.value())
                  << " reasons=" << hot.value().reasons.render() << '\n';
    }

    std::cout << "step 4: first fresh sample below the recovery gate\n";
    const auto cool_first = publish_and_evaluate(70.0, 1000);
    report_state("single cool sample 70.00 C", cool_first, ThermalState::DERATED);
    if (cool_first) {
        report(cool_first.value().retained_by_hysteresis,
               "the previous envelope is retained by hysteresis");
        print_recovery("recovery after one sample", cool_first.value().recovery);
        report(!cool_first.value().recovery.allowed,
               "recovery is not yet authorised after a single sample");
    }

    std::cout << "step 5: second consecutive cool sample\n";
    const auto cool_second = publish_and_evaluate(70.0, 1000);
    report_state("second cool sample 70.00 C", cool_second, ThermalState::DERATED);
    if (cool_second) {
        print_recovery("recovery after two samples", cool_second.value().recovery);
        report(!cool_second.value().recovery.allowed,
               "recovery is not yet authorised after two samples");
    }

    std::cout << "step 6: third consecutive cool sample completes the span\n";
    const auto cool_third = publish_and_evaluate(70.0, 1000);
    report_state("third cool sample 70.00 C", cool_third, ThermalState::NORMAL);
    if (cool_third) {
        print_recovery("recovery after three samples", cool_third.value().recovery);
        report(cool_third.value().recovery.allowed,
               "recovery is authorised once samples and span are satisfied");
        report(!cool_third.value().retained_by_hysteresis,
               "the hysteresis retention is released");
        report(cool_third.value().decision == ThermalDecision::ALLOW,
               "the released envelope decides ALLOW");
        bool released_from_derated = false;
        std::cout << "       recorded transitions:\n";
        for (const auto& record : governor->history()) {
            std::cout << "         seq=" << record.sequence << " "
                      << to_string(record.from) << " -> " << to_string(record.to)
                      << " derating " << to_string(record.derating_from) << " -> "
                      << to_string(record.derating_to)
                      << " temperature=" << record.temperature.render() << '\n';
            if (record.domain == domain && record.from == ThermalState::DERATED &&
                record.to == ThermalState::NORMAL) {
                released_from_derated = true;
            }
        }
        report(released_from_derated,
               "the recorded history shows DERATED -> NORMAL only after recovery");
    }

    if (!governor->shutdown().ok()) {
        std::cerr << "fatal: shutdown failed\n";
        return 1;
    }
    std::cout << (g_failures == 0 ? "RESULT: SUCCESS\n" : "RESULT: FAILURE\n");
    return g_failures == 0 ? 0 : 1;
}
