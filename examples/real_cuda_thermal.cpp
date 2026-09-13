// Thermal Governor - example: real CUDA completed-work proof.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// When this build contains no CUDA instrumentation the example reports
// UNSUPPORTED and exits 0. Otherwise it runs a small bounded deterministic
// workload (32 iterations over 2^18 elements), samples the real NVML
// temperature before and after, and prints CPU parity, checksums, elapsed
// time and the device-memory release flag. Cooling is never disabled and the
// workload is deliberately tiny: nothing here can overheat hardware.

#include "thermal_governor/thermal_governor.hpp"

#include <cstddef>
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

std::optional<DegreesCelsius> sample_nvml_temperature(NvmlThermalBackend& backend, DeviceId device,
                                                      const std::string& when) {
    auto reading = backend.query_temperature(device);
    if (!reading) {
        std::cout << "       nvml temperature " << when
                  << " unavailable: " << reading.error().render() << '\n';
        return std::nullopt;
    }
    std::cout << "       nvml temperature " << when << '=' << reading.value().temperature.render()
              << " C source=" << to_string(reading.value().source) << '\n';
    return reading.value().temperature;
}

}  // namespace

int main() {
    std::cout << "== Thermal Governor example: real CUDA thermal workload ==\n";

    if (!cuda_support_compiled()) {
        std::cout << "STATUS: UNSUPPORTED - this build was configured without CUDA support\n";
        auto count = cuda_device_count();
        if (count) {
            std::cout << "       cuda_device_count=" << count.value() << '\n';
        } else {
            std::cout << "       cuda_device_count: " << count.error().render() << '\n';
        }
        std::cout << "       proof_provenance=" << to_string(cuda_proof_provenance()) << '\n';
        std::cout << "RESULT: SUCCESS (UNSUPPORTED path exits 0)\n";
        return 0;
    }

    auto count = cuda_device_count();
    if (!count) {
        std::cout << "STATUS: UNSUPPORTED - no CUDA device is visible: "
                  << count.error().render() << '\n';
        std::cout << "RESULT: SUCCESS (UNSUPPORTED path exits 0)\n";
        return 0;
    }
    std::cout << "STATUS: SUPPORTED - cuda_device_count=" << count.value() << '\n';

    CudaWorkloadRequest request;
    request.device_ordinal = 0;
    request.iterations = 32;
    request.elements = std::size_t{1} << 18;
    std::cout << "workload: iterations=" << request.iterations << " elements=" << request.elements
              << " device_ordinal=" << request.device_ordinal
              << " seed=" << request.seed << '\n';

    std::unique_ptr<NvmlThermalBackend> backend;
    const DeviceId nvml_device{StrongId<DeviceIdTag>{1}};
    auto nvml = NvmlThermalBackend::create();
    if (nvml) {
        backend = std::move(nvml.value());
        std::cout << "nvml: driver_version=" << backend->driver_version()
                  << " device_count=" << backend->device_count() << '\n';
    } else {
        std::cout << "nvml unavailable before the workload: " << nvml.error().render() << '\n';
    }

    std::cout << "step 1: temperature before the workload\n";
    std::optional<DegreesCelsius> before;
    if (backend != nullptr) {
        before = sample_nvml_temperature(*backend, nvml_device, "before");
    } else {
        std::cout << "       no NVML backend: the temperature before is UNKNOWN, not zero\n";
    }

    std::cout << "step 2: run the bounded workload\n";
    auto result = run_cuda_workload(request);
    if (!result) {
        std::cout << "[FAIL] run_cuda_workload: " << result.error().render() << '\n';
        std::cout << "RESULT: FAILURE\n";
        return 1;
    }
    const CudaWorkloadResult& workload = result.value();

    std::cout << "step 3: temperature after the workload\n";
    std::optional<DegreesCelsius> after;
    if (backend != nullptr) {
        after = sample_nvml_temperature(*backend, nvml_device, "after");
    } else {
        std::cout << "       no NVML backend: the temperature after is UNKNOWN, not zero\n";
    }
    if (before.has_value() && after.has_value()) {
        const TemperatureDelta delta = *after - *before;
        std::cout << "       observed_delta=" << number(delta.value()) << " C\n";
    }

    std::cout << "step 4: completed-work evidence\n";
    std::cout << "       completed=" << (workload.completed ? "true" : "false")
              << " device_name=\"" << workload.device_name << "\""
              << " sm_count=" << workload.sm_count
              << " runtime_version=" << workload.runtime_version
              << " driver_version=" << workload.driver_version << '\n';
    std::cout << "       device_checksum=" << workload.device_checksum
              << " host_checksum=" << workload.host_checksum
              << " cpu_parity=" << (workload.cpu_parity ? "true" : "false")
              << " mismatches=" << workload.mismatches << '\n';
    std::cout << "       host_elapsed_ms=" << number(workload.host_elapsed_ms)
              << " device_kernel_ms=" << number(workload.device_kernel_ms)
              << " bytes_allocated=" << workload.bytes_allocated << '\n';
    std::cout << "       device_memory_released="
              << (workload.device_memory_released ? "true" : "false") << '\n';
    if (!workload.detail.empty()) {
        std::cout << "       detail=" << workload.detail << '\n';
    }

    report(workload.completed, "the CUDA workload completed");
    report(workload.cpu_parity, "the device checksum matches the host recomputation");
    report(workload.mismatches == 0, "no element mismatched");
    report(workload.device_memory_released, "every device allocation was released");
    report(workload.host_elapsed_ms >= 0.0, "the elapsed time is a measured value");

    std::cout << (g_failures == 0 ? "RESULT: SUCCESS\n" : "RESULT: FAILURE\n");
    return g_failures == 0 ? 0 : 1;
}
