// Thermal Governor - example: real NVML telemetry.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// NVML is loaded dynamically at run time. When the library or a device is
// absent this example reports UNSUPPORTED and exits 0: an absent sensor is
// never reported as a zero reading. When NVML is present, every capability is
// classified independently and the real temperature is printed.

#include "thermal_governor/thermal_governor.hpp"

#include <cstddef>
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

std::string pad(const std::string& text, std::size_t width) {
    if (text.size() >= width) {
        return text + " ";
    }
    return text + std::string(width - text.size(), ' ');
}

}  // namespace

int main() {
    std::cout << "== Thermal Governor example: real NVML telemetry ==\n";

    auto created = NvmlThermalBackend::create();
    if (!created) {
        std::cout << "STATUS: UNSUPPORTED - NVML/CUDA is not available on this host\n";
        std::cout << "       reason: " << created.error().render() << '\n';
        std::cout << "       no temperature is fabricated; every capability is unavailable\n";
        std::cout << "RESULT: SUCCESS (UNSUPPORTED path exits 0)\n";
        return 0;
    }

    std::unique_ptr<NvmlThermalBackend> backend = std::move(created.value());
    std::cout << "STATUS: SUPPORTED - the real NVML backend is loaded\n";
    std::cout << "       backend=" << backend->name() << " driver_version="
              << backend->driver_version() << " nvml_version=" << backend->nvml_version()
              << " device_count=" << backend->device_count() << '\n';
    std::cout << "       nvml_available()=" << (nvml_available() ? "true" : "false") << '\n';

    BackendCapabilities capabilities = backend->capabilities();
    std::cout << "step 1: per-capability classification\n";
    std::cout << "       backend_name=" << capabilities.backend_name
              << " backend_version=" << capabilities.backend_version
              << " provenance=" << to_string(capabilities.provenance) << '\n';
    for (std::size_t index = 0; index < kThermalCapabilityCount; ++index) {
        const auto capability = static_cast<ThermalCapability>(index);
        const CapabilityState state = capabilities.capabilities.get(capability);
        std::cout << "       " << pad(std::string(capability_name(capability)), 34)
                  << to_string(state);
        const std::string& detail = capabilities.capabilities.detail(capability);
        if (!detail.empty()) {
            std::cout << " (" << detail << ')';
        }
        std::cout << '\n';
    }

    std::cout << "step 2: device identities\n";
    auto identities = backend->query_device_identity();
    if (!identities) {
        std::cout << "[FAIL] query_device_identity: " << identities.error().render() << '\n';
        std::cout << "RESULT: FAILURE\n";
        return 1;
    }
    if (identities.value().empty()) {
        std::cout << "STATUS: NO DEVICE - NVML initialised but no device is visible\n";
        std::cout << "RESULT: SUCCESS (no device path exits 0)\n";
        return 0;
    }
    for (const auto& identity : identities.value()) {
        std::cout << "       device=" << identity.device.value() << " index=" << identity.index
                  << " name=\"" << identity.name << "\" uuid=\"" << identity.uuid
                  << "\" pci=\"" << identity.pci_bus_id << "\"\n";
    }

    std::cout << "step 3: real temperatures\n";
    for (const auto& identity : identities.value()) {
        const CapabilityState temperature_state =
            capabilities.capabilities.get(ThermalCapability::TEMPERATURE);
        auto reading = backend->query_temperature(identity.device);
        if (!reading) {
            if (is_supported(temperature_state)) {
                std::cout << "[FAIL] device " << identity.device.value()
                          << " claims TEMPERATURE support but the read failed: "
                          << reading.error().render() << '\n';
                ++g_failures;
            } else {
                std::cout << "       device=" << identity.device.value()
                          << " temperature unavailable (capability="
                          << to_string(temperature_state) << ")\n";
            }
            continue;
        }
        std::cout << "       device=" << identity.device.value()
                  << " temperature=" << reading.value().temperature.render()
                  << " C source=" << to_string(reading.value().source) << '\n';
        report(reading.value().temperature.is_valid(),
               "device " + std::to_string(identity.device.value()) +
                   " reported a valid real temperature");
    }

    std::cout << "step 4: optional vendor limits and throttle reasons\n";
    for (const auto& identity : identities.value()) {
        auto limit = backend->query_temperature_limit(identity.device);
        if (limit) {
            std::cout << "       device=" << identity.device.value()
                      << " temperature_limit=" << limit.value().render() << " C\n";
        } else {
            std::cout << "       device=" << identity.device.value()
                      << " temperature_limit=<unsupported>\n";
        }
        auto throttle = backend->query_throttle_reasons(identity.device);
        if (throttle) {
            std::cout << "       device=" << identity.device.value()
                      << " throttle_class=" << to_string(throttle.value().classification)
                      << " reasons=" << throttle.value().render_reasons() << '\n';
        } else {
            std::cout << "       device=" << identity.device.value()
                      << " throttle_reasons=<unsupported>\n";
        }
    }

    std::cout << (g_failures == 0 ? "RESULT: SUCCESS\n" : "RESULT: FAILURE\n");
    return g_failures == 0 ? 0 : 1;
}
