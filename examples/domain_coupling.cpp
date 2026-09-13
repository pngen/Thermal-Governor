// Thermal Governor - example: thermal domain coupling.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Three independent synthetic accelerator domains. Domain 1 and domain 2 are
// explicitly coupled (shared airflow path); domain 3 is not. Heating domain 1
// co-derates its coupled neighbour while the independent domain keeps its
// full envelope.

#include "thermal_governor/thermal_governor.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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
    std::cout << "== Thermal Governor example: domain coupling ==\n";

    auto clock = std::make_shared<ManualClock>(SteadyTimePoint{std::chrono::seconds{900}});
    GovernorConfig config;
    config.clock = clock;
    auto created = ThermalGovernor::create(config);
    if (!created) {
        std::cerr << "fatal: governor creation failed: " << created.error().render() << '\n';
        return 1;
    }
    std::unique_ptr<ThermalGovernor> governor = std::move(created.value());

    const std::vector<ThermalDomainId> domains{
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}},
        ThermalDomainId{StrongId<ThermalDomainIdTag>{2}},
        ThermalDomainId{StrongId<ThermalDomainIdTag>{3}}};
    const DeviceGeneration device_generation{StrongId<DeviceGenerationTag>{1}};
    const ThermalDomainGeneration domain_generation{StrongId<ThermalDomainGenerationTag>{1}};

    std::vector<DeviceId> devices;
    devices.reserve(domains.size());

    for (std::size_t index = 0; index < domains.size(); ++index) {
        const DeviceId device{StrongId<DeviceIdTag>{index + 1}};
        devices.push_back(device);

        DeviceRegistration registration;
        registration.device = device;
        registration.generation = device_generation;
        registration.label = Label{"synthetic-accelerator-" + std::to_string(index)};
        registration.provenance = Provenance::SYNTHETIC;
        registration.capabilities.set(ThermalCapability::TEMPERATURE,
                                      CapabilityState::SUPPORTED_SYNTHETIC);
        if (!governor->register_device(registration).ok()) {
            std::cerr << "fatal: register_device failed\n";
            return 1;
        }

        ThermalDomainDefinition definition;
        definition.id = domains[index];
        definition.generation = domain_generation;
        definition.type = ThermalDomainType::ACCELERATOR;
        definition.label = Label{"accelerator-" + std::to_string(index)};
        definition.provenance = Provenance::SYNTHETIC;
        definition.evidence_source = "synthetic temperature model";
        definition.members.push_back(DomainMember{SubjectRef::for_device(device, device_generation),
                                                  1.0});
        if (!governor->register_thermal_domain(definition).ok()) {
            std::cerr << "fatal: register_thermal_domain failed\n";
            return 1;
        }
    }

    // A directed coupling edge makes its source observe the thermal state of
    // its destination, so mutual thermal coupling is modelled with one
    // explicit edge in each direction.
    bool couplings_ok = true;
    const auto register_airflow_edge = [&](std::uint64_t id, ThermalDomainId source,
                                           ThermalDomainId destination) {
        CouplingRelation relation;
        relation.id = CouplingId{StrongId<CouplingIdTag>{id}};
        relation.source = source;
        relation.destination = destination;
        relation.type = CouplingType::SHARES_AIRFLOW_PATH;
        relation.generation = CouplingGeneration{StrongId<CouplingGenerationTag>{1}};
        relation.provenance = Provenance::SYNTHETIC;
        relation.evidence_source = "synthetic rack airflow model";
        relation.weight = 0.8;
        relation.directed = true;
        auto status = governor->register_coupling(relation);
        if (!status.ok()) {
            std::cout << "[FAIL] register_coupling: " << status.error().render() << '\n';
            ++g_failures;
            couplings_ok = false;
            return;
        }
        std::cout << "       coupling edge " << relation.source.value() << " -> "
                  << relation.destination.value() << " (" << to_string(relation.type)
                  << ", weight=" << number(relation.weight)
                  << ", provenance=" << to_string(relation.provenance) << ")\n";
    };
    std::cout << "coupling: domain 1 and domain 2 share an airflow path\n";
    register_airflow_edge(1, domains[0], domains[1]);
    register_airflow_edge(2, domains[1], domains[0]);
    report(couplings_ok, "both directed coupling edges were registered");

    std::uint64_t sequence = 0;
    const auto publish = [&](std::size_t index, double temperature, std::int64_t advance_ms) {
        clock->advance(Milliseconds{advance_ms});
        ++sequence;
        auto receipt = governor->publish_temperature(make_sample(
            devices[index], device_generation, domains[index], domain_generation, temperature,
            sequence, clock->now(), clock->wall_now(), governor->epoch()));
        if (!receipt) {
            std::cout << "[FAIL] publish for domain " << domains[index].value() << ": "
                      << receipt.error().render() << '\n';
            ++g_failures;
        }
    };

    const auto describe = [&](const std::string& label, ThermalDomainId domain,
                              ThermalState expected) {
        auto evaluation = governor->evaluate_domain(domain);
        if (!evaluation) {
            std::cout << "[FAIL] " << label << ": " << evaluation.error().render() << '\n';
            ++g_failures;
            return;
        }
        const ThermalEvaluation& value = evaluation.value();
        std::cout << "       " << label << " state=" << to_string(value.state)
                  << " decision=" << to_string(value.decision)
                  << " derating=" << to_string(value.derating)
                  << " temperature=" << value.headroom.current_temperature.render()
                  << " coupled_pressure=" << (value.reasons.contains(
                         ThermalReasonCode::COUPLED_DOMAIN_PRESSURE) ? "true" : "false")
                  << '\n';
        report(value.state == expected,
               label + " evaluates to " + std::string(to_string(expected)));
    };

    std::cout << "step 1: all three domains start cool\n";
    for (std::size_t index = 0; index < domains.size(); ++index) {
        publish(index, 60.0, index == 0 ? 0 : 50);
    }
    describe("domain 1", domains[0], ThermalState::NORMAL);
    describe("domain 2", domains[1], ThermalState::NORMAL);
    describe("domain 3", domains[2], ThermalState::NORMAL);

    std::cout << "step 2: domain 1 goes hot (85.00 C)\n";
    publish(0, 85.0, 100);
    describe("domain 1", domains[0], ThermalState::DERATED);

    std::cout << "step 3: coupled domain 2 is co-derated by propagation\n";
    describe("domain 2", domains[1], ThermalState::DERATED);
    auto coupled = governor->evaluate_domain(domains[1]);
    if (coupled) {
        report(coupled.value().reasons.contains(ThermalReasonCode::COUPLED_DOMAIN_PRESSURE),
               "domain 2 records coupled domain pressure as a reason");
        report(!coupled.value().recovery.coupled_domains_satisfied,
               "domain 2 recovery records that its coupled neighbour has not recovered");
        report(coupled.value().recovery.blocking_reasons.contains(
                   ThermalReasonCode::RECOVERY_BLOCKED_BY_COUPLED_DOMAIN),
               "domain 2 recovery is blocked while its coupled neighbour is derated");
        std::cout << "       permitted_concurrency="
                  << number(coupled.value().permitted_concurrency.value())
                  << " derating=" << to_string(coupled.value().derating)
                  << " reasons=" << coupled.value().reasons.render() << '\n';
    }

    std::cout << "step 4: independent domain 3 is unaffected\n";
    describe("domain 3", domains[2], ThermalState::NORMAL);
    auto independent = governor->evaluate_domain(domains[2]);
    if (independent) {
        report(!independent.value().reasons.contains(ThermalReasonCode::COUPLED_DOMAIN_PRESSURE),
               "domain 3 records no coupled domain pressure");
        report(independent.value().derating == DeratingLevel::FULL_CAPABILITY &&
                   independent.value().permitted_concurrency.value() == 100.0,
               "domain 3 keeps its unrestricted envelope");
    }

    auto snapshot = governor->snapshot();
    report(snapshot != nullptr && snapshot->couplings.size() == 2,
           "the snapshot exposes exactly two coupling edges");
    if (snapshot != nullptr) {
        const auto coupled_view = snapshot->domains.find(domains[1].value());
        if (coupled_view != snapshot->domains.end()) {
            for (const auto& edge : coupled_view->second.couplings) {
                std::cout << "       coupling edge " << edge.source.value() << " -> "
                          << edge.destination.value() << " type=" << to_string(edge.type)
                          << " weight=" << number(edge.weight)
                          << " provenance=" << to_string(edge.provenance) << '\n';
            }
            report(!coupled_view->second.couplings.empty(),
                   "domain 2 is attached to the coupling edge");
        }
        const auto independent_view = snapshot->domains.find(domains[2].value());
        if (independent_view != snapshot->domains.end()) {
            report(independent_view->second.couplings.empty(),
                   "domain 3 is attached to no coupling edge");
        }
    }

    if (!governor->shutdown().ok()) {
        std::cerr << "fatal: shutdown failed\n";
        return 1;
    }
    std::cout << (g_failures == 0 ? "RESULT: SUCCESS\n" : "RESULT: FAILURE\n");
    return g_failures == 0 ? 0 : 1;
}
