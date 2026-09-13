// Thermal Governor - example: SYNTHETIC rack envelope.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// WARNING: every number in this example is SYNTHETIC. No rack sensor is
// present and no physical rack is being governed. The example builds a
// modelled rack domain, two modelled node domains and two modelled devices,
// exercises one hot member, queries the rack-level envelope and drives the
// rack through recovery. Provenance stays SYNTHETIC from registration to
// evaluation and is never upgraded.
//
// Structure: rack domain 100 (RACK) -> node domains 1 and 2 (NODE) -> one
// device each. The rack aggregate is a SYNTHETIC_AGGREGATION over its member
// devices with a modelled offset; the node domains aggregate their own
// device directly. Device members are what resolve the temperature
// capability of a domain, and they keep the recovery sample history bound to
// one device generation.

#include "thermal_governor/thermal_governor.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
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

void print_block(const std::string& title, const std::string& body) {
    std::cout << "--- " << title << " ---\n" << body;
}

ThermalEvidence make_device_sample(DeviceId device, DeviceGeneration device_generation,
                                   ThermalDomainId domain,
                                   ThermalDomainGeneration domain_generation, double temperature,
                                   std::uint64_t sequence, SteadyTimePoint measured_at,
                                   WallTimePoint measured_wall_at, CoordinatorEpoch epoch) {
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
    std::cout << "== Thermal Governor example: SYNTHETIC rack envelope ==\n";
    std::cout << "NOTE: SYNTHETIC scenario - no physical rack, node or device is involved.\n";

    auto clock = std::make_shared<ManualClock>(SteadyTimePoint{std::chrono::seconds{2000}});
    GovernorConfig config;
    config.clock = clock;
    auto created = ThermalGovernor::create(config);
    if (!created) {
        std::cerr << "fatal: governor creation failed: " << created.error().render() << '\n';
        return 1;
    }
    std::unique_ptr<ThermalGovernor> governor = std::move(created.value());

    constexpr std::size_t kNodeCount = 2;
    const RackId rack{StrongId<RackIdTag>{1}};
    const RackGeneration rack_generation{StrongId<RackGenerationTag>{1}};
    const ThermalDomainId rack_domain{StrongId<ThermalDomainIdTag>{100}};
    const ThermalDomainGeneration domain_generation{StrongId<ThermalDomainGenerationTag>{1}};
    const DeviceGeneration device_generation{StrongId<DeviceGenerationTag>{1}};
    const NodeGeneration node_generation{StrongId<NodeGenerationTag>{1}};

    std::vector<DeviceId> devices;
    std::vector<NodeId> nodes;
    std::vector<ThermalDomainId> node_domains;

    std::cout << "step 1: register the modelled topology\n";
    for (std::size_t index = 0; index < kNodeCount; ++index) {
        const DeviceId device{StrongId<DeviceIdTag>{index + 1}};
        const NodeId node{StrongId<NodeIdTag>{index + 1}};
        const ThermalDomainId node_domain{StrongId<ThermalDomainIdTag>{index + 1}};
        devices.push_back(device);
        nodes.push_back(node);
        node_domains.push_back(node_domain);

        DeviceRegistration registration;
        registration.device = device;
        registration.generation = device_generation;
        registration.node = node;
        registration.node_generation = node_generation;
        registration.rack = rack;
        registration.rack_generation = rack_generation;
        registration.label = Label{"synthetic-device-" + std::to_string(index)};
        registration.provenance = Provenance::SYNTHETIC;
        registration.capabilities.set(ThermalCapability::TEMPERATURE,
                                      CapabilityState::SUPPORTED_SYNTHETIC);
        registration.capabilities.set(ThermalCapability::RACK_THERMAL_METADATA,
                                      CapabilityState::SUPPORTED_SYNTHETIC);
        if (!governor->register_device(registration).ok()) {
            std::cerr << "fatal: register_device failed\n";
            return 1;
        }
    }

    // A multi-member aggregate is evaluated against a synthesized record
    // whose subject is the thermal domain itself, so the device-generation
    // binding of the recovery gate is inapplicable to the rack envelope. The
    // rack therefore gets its own policy: every other recovery gate stays
    // enabled.
    ThermalPolicy rack_policy = ThermalPolicy::make_default();
    rack_policy.id = ThermalPolicyId{StrongId<ThermalPolicyIdTag>{2}};
    rack_policy.label = Label{"synthetic-rack-envelope"};
    rack_policy.recovery.require_same_device_generation = false;
    auto policy_result = governor->set_policy(rack_policy);
    if (!policy_result.has_value()) {
        std::cerr << "fatal: set_policy failed: " << policy_result.error().render() << '\n';
        return 1;
    }
    std::cout << "       rack policy id=" << rack_policy.id.value()
              << " label=" << rack_policy.label.text()
              << " require_same_device_generation=false\n";

    ThermalDomainDefinition rack_definition;
    rack_definition.id = rack_domain;
    rack_definition.generation = domain_generation;
    rack_definition.type = ThermalDomainType::RACK;
    rack_definition.label = Label{"rack-domain-SYNTHETIC"};
    rack_definition.provenance = Provenance::SYNTHETIC;
    rack_definition.evidence_source = "SYNTHETIC rack envelope model (+1.50 C over the hottest member)";
    rack_definition.aggregation = AggregationRule::SYNTHETIC_AGGREGATION;
    rack_definition.synthetic_aggregate_offset = 1.5;
    rack_definition.rack = rack;
    rack_definition.rack_generation = rack_generation;
    rack_definition.policy = rack_policy.id;
    for (std::size_t index = 0; index < kNodeCount; ++index) {
        rack_definition.members.push_back(
            DomainMember{SubjectRef::for_device(devices[index], device_generation), 1.0});
    }
    if (!governor->register_thermal_domain(rack_definition).ok()) {
        std::cerr << "fatal: register_thermal_domain (rack) failed\n";
        return 1;
    }
    std::cout << "       rack aggregation=" << to_string(rack_definition.aggregation)
              << " offset=" << number(*rack_definition.synthetic_aggregate_offset)
              << " provenance=" << to_string(rack_definition.provenance) << '\n';

    for (std::size_t index = 0; index < kNodeCount; ++index) {
        ThermalDomainDefinition node_definition;
        node_definition.id = node_domains[index];
        node_definition.generation = domain_generation;
        node_definition.type = ThermalDomainType::NODE;
        node_definition.label = Label{"node-domain-" + std::to_string(index)};
        node_definition.provenance = Provenance::SYNTHETIC;
        node_definition.evidence_source = "SYNTHETIC node aggregation";
        node_definition.aggregation = AggregationRule::HOTTEST_MEMBER_GOVERNS;
        node_definition.parent = rack_domain;
        node_definition.rack = rack;
        node_definition.rack_generation = rack_generation;
        node_definition.members.push_back(
            DomainMember{SubjectRef::for_device(devices[index], device_generation), 1.0});
        if (!governor->register_thermal_domain(node_definition).ok()) {
            std::cerr << "fatal: register_thermal_domain (node) failed\n";
            return 1;
        }
        std::cout << "       rack domain " << rack_domain.value() << " -> node domain "
                  << node_domains[index].value() << " -> device " << devices[index].value()
                  << " (node " << nodes[index].value() << ")\n";
    }

    std::uint64_t sequence = 0;
    const auto publish_step = [&](double first, double second, std::int64_t advance_ms) {
        clock->advance(Milliseconds{advance_ms});
        const double temperatures[kNodeCount] = {first, second};
        for (std::size_t index = 0; index < kNodeCount; ++index) {
            ++sequence;
            auto receipt = governor->publish_temperature(
                make_device_sample(devices[index], device_generation, node_domains[index],
                                   domain_generation, temperatures[index], sequence, clock->now(),
                                   clock->wall_now(), governor->epoch()));
            if (!receipt) {
                std::cout << "[FAIL] device publish: " << receipt.error().render() << '\n';
                ++g_failures;
            }
        }
    };

    const auto describe = [&](const std::string& label, ThermalDomainId domain,
                              ThermalState expected) -> std::optional<ThermalEvaluation> {
        auto evaluation = governor->evaluate_domain(domain);
        if (!evaluation) {
            std::cout << "[FAIL] " << label << ": " << evaluation.error().render() << '\n';
            ++g_failures;
            return std::nullopt;
        }
        const ThermalEvaluation& value = evaluation.value();
        std::cout << "       " << label << " state=" << to_string(value.state)
                  << " decision=" << to_string(value.decision)
                  << " temperature=" << value.headroom.current_temperature.render()
                  << " provenance=" << to_string(value.provenance) << '\n';
        report(value.state == expected,
               label + " evaluates to " + std::string(to_string(expected)));
        return value;
    };

    std::cout << "step 2: cool rack (device 60.00/58.00 C)\n";
    publish_step(60.0, 58.0, 0);
    describe("node domain 1", node_domains[0], ThermalState::NORMAL);
    describe("node domain 2", node_domains[1], ThermalState::NORMAL);
    const auto rack_cool = describe("rack domain", rack_domain, ThermalState::NORMAL);
    if (rack_cool) {
        report(rack_cool->headroom.current_temperature.value() == 61.5,
               "the rack aggregate is the hottest member plus the modelled 1.50 C offset");
        report(rack_cool->provenance == Provenance::SYNTHETIC,
               "the evaluated rack provenance stays SYNTHETIC");
    }
    auto cool_envelope = governor->query_envelope(rack_domain);
    if (cool_envelope) {
        print_block("rack envelope while cool (stored view)", render_envelope(cool_envelope.value()));
        report(cool_envelope.value().provenance == Provenance::SYNTHETIC,
               "the stored rack envelope is labelled SYNTHETIC");
    }
    std::cout << "       note: query_envelope returns the stored evaluation, which advances"
              << " when state, derating or decision changes\n";

    std::cout << "step 3: one hot member (device 1 at 83.00 C)\n";
    publish_step(83.0, 76.0, 100);
    describe("node domain 1", node_domains[0], ThermalState::DERATED);
    describe("node domain 2", node_domains[1], ThermalState::WARM);
    describe("rack domain", rack_domain, ThermalState::DERATED);
    auto hot_envelope = governor->query_envelope(rack_domain);
    if (hot_envelope) {
        print_block("rack envelope while one member is hot", render_envelope(hot_envelope.value()));
        report(hot_envelope.value().headroom.current_temperature.value() == 84.5,
               "the rack aggregate is the hottest member plus the modelled 1.50 C offset");
        report(hot_envelope.value().provenance == Provenance::SYNTHETIC,
               "the hot rack envelope is still labelled SYNTHETIC");
        report(!hot_envelope.value().permits_full_capability(),
               "the rack envelope no longer permits full capability");
    }

    std::cout << "step 4: cool the rack back down over three sample rounds\n";
    for (int round = 0; round < 3; ++round) {
        publish_step(64.0, 58.0, 1000);
        auto evaluation = governor->evaluate_domain(rack_domain);
        if (!evaluation) {
            std::cout << "[FAIL] rack evaluation unavailable\n";
            ++g_failures;
            continue;
        }
        const RecoveryAssessment& recovery = evaluation.value().recovery;
        std::cout << "       round " << (round + 1)
                  << " state=" << to_string(evaluation.value().state)
                  << " allowed=" << (recovery.allowed ? "true" : "false")
                  << " samples=" << recovery.qualifying_samples << "/"
                  << recovery.required_samples
                  << " span_ms=" << recovery.qualifying_span.count() << "/"
                  << recovery.required_span.count() << '\n';
    }

    const auto recovered = governor->evaluate_domain(rack_domain);
    report(recovered.has_value(), "the rack domain evaluates after cooling");
    if (recovered) {
        print_block("rack recovery assessment", recovered.value().recovery.render());
        report(recovered.value().recovery.allowed,
               "the SYNTHETIC rack domain is permitted to recover");
    }
    describe("rack domain", rack_domain, ThermalState::NORMAL);
    auto released = governor->query_envelope(rack_domain);
    if (released) {
        report(released.value().permits_full_capability(),
               "the released rack envelope permits full capability again");
        report(released.value().provenance == Provenance::SYNTHETIC,
               "recovery never upgrades SYNTHETIC provenance");
    }

    auto stored = governor->evaluate_recovery(rack_domain);
    report(stored.has_value() && stored.value().allowed,
           "the stored rack assessment agrees once the state has settled");

    if (!governor->shutdown().ok()) {
        std::cerr << "fatal: shutdown failed\n";
        return 1;
    }
    std::cout << (g_failures == 0 ? "RESULT: SUCCESS\n" : "RESULT: FAILURE\n");
    return g_failures == 0 ? 0 : 1;
}
