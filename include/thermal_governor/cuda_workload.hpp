// Thermal Governor — real CUDA completed-work proof.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_CUDA_WORKLOAD_HPP
#define THERMAL_GOVERNOR_CUDA_WORKLOAD_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "thermal_governor/error.hpp"
#include "thermal_governor/provenance.hpp"

namespace thermal_governor {

/// A deterministic, bounded CUDA workload request.
///
/// The workload is sized to produce meaningful thermal telemetry without
/// unsafe stress. Thermal Governor never disables cooling and never drives
/// hardware into genuine thermal distress.
struct CudaWorkloadRequest {
    int device_ordinal = 0;
    std::uint32_t iterations = 64;
    std::size_t elements = 1U << 20;
    std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
};

/// Result of a completed CUDA workload.
struct CudaWorkloadResult {
    bool completed = false;

    std::string device_name;
    int device_ordinal = 0;
    int sm_count = 0;
    int driver_version = 0;
    int runtime_version = 0;

    std::uint64_t device_checksum = 0;
    std::uint64_t host_checksum = 0;
    /// True when the host-side recomputation matched the device result.
    bool cpu_parity = false;
    std::uint64_t mismatches = 0;

    double host_elapsed_ms = 0.0;
    double device_kernel_ms = 0.0;

    std::size_t bytes_allocated = 0;
    /// True when cudaFree returned success for every allocation.
    bool device_memory_released = false;

    std::string detail;
};

/// True when this build contains real CUDA instrumentation.
[[nodiscard]] bool cuda_support_compiled() noexcept;

/// Number of CUDA devices visible to the driver, or an error.
[[nodiscard]] Result<int> cuda_device_count();

/// Run the deterministic bounded workload on a real CUDA device.
[[nodiscard]] Result<CudaWorkloadResult> run_cuda_workload(const CudaWorkloadRequest& request);

/// Provenance of the CUDA proof in this build.
[[nodiscard]] Provenance cuda_proof_provenance() noexcept;

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_CUDA_WORKLOAD_HPP
