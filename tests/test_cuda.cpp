// Thermal Governor — REAL CUDA completed-work proof.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <string>

#include "framework.hpp"
#include "thermal_governor/cuda_workload.hpp"
#include "thermal_governor/nvml_backend.hpp"

namespace {

using namespace thermal_governor;

void report_unsupported(const char* what) {
    std::printf("NOTE cuda::%s UNSUPPORTED in this configuration\n", what);
    std::fflush(stdout);
}

}  // namespace

TG_CASE(cuda, support_is_declared_honestly) {
    const bool compiled = cuda_support_compiled();
    const Provenance provenance = cuda_proof_provenance();
    std::printf("NOTE cuda compiled=%s provenance=%s\n", compiled ? "true" : "false",
                std::string(to_string(provenance)).c_str());
    std::fflush(stdout);
    if (compiled) {
        TG_CHECK_EQ(provenance, Provenance::REAL);
    } else {
        // A build without CUDA must say UNSUPPORTED, never UNKNOWN.
        TG_CHECK_EQ(provenance, Provenance::UNSUPPORTED);
        auto count = cuda_device_count();
        TG_CHECK(!count.has_value());
        TG_CHECK_EQ(count.error().code, ThermalErrorCode::CAPABILITY_UNSUPPORTED);
    }
}

TG_CASE(cuda, completed_work_with_cpu_parity) {
    TG_PHASE("CUDA_ENUMERATE");
    if (!cuda_support_compiled()) {
        report_unsupported("completed_work_with_cpu_parity");
        return;
    }
    auto count = cuda_device_count();
    TG_OK(count);
    if (count.value() <= 0) {
        report_unsupported("completed_work_with_cpu_parity");
        return;
    }

    // A deliberately small, bounded workload: enough real work to produce
    // meaningful telemetry, never enough to distress the hardware.
    CudaWorkloadRequest request;
    request.device_ordinal = 0;
    request.iterations = 32;
    request.elements = 1U << 18;

    DeviceId device{};
    bool have_temperature = false;
    DegreesCelsius before{};
    auto backend = NvmlThermalBackend::create();
    if (backend.has_value()) {
        auto identities = backend.value()->query_device_identity();
        if (identities.has_value() && !identities.value().empty()) {
            device = identities.value().front().device;
            auto reading = backend.value()->query_temperature(device);
            if (reading.has_value()) {
                before = reading.value().temperature;
                have_temperature = true;
                std::printf("NOTE cuda pre-workload temperature=%s C\n", before.render().c_str());
                std::fflush(stdout);
            }
        }
    }

    TG_PHASE("CUDA_EXECUTE");
    auto result = run_cuda_workload(request);
    TG_OK(result);
    TG_CHECK(result.value().completed);
    TG_CHECK(result.value().cpu_parity);
    TG_CHECK_EQ(result.value().mismatches, 0U);
    TG_CHECK_EQ(result.value().device_checksum, result.value().host_checksum);
    TG_CHECK(result.value().device_memory_released);
    TG_CHECK(result.value().bytes_allocated > 0);
    TG_CHECK(result.value().sm_count > 0);
    std::printf(
        "NOTE cuda device=%s sm=%d driver=%d runtime=%d kernel_ms=%.3f checksum=%llu "
        "parity=true memory_released=true\n",
        result.value().device_name.c_str(), result.value().sm_count,
        result.value().driver_version, result.value().runtime_version,
        result.value().device_kernel_ms,
        static_cast<unsigned long long>(result.value().device_checksum));
    std::fflush(stdout);

    TG_PHASE("CUDA_POST_TELEMETRY");
    if (have_temperature && backend.has_value()) {
        auto reading = backend.value()->query_temperature(device);
        TG_OK(reading);
        std::printf("NOTE cuda post-workload temperature=%s C\n",
                    reading.value().temperature.render().c_str());
        std::fflush(stdout);
        // This workload is bounded on purpose: it is a completed-work proof,
        // not a stress test, and no throttling claim is made.
        auto observation = backend.value()->query_throttle_reasons(device);
        if (observation.has_value()) {
            std::printf("NOTE cuda post-workload throttle class=%s\n",
                        std::string(to_string(observation.value().classification)).c_str());
            std::fflush(stdout);
        }
    }
}

TG_CASE(cuda, invalid_request_is_rejected) {
    if (!cuda_support_compiled()) {
        report_unsupported("invalid_request_is_rejected");
        return;
    }
    CudaWorkloadRequest request;
    request.iterations = 0;
    auto result = run_cuda_workload(request);
    TG_CHECK(!result.has_value());
    TG_CHECK_EQ(result.error().code, ThermalErrorCode::INVALID_ARGUMENT);
}

TG_CASE(cuda, absent_device_ordinal_is_rejected) {
    if (!cuda_support_compiled()) {
        report_unsupported("absent_device_ordinal_is_rejected");
        return;
    }
    CudaWorkloadRequest request;
    request.device_ordinal = 9999;
    auto result = run_cuda_workload(request);
    TG_CHECK(!result.has_value());
    TG_CHECK_EQ(result.error().code, ThermalErrorCode::UNKNOWN_DEVICE);
}
