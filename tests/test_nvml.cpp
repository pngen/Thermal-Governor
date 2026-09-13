// Thermal Governor — REAL NVML telemetry proof.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <string>

#include "framework.hpp"
#include "thermal_governor/nvml_backend.hpp"

namespace {

using namespace thermal_governor;

/// Report an honest UNSUPPORTED classification rather than a silent pass.
void report_unsupported(const char* what) {
    std::printf("NOTE nvml::%s UNSUPPORTED on this host\n", what);
    std::fflush(stdout);
}

}  // namespace

TG_CASE(nvml, library_and_initialisation) {
    TG_PHASE("NVML_INIT");
    auto backend = NvmlThermalBackend::create();
    if (!backend.has_value()) {
        report_unsupported("library_and_initialisation");
        std::printf("NOTE nvml creation error: %s\n", backend.error().render().c_str());
        std::fflush(stdout);
        return;
    }
    std::printf("NOTE nvml driver=%s nvml=%s devices=%llu\n",
                backend.value()->driver_version().c_str(),
                backend.value()->nvml_version().c_str(),
                static_cast<unsigned long long>(backend.value()->device_count()));
    std::fflush(stdout);
    TG_CHECK(backend.value()->device_count() > 0);
}

TG_CASE(nvml, capabilities_are_classified_independently) {
    TG_PHASE("NVML_CAPABILITIES");
    auto backend = NvmlThermalBackend::create();
    if (!backend.has_value()) {
        report_unsupported("capabilities_are_classified_independently");
        return;
    }
    auto capabilities = backend.value()->capabilities();
    TG_CHECK_EQ(capabilities.provenance, Provenance::REAL);
    // Temperature must be a genuine reading on a host with a working device.
    TG_CHECK_EQ(capabilities.capabilities.get(ThermalCapability::TEMPERATURE),
                CapabilityState::SUPPORTED_REAL);
    // These are genuinely absent from NVML and must not be fabricated.
    TG_CHECK_EQ(capabilities.capabilities.get(ThermalCapability::COOLING_TELEMETRY),
                CapabilityState::UNSUPPORTED);
    TG_CHECK_EQ(capabilities.capabilities.get(ThermalCapability::RACK_THERMAL_METADATA),
                CapabilityState::UNSUPPORTED);
    TG_CHECK_EQ(capabilities.capabilities.get(ThermalCapability::NODE_THERMAL_METADATA),
                CapabilityState::UNSUPPORTED);
    TG_CHECK_EQ(capabilities.capabilities.get(ThermalCapability::ACTUAL_THROTTLE_ENFORCEMENT),
                CapabilityState::UNSUPPORTED);
    std::printf("NOTE nvml capability matrix:\n%s\n", capabilities.capabilities.render().c_str());
    std::fflush(stdout);
}

TG_CASE(nvml, real_device_identity_and_temperature) {
    TG_PHASE("NVML_TELEMETRY");
    auto backend = NvmlThermalBackend::create();
    if (!backend.has_value()) {
        report_unsupported("real_device_identity_and_temperature");
        return;
    }
    auto identities = backend.value()->query_device_identity();
    TG_OK(identities);
    TG_CHECK(!identities.value().empty());
    for (const auto& identity : identities.value()) {
        std::printf("NOTE nvml device index=%u name=%s uuid=%s pci=%s\n", identity.index,
                    identity.name.c_str(), identity.uuid.c_str(), identity.pci_bus_id.c_str());
        TG_CHECK(!identity.uuid.empty());
        TG_CHECK(!identity.name.empty());
    }
    std::fflush(stdout);

    const DeviceId device = identities.value().front().device;
    auto reading = backend.value()->query_temperature(device);
    TG_OK(reading);
    TG_CHECK_EQ(reading.value().source, MeasurementSource::NVML_GPU_TEMPERATURE);
    // A real GPU temperature is never below absolute zero and never absurd.
    TG_CHECK(reading.value().temperature.value() > -100.0);
    TG_CHECK(reading.value().temperature.value() < 200.0);
    std::printf("NOTE nvml real temperature=%s C\n", reading.value().temperature.render().c_str());
    std::fflush(stdout);
}

