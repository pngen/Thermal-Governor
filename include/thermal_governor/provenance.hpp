// Thermal Governor — provenance classification.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_PROVENANCE_HPP
#define THERMAL_GOVERNOR_PROVENANCE_HPP

#include <cstdint>
#include <string_view>

namespace thermal_governor {

/// Honest classification of where a piece of thermal knowledge came from.
///
/// Provenance is never silently upgraded. A synthetic rack envelope stays
/// synthetic forever; an absent sensor is UNSUPPORTED, never zero.
enum class Provenance : std::uint8_t {
    UNKNOWN = 0,
    REAL = 1,
    SYNTHETIC = 2,
    UNSUPPORTED = 3,
};

[[nodiscard]] constexpr std::string_view to_string(Provenance p) noexcept {
    switch (p) {
        case Provenance::UNKNOWN: return "UNKNOWN";
        case Provenance::REAL: return "REAL";
        case Provenance::SYNTHETIC: return "SYNTHETIC";
        case Provenance::UNSUPPORTED: return "UNSUPPORTED";
    }
    return "UNRECOGNISED_PROVENANCE";
}

/// Ordered strength of provenance for combination rules. REAL evidence
/// combined with SYNTHETIC evidence yields SYNTHETIC: the weakest honest
/// classification wins.
[[nodiscard]] constexpr Provenance combine(Provenance a, Provenance b) noexcept {
    if (a == b) {
        return a;
    }
    if (a == Provenance::UNSUPPORTED || b == Provenance::UNSUPPORTED) {
        return Provenance::UNSUPPORTED;
    }
    if (a == Provenance::UNKNOWN || b == Provenance::UNKNOWN) {
        // UNKNOWN dominates only over UNSUPPORTED; anything concrete plus
        // UNKNOWN degrades to SYNTHETIC-free UNKNOWN.
        return Provenance::UNKNOWN;
    }
    return Provenance::SYNTHETIC;
}

/// Origin of a temperature measurement.
enum class MeasurementSource : std::uint8_t {
    UNKNOWN = 0,
    NVML_GPU_TEMPERATURE,
    NVML_TEMPERATURE_THRESHOLD,
    CUDA_DEVICE_ATTRIBUTE,
    PLATFORM_SENSOR,
    NODE_SENSOR,
    RACK_SENSOR,
    COOLING_ZONE_SENSOR,
    OPERATOR_INJECTED,
    SYNTHETIC_MODEL,
};

[[nodiscard]] constexpr std::string_view to_string(MeasurementSource s) noexcept {
    switch (s) {
        case MeasurementSource::UNKNOWN: return "UNKNOWN";
        case MeasurementSource::NVML_GPU_TEMPERATURE: return "NVML_GPU_TEMPERATURE";
        case MeasurementSource::NVML_TEMPERATURE_THRESHOLD: return "NVML_TEMPERATURE_THRESHOLD";
        case MeasurementSource::CUDA_DEVICE_ATTRIBUTE: return "CUDA_DEVICE_ATTRIBUTE";
        case MeasurementSource::PLATFORM_SENSOR: return "PLATFORM_SENSOR";
        case MeasurementSource::NODE_SENSOR: return "NODE_SENSOR";
        case MeasurementSource::RACK_SENSOR: return "RACK_SENSOR";
        case MeasurementSource::COOLING_ZONE_SENSOR: return "COOLING_ZONE_SENSOR";
        case MeasurementSource::OPERATOR_INJECTED: return "OPERATOR_INJECTED";
        case MeasurementSource::SYNTHETIC_MODEL: return "SYNTHETIC_MODEL";
    }
    return "UNRECOGNISED_SOURCE";
}

/// Integrity state of a received evidence record.
enum class IntegrityStatus : std::uint8_t {
    OK = 0,
    DEGRADED = 1,
    FAILED = 2,
};

[[nodiscard]] constexpr std::string_view to_string(IntegrityStatus s) noexcept {
    switch (s) {
        case IntegrityStatus::OK: return "OK";
        case IntegrityStatus::DEGRADED: return "DEGRADED";
        case IntegrityStatus::FAILED: return "FAILED";
    }
    return "UNRECOGNISED_INTEGRITY";
}

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_PROVENANCE_HPP
