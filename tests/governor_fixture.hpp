// Thermal Governor — shared test fixture.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_TESTS_GOVERNOR_FIXTURE_HPP
#define THERMAL_GOVERNOR_TESTS_GOVERNOR_FIXTURE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "thermal_governor/thermal_governor.hpp"

namespace tg {

/// A governor wired to a manually advanced clock so every duration-dependent
/// policy path is deterministic and no test ever sleeps.
struct GovernorFixture {
    std::shared_ptr<thermal_governor::ManualClock> clock =
        std::make_shared<thermal_governor::ManualClock>(thermal_governor::SteadyTimePoint{});
    std::unique_ptr<thermal_governor::ThermalGovernor> governor;
    std::uint64_t telemetry = 0;
    std::string durable_path;

    [[nodiscard]] static std::unique_ptr<GovernorFixture> create() {
        auto fixture = std::make_unique<GovernorFixture>();
        thermal_governor::GovernorConfig config;
        config.coordinator =
            thermal_governor::CoordinatorId{thermal_governor::StrongId<thermal_governor::CoordinatorIdTag>{1}};
        config.clock = fixture->clock;
        auto created = thermal_governor::ThermalGovernor::create(std::move(config));
        if (!created.has_value()) {
            return nullptr;
        }
        fixture->governor = std::move(created.value());
        return fixture;
    }

    [[nodiscard]] static std::unique_ptr<GovernorFixture> create_with_state(
        const std::string& durable_path) {
        auto fixture = std::make_unique<GovernorFixture>();
        fixture->durable_path = durable_path;
        thermal_governor::GovernorConfig config;
        config.coordinator =
            thermal_governor::CoordinatorId{thermal_governor::StrongId<thermal_governor::CoordinatorIdTag>{1}};
        config.clock = fixture->clock;
        config.durable_path = durable_path;
        auto created = thermal_governor::ThermalGovernor::create(std::move(config));
        if (!created.has_value()) {
            return nullptr;
        }
        fixture->governor = std::move(created.value());
        return fixture;
    }

    void advance(std::int64_t milliseconds) {
        clock->advance(thermal_governor::Milliseconds{milliseconds});
    }

    [[nodiscard]] thermal_governor::Status add_device(
        std::uint64_t device, std::uint64_t generation = 1,
        thermal_governor::CapabilityState temperature =
            thermal_governor::CapabilityState::SUPPORTED_SYNTHETIC,
        thermal_governor::CapabilityState throttle = thermal_governor::CapabilityState::UNSUPPORTED,
        thermal_governor::CapabilityState limit = thermal_governor::CapabilityState::UNSUPPORTED,
        thermal_governor::Provenance provenance = thermal_governor::Provenance::SYNTHETIC) {
        thermal_governor::DeviceRegistration registration;
        registration.device =
            thermal_governor::DeviceId{thermal_governor::StrongId<thermal_governor::DeviceIdTag>{device}};
        registration.generation = thermal_governor::DeviceGeneration{
            thermal_governor::StrongId<thermal_governor::DeviceGenerationTag>{generation}};
        registration.node =
            thermal_governor::NodeId{thermal_governor::StrongId<thermal_governor::NodeIdTag>{1}};
        registration.node_generation =
            thermal_governor::NodeGeneration{thermal_governor::StrongId<thermal_governor::NodeGenerationTag>{1}};
        registration.rack =
            thermal_governor::RackId{thermal_governor::StrongId<thermal_governor::RackIdTag>{1}};
        registration.rack_generation =
            thermal_governor::RackGeneration{thermal_governor::StrongId<thermal_governor::RackGenerationTag>{1}};
        registration.label = thermal_governor::Label{"device-" + std::to_string(device)};
        registration.provenance = provenance;
        registration.capabilities.set(thermal_governor::ThermalCapability::DEVICE_IDENTITY,
                                      thermal_governor::CapabilityState::SUPPORTED_SYNTHETIC);
        registration.capabilities.set(thermal_governor::ThermalCapability::TEMPERATURE, temperature);
        registration.capabilities.set(thermal_governor::ThermalCapability::THROTTLE_REASONS,
                                      throttle);
        registration.capabilities.set(thermal_governor::ThermalCapability::TEMPERATURE_LIMIT, limit);
        registration.capabilities.set(thermal_governor::ThermalCapability::CURRENT_CLOCK,
                                      thermal_governor::CapabilityState::SUPPORTED_SYNTHETIC);
        registration.capabilities.set(thermal_governor::ThermalCapability::MAX_CLOCK,
                                      thermal_governor::CapabilityState::SUPPORTED_SYNTHETIC);
        return governor->register_device(registration);
    }