TG_CASE(nvml, throttle_reasons_when_supported) {
    TG_PHASE("NVML_THROTTLE");
    auto backend = NvmlThermalBackend::create();
    if (!backend.has_value()) {
        report_unsupported("throttle_reasons_when_supported");
        return;
    }
    auto capabilities = backend.value()->capabilities();
    auto identities = backend.value()->query_device_identity();
    TG_OK(identities);
    TG_CHECK(!identities.value().empty());
    const DeviceId device = identities.value().front().device;
    auto observation = backend.value()->query_throttle_reasons(device);
    if (capabilities.capabilities.get(ThermalCapability::THROTTLE_REASONS) ==
        CapabilityState::UNSUPPORTED) {
        TG_CHECK(!observation.has_value());
        TG_CHECK_EQ(observation.error().code, ThermalErrorCode::CAPABILITY_UNSUPPORTED);
        return;
    }
    TG_OK(observation);
    // A cool idle device reporting a thermal slowdown would be a defect in
    // the classification, not a genuine reading.
    std::printf("NOTE nvml throttle class=%s reasons=%s\n",
                std::string(to_string(observation.value().classification)).c_str(),
                observation.value().render_reasons().c_str());
    std::fflush(stdout);
    TG_CHECK(observation.value().classification != ThrottleClass::THROTTLE_UNKNOWN);
}

TG_CASE(nvml, absent_cooling_telemetry_is_never_fabricated) {
    TG_PHASE("NVML_COOLING");
    auto backend = NvmlThermalBackend::create();
    if (!backend.has_value()) {
        report_unsupported("absent_cooling_telemetry_is_never_fabricated");
        return;
    }
    auto identities = backend.value()->query_device_identity();
    TG_OK(identities);
    TG_CHECK(!identities.value().empty());
    const DeviceId device = identities.value().front().device;
    // NVML exposes no coolant or airflow telemetry. An absent sensor is
    // reported as UNSUPPORTED, never as zero cooling.
    auto cooling = backend.value()->query_cooling_state(device);
    TG_CHECK(!cooling.has_value());
    TG_CHECK_EQ(cooling.error().code, ThermalErrorCode::CAPABILITY_UNSUPPORTED);

    auto node = backend.value()->query_node_temperature(NodeId{StrongId<NodeIdTag>{1}});
    TG_CHECK(!node.has_value());
    TG_CHECK_EQ(node.error().code, ThermalErrorCode::CAPABILITY_UNSUPPORTED);

    auto rack = backend.value()->query_rack_temperature(RackId{StrongId<RackIdTag>{1}});
    TG_CHECK(!rack.has_value());
    TG_CHECK_EQ(rack.error().code, ThermalErrorCode::CAPABILITY_UNSUPPORTED);

    // A fan reading is present only when the device genuinely reports one.
    auto fan = backend.value()->query_fan_state(device);
    if (fan.has_value()) {
        TG_CHECK_EQ(fan.value().provenance, Provenance::REAL);
        std::printf("NOTE nvml fan count=%u speed=%s\n", fan.value().fan_count,
                    fan.value().speed_percent.has_value()
                        ? std::to_string(*fan.value().speed_percent).c_str()
                        : "<none>");
        std::fflush(stdout);
    } else {
        TG_CHECK_EQ(fan.error().code, ThermalErrorCode::CAPABILITY_UNSUPPORTED);
    }
}

TG_CASE(nvml, unknown_device_is_a_typed_error) {
    TG_PHASE("NVML_UNKNOWN_DEVICE");
    auto backend = NvmlThermalBackend::create();
    if (!backend.has_value()) {
        report_unsupported("unknown_device_is_a_typed_error");
        return;
    }
    auto reading = backend.value()->query_temperature(DeviceId{StrongId<DeviceIdTag>{9999}});
    TG_CHECK(!reading.has_value());
    TG_CHECK_EQ(reading.error().code, ThermalErrorCode::UNKNOWN_DEVICE);
}

TG_CASE(nvml, unsupported_enforcement_is_reported_honestly) {
    TG_PHASE("NVML_ENFORCEMENT");
    auto backend = NvmlThermalBackend::create();
    if (!backend.has_value()) {
        report_unsupported("unsupported_enforcement_is_reported_honestly");
        return;
    }
    auto identities = backend.value()->query_device_identity();
    TG_OK(identities);
    TG_CHECK(!identities.value().empty());
    // Thermal Governor does not own clock enforcement through NVML, and says
    // so rather than pretending to have acted.
    auto enforced = backend.value()->request_clock_ceiling(identities.value().front().device,
                                                           MegaHertz{1000});
    TG_CHECK(!enforced.ok());
    TG_CHECK_EQ(enforced.code(), ThermalErrorCode::CAPABILITY_UNSUPPORTED);
}
