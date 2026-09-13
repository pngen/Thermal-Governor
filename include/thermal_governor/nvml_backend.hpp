// Thermal Governor — NVIDIA NVML thermal backend (dynamically loaded).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_NVML_BACKEND_HPP
#define THERMAL_GOVERNOR_NVML_BACKEND_HPP

#include <memory>
#include <string>

#include "thermal_governor/backend.hpp"
#include "thermal_governor/error.hpp"

namespace thermal_governor {

/// Dynamically loaded NVML thermal backend.
///
/// NVML is loaded at run time from the platform library path. No NVML type
/// or handle appears in this header: the public core never sees vendor
/// objects.
///
/// Every capability is probed and classified independently. A device that
/// reports a temperature but no fan telemetry yields SUPPORTED_REAL for
/// TEMPERATURE and UNSUPPORTED for FAN_TELEMETRY.
class NvmlThermalBackend final : public IThermalBackend {
public:
    ~NvmlThermalBackend() override;

    /// Load NVML and initialise it. Fails with CAPABILITY_UNSUPPORTED when
    /// the library is absent or initialisation fails.
    [[nodiscard]] static Result<std::unique_ptr<NvmlThermalBackend>> create();

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] BackendCapabilities capabilities() override;

    [[nodiscard]] Result<std::vector<DeviceIdentity>> query_device_identity() override;
    [[nodiscard]] Result<TemperatureReading> query_temperature(DeviceId device) override;
    [[nodiscard]] Result<DegreesCelsius> query_temperature_limit(DeviceId device) override;
    [[nodiscard]] Result<DegreesCelsius> query_shutdown_limit(DeviceId device) override;
    [[nodiscard]] Result<ThrottleObservation> query_throttle_reasons(DeviceId device) override;
    [[nodiscard]] Result<MegaHertz> query_current_clock(DeviceId device) override;
    [[nodiscard]] Result<MegaHertz> query_max_clock(DeviceId device) override;
    [[nodiscard]] Result<FanEvidence> query_fan_state(DeviceId device) override;
    [[nodiscard]] Result<CoolingEvidence> query_cooling_state(DeviceId device) override;
    [[nodiscard]] Result<DegreesCelsius> query_node_temperature(NodeId node) override;
    [[nodiscard]] Result<DegreesCelsius> query_rack_temperature(RackId rack) override;

    [[nodiscard]] std::string driver_version() const;
    [[nodiscard]] std::string nvml_version() const;
    [[nodiscard]] std::size_t device_count() const noexcept;

private:
    NvmlThermalBackend();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// True when NVML is loadable and initialisable on this host.
[[nodiscard]] bool nvml_available() noexcept;

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_NVML_BACKEND_HPP