    [[nodiscard]] thermal_governor::Status add_accelerator_domain(
        std::uint64_t domain, std::uint64_t generation, std::uint64_t device,
        thermal_governor::Provenance provenance = thermal_governor::Provenance::SYNTHETIC) {
        thermal_governor::ThermalDomainDefinition definition;
        definition.id =
            thermal_governor::ThermalDomainId{thermal_governor::StrongId<thermal_governor::ThermalDomainIdTag>{domain}};
        definition.generation = thermal_governor::ThermalDomainGeneration{
            thermal_governor::StrongId<thermal_governor::ThermalDomainGenerationTag>{generation}};
        definition.type = thermal_governor::ThermalDomainType::ACCELERATOR;
        definition.label = thermal_governor::Label{"domain-" + std::to_string(domain)};
        definition.provenance = provenance;
        definition.evidence_source = "synthetic test device";
        definition.policy =
            thermal_governor::ThermalPolicyId{thermal_governor::StrongId<thermal_governor::ThermalPolicyIdTag>{1}};
        definition.aggregation = thermal_governor::AggregationRule::HOTTEST_MEMBER_GOVERNS;
        thermal_governor::DomainMember member;
        member.subject = thermal_governor::SubjectRef::for_device(
            thermal_governor::DeviceId{thermal_governor::StrongId<thermal_governor::DeviceIdTag>{device}},
            thermal_governor::DeviceGeneration{
                thermal_governor::StrongId<thermal_governor::DeviceGenerationTag>{1}});
        definition.members.push_back(member);
        return governor->register_thermal_domain(definition);
    }

    /// Publish temperature evidence for a device. Returns the receipt.
    [[nodiscard]] thermal_governor::Result<thermal_governor::EvidenceReceipt> publish(
        std::uint64_t device, double temperature, std::uint64_t domain = 1,
        std::uint64_t domain_generation = 1, std::uint64_t device_generation = 1,
        thermal_governor::WorkerId worker = thermal_governor::WorkerId{},
        thermal_governor::WorkerBootId boot = thermal_governor::WorkerBootId{},
        std::optional<thermal_governor::ThrottleObservation> throttle = std::nullopt,
        thermal_governor::Provenance provenance = thermal_governor::Provenance::SYNTHETIC,
        std::optional<double> confidence = std::nullopt,
        std::optional<double> limit = std::nullopt) {
        ++telemetry;
        thermal_governor::ThermalEvidence evidence;
        evidence.evidence_id =
            thermal_governor::EvidenceId{thermal_governor::StrongId<thermal_governor::EvidenceIdTag>{telemetry}};
        evidence.subject = thermal_governor::SubjectRef::for_device(
            thermal_governor::DeviceId{thermal_governor::StrongId<thermal_governor::DeviceIdTag>{device}},
            thermal_governor::DeviceGeneration{
                thermal_governor::StrongId<thermal_governor::DeviceGenerationTag>{device_generation}});
        evidence.domain =
            thermal_governor::ThermalDomainId{thermal_governor::StrongId<thermal_governor::ThermalDomainIdTag>{domain}};
        evidence.domain_generation = thermal_governor::ThermalDomainGeneration{
            thermal_governor::StrongId<thermal_governor::ThermalDomainGenerationTag>{domain_generation}};
        evidence.temperature = thermal_governor::DegreesCelsius{temperature};
        evidence.source = thermal_governor::MeasurementSource::SYNTHETIC_MODEL;
        evidence.provenance = provenance;
        if (provenance == thermal_governor::Provenance::REAL) {
            evidence.source = thermal_governor::MeasurementSource::NVML_GPU_TEMPERATURE;
        }
        evidence.measurement_sequence = telemetry;
        evidence.telemetry_generation = thermal_governor::TelemetryGeneration{
            thermal_governor::StrongId<thermal_governor::TelemetryGenerationTag>{telemetry}};
        evidence.worker = worker;
        evidence.worker_boot = boot;
        evidence.coordinator_epoch = governor->epoch();
        evidence.measured_at = clock->now();
        evidence.measured_wall_at = clock->wall_now();
        evidence.capability_generation =
            thermal_governor::CapabilityGeneration{thermal_governor::StrongId<thermal_governor::CapabilityGenerationTag>{1}};
        evidence.topology_generation =
            thermal_governor::TopologyGeneration{thermal_governor::StrongId<thermal_governor::TopologyGenerationTag>{1}};
        evidence.throttle = throttle;
        evidence.confidence = confidence;
        if (limit.has_value()) {
            evidence.temperature_limit = thermal_governor::DegreesCelsius{*limit};
        }
        return governor->publish_temperature(evidence);
    }

    /// Advance the clock and publish in one step.
    [[nodiscard]] thermal_governor::Result<thermal_governor::EvidenceReceipt> publish_after(
        std::int64_t milliseconds, std::uint64_t device, double temperature,
        std::uint64_t domain = 1) {
        advance(milliseconds);
        return publish(device, temperature, domain);
    }
};

}  // namespace tg

#endif  // THERMAL_GOVERNOR_TESTS_GOVERNOR_FIXTURE_HPP
