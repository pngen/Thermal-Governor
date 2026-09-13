// Thermal Governor — CUDA completed-work proof (no-CUDA configuration).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/cuda_workload.hpp"

#ifndef THERMAL_GOVERNOR_HAS_CUDA

namespace thermal_governor {

bool cuda_support_compiled() noexcept { return false; }

Result<int> cuda_device_count() {
    return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                        "this build was configured without CUDA support"};
}

Result<CudaWorkloadResult> run_cuda_workload(const CudaWorkloadRequest& request) {
    (void)request;
    return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                        "this build was configured without CUDA support"};
}

Provenance cuda_proof_provenance() noexcept { return Provenance::UNSUPPORTED; }

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_HAS_CUDA
