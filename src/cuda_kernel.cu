// Thermal Governor — CUDA completed-work kernel and host driver.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/cuda_workload.hpp"

#ifdef THERMAL_GOVERNOR_HAS_CUDA

#include <cuda_runtime.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace thermal_governor {
namespace {

/// Deterministic integer mixing step. Integer arithmetic is used
/// deliberately so that device and host results are bit-identical: a
/// floating-point reduction would make CPU parity a tolerance test rather
/// than an exact proof.
__host__ __device__ inline std::uint32_t mix(std::uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    x += 0x9E3779B9u;
    x ^= (x >> 11) * 0x85EBCA6Bu;
    return x;
}

__global__ void mix_kernel(std::uint32_t* data, std::size_t count, std::uint32_t rounds) {
    const std::size_t index =
        static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= count) {
        return;
    }
    std::uint32_t value = data[index];
    for (std::uint32_t round = 0; round < rounds; ++round) {
        value = mix(value);
    }
    data[index] = value;
}

[[nodiscard]] std::string cuda_error_text(cudaError_t code) {
    return std::string(cudaGetErrorString(code));
}

void host_mix(std::vector<std::uint32_t>& data, std::uint32_t rounds) {
    for (auto& value : data) {
        for (std::uint32_t round = 0; round < rounds; ++round) {
            value = mix(value);
        }
    }
}

}  // namespace

bool cuda_support_compiled() noexcept { return true; }

Provenance cuda_proof_provenance() noexcept { return Provenance::REAL; }

Result<int> cuda_device_count() {
    int count = 0;
    const cudaError_t code = cudaGetDeviceCount(&count);
    if (code != cudaSuccess) {
        return ThermalError{ThermalErrorCode::BACKEND_FAILURE,
                            "cudaGetDeviceCount failed: " + cuda_error_text(code)};
    }
    return count;
}

Result<CudaWorkloadResult> run_cuda_workload(const CudaWorkloadRequest& request) {
    if (request.elements == 0) {
        return ThermalError{ThermalErrorCode::INVALID_ARGUMENT, "workload element count is zero"};
    }
    if (request.iterations == 0) {
        return ThermalError{ThermalErrorCode::INVALID_ARGUMENT, "workload iteration count is zero"};
    }

    CudaWorkloadResult result;
    result.device_ordinal = request.device_ordinal;

    int device_count = 0;
    cudaError_t code = cudaGetDeviceCount(&device_count);
    if (code != cudaSuccess) {
        return ThermalError{ThermalErrorCode::BACKEND_FAILURE,
                            "cudaGetDeviceCount failed: " + cuda_error_text(code)};
    }
    if (request.device_ordinal < 0 || request.device_ordinal >= device_count) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE,
                            "requested CUDA device ordinal is not present"};
    }

    code = cudaSetDevice(request.device_ordinal);
    if (code != cudaSuccess) {
        return ThermalError{ThermalErrorCode::BACKEND_FAILURE,
                            "cudaSetDevice failed: " + cuda_error_text(code)};
    }

    cudaDeviceProp properties{};
    code = cudaGetDeviceProperties(&properties, request.device_ordinal);
    if (code != cudaSuccess) {
        return ThermalError{ThermalErrorCode::BACKEND_FAILURE,
                            "cudaGetDeviceProperties failed: " + cuda_error_text(code)};
    }
    result.device_name = properties.name;
    result.sm_count = properties.multiProcessorCount;
    cudaDriverGetVersion(&result.driver_version);
    cudaRuntimeGetVersion(&result.runtime_version);

    const std::size_t bytes = request.elements * sizeof(std::uint32_t);
    result.bytes_allocated = bytes;

    // Deterministic, reproducible input derived only from the seed.
    std::vector<std::uint32_t> host(request.elements);
    std::uint32_t state = static_cast<std::uint32_t>(request.seed ^ (request.seed >> 32));
    for (std::size_t i = 0; i < request.elements; ++i) {
        state = state * 1664525u + 1013904223u;
        host[i] = state;
    }

    std::uint32_t* device_data = nullptr;
    code = cudaMalloc(reinterpret_cast<void**>(&device_data), bytes);
    if (code != cudaSuccess) {
        return ThermalError{ThermalErrorCode::BACKEND_FAILURE,
                            "cudaMalloc failed: " + cuda_error_text(code)};
    }

    code = cudaMemcpy(device_data, host.data(), bytes, cudaMemcpyHostToDevice);
    if (code != cudaSuccess) {
        cudaFree(device_data);
        return ThermalError{ThermalErrorCode::BACKEND_FAILURE,
                            "host-to-device copy failed: " + cuda_error_text(code)};
    }

    const auto start = std::chrono::steady_clock::now();

    const int threads = 256;
    const int blocks = static_cast<int>((request.elements + threads - 1) / threads);
    mix_kernel<<<blocks, threads>>>(device_data, request.elements, request.iterations);

    code = cudaGetLastError();
    if (code != cudaSuccess) {
        cudaFree(device_data);
        return ThermalError{ThermalErrorCode::BACKEND_FAILURE,
                            "kernel launch failed: " + cuda_error_text(code)};
    }
    code = cudaDeviceSynchronize();
    const auto finish = std::chrono::steady_clock::now();
    if (code != cudaSuccess) {
        cudaFree(device_data);
        return ThermalError{ThermalErrorCode::BACKEND_FAILURE,
                            "kernel execution failed: " + cuda_error_text(code)};
    }
    result.device_kernel_ms =
        std::chrono::duration<double, std::milli>(finish - start).count();

    std::vector<std::uint32_t> device_result(request.elements);
    code = cudaMemcpy(device_result.data(), device_data, bytes, cudaMemcpyDeviceToHost);
    if (code != cudaSuccess) {
        cudaFree(device_data);
        return ThermalError{ThermalErrorCode::BACKEND_FAILURE,
                            "device-to-host copy failed: " + cuda_error_text(code)};
    }

    const cudaError_t free_code = cudaFree(device_data);
    result.device_memory_released = free_code == cudaSuccess;
    if (free_code != cudaSuccess) {
        return ThermalError{ThermalErrorCode::BACKEND_FAILURE,
                            "cudaFree failed: " + cuda_error_text(free_code)};
    }

    host_mix(host, request.iterations);

    std::uint64_t device_checksum = 1469598103934665603ULL;
    std::uint64_t host_checksum = 1469598103934665603ULL;
    std::uint64_t mismatches = 0;
    for (std::size_t i = 0; i < request.elements; ++i) {
        device_checksum = (device_checksum ^ device_result[i]) * 1099511628211ULL;
        host_checksum = (host_checksum ^ host[i]) * 1099511628211ULL;
        if (device_result[i] != host[i]) {
            ++mismatches;
        }
    }

    result.device_checksum = device_checksum;
    result.host_checksum = host_checksum;
    result.mismatches = mismatches;
    result.cpu_parity = mismatches == 0 && device_checksum == host_checksum;
    result.host_elapsed_ms = std::chrono::duration<double, std::milli>(finish - start).count();
    result.completed = true;
    result.detail = "deterministic integer mixing workload completed";

    // Leave the device in a clean state for the next caller.
    cudaDeviceReset();
    return result;
}

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_HAS_CUDA
