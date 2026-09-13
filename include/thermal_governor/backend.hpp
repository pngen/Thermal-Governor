// Thermal Governor — vendor-neutral thermal backend interface.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_BACKEND_HPP
#define THERMAL_GOVERNOR_BACKEND_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "thermal_governor/capability.hpp"
#include "thermal_governor/error.hpp"
#include "thermal_governor/evidence.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/provenance.hpp"
#include "thermal_governor/quantity.hpp"
#include "thermal_governor/throttle.hpp"

namespace thermal_governor {

/// Stable, vendor-neutral device identity.
struct DeviceIdentity {
    DeviceId device{};
    std::string uuid;
    std::string pci_bus_id;
    std::string name;
    std::uint32_t index = 0;

    friend bool operator==(const DeviceIdentity&, const DeviceIdentity&) = default;
};

/// A temperature reading with its source classification.
struct TemperatureReading {
    DegreesCelsius temperature{};
    MeasurementSource source = MeasurementSource::UNKNOWN;
};

/// Narrow vendor-neutral thermal telemetry interface.
///
/// No vendor SDK object ever crosses this boundary: the public core carries
/// only Thermal Governor types.
class IThermalBackend {
public:
    virtual ~IThermalBackend() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual BackendCapabilities capabilities() = 0;

    [[nodiscard]] virtual Result<std::vector<DeviceIdentity>> query_device_identity() = 0;
    [[nodiscard]] virtual Result<TemperatureReading> query_temperature(DeviceId device) = 0;
    [[nodiscard]] virtual Result<DegreesCelsius> query_temperature_limit(DeviceId device) = 0;
    [[nodiscard]] virtual Result<DegreesCelsius> query_shutdown_limit(DeviceId device) = 0;
    [[nodiscard]] virtual Result<ThrottleObservation> query_throttle_reasons(DeviceId device) = 0;
    [[nodiscard]] virtual Result<MegaHertz> query_current_clock(DeviceId device) = 0;
    [[nodiscard]] virtual Result<MegaHertz> query_max_clock(DeviceId device) = 0;
    [[nodiscard]] virtual Result<FanEvidence> query_fan_state(DeviceId device) = 0;
    [[nodiscard]] virtual Result<CoolingEvidence> query_cooling_state(DeviceId device) = 0;
    [[nodiscard]] virtual Result<DegreesCelsius> query_node_temperature(NodeId node) = 0;
    [[nodiscard]] virtual Result<DegreesCelsius> query_rack_temperature(RackId rack) = 0;

    /// Optional enforcement path. Backends that cannot enforce return
    /// CAPABILITY_UNSUPPORTED rather than pretending to act.
    [[nodiscard]] virtual Status request_clock_ceiling(DeviceId device, MegaHertz ceiling) {
        (void)device;
        (void)ceiling;
        return Status::failure(ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                               "backend does not own clock enforcement");
    }
};

/// A backend that exposes no thermal telemetry at all. Every capability is
/// UNSUPPORTED; nothing is fabricated.
class UnsupportedThermalBackend final : public IThermalBackend {
public:
    [[nodiscard]] std::string name() const override { return "unsupported"; }
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
};

/// A programmable backend used for synthetic thermal scenarios and for
/// deterministic tests. All evidence it produces is SYNTHETIC.
class SyntheticThermalBackend final : public IThermalBackend {
public:
    SyntheticThermalBackend();

    /// Register a synthetic device with an initial temperature.
    void add_device(DeviceIdentity identity, DegreesCelsius temperature,
                    Provenance provenance = Provenance::SYNTHETIC);

    void set_temperature(DeviceId device, DegreesCelsius temperature);
    void set_throttle(DeviceId device, ThrottleObservation observation);
    void set_temperature_limit(DeviceId device, DegreesCelsius limit);
    void set_current_clock(DeviceId device, MegaHertz clock);
    void set_max_clock(DeviceId device, MegaHertz clock);
    void set_fan_state(DeviceId device, FanEvidence fan);
    void set_cooling_state(DeviceId device, CoolingEvidence cooling);
    /// Declare a capability state explicitly, including UNSUPPORTED.
    void set_capability(ThermalCapability capability, CapabilityState state);
    void clear_devices();

    [[nodiscard]] std::string name() const override { return "synthetic"; }
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

private:
    struct Entry {
        DeviceIdentity identity;
        DegreesCelsius temperature{};
        Provenance provenance = Provenance::SYNTHETIC;
        std::optional<ThrottleObservation> throttle;
        std::optional<DegreesCelsius> limit;
        std::optional<DegreesCelsius> shutdown;
        std::optional<MegaHertz> clock;
        std::optional<MegaHertz> max_clock;
        std::optional<FanEvidence> fan;
        std::optional<CoolingEvidence> cooling;
    };

    [[nodiscard]] Entry* find(DeviceId device);
    [[nodiscard]] const Entry* find(DeviceId device) const;

    std::vector<Entry> entries_;
    CapabilitySet declared_;
    bool declared_explicit_ = false;
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_BACKEND_HPP
