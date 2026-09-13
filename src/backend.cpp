// Thermal Governor — backend implementations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/backend.hpp"

#include <algorithm>

namespace thermal_governor {
namespace {

[[nodiscard]] Status unsupported(const char* what) {
    return Status::failure(ThermalErrorCode::CAPABILITY_UNSUPPORTED, what);
}

template <class T>
[[nodiscard]] Result<T> unsupported_result(const char* what) {
    return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED, what};
}

}  // namespace

// --- UnsupportedThermalBackend -------------------------------------------

BackendCapabilities UnsupportedThermalBackend::capabilities() {
    BackendCapabilities out;
    out.backend_name = name();
    out.backend_version = "1.0.0";
    out.provenance = Provenance::UNSUPPORTED;
    for (std::size_t i = 0; i < kThermalCapabilityCount; ++i) {
        out.capabilities.set(static_cast<ThermalCapability>(i), CapabilityState::UNSUPPORTED);
    }
    return out;
}

Result<std::vector<DeviceIdentity>> UnsupportedThermalBackend::query_device_identity() {
    return unsupported_result<std::vector<DeviceIdentity>>("device identity is unsupported");
}
Result<TemperatureReading> UnsupportedThermalBackend::query_temperature(DeviceId) {
    return unsupported_result<TemperatureReading>("temperature telemetry is unsupported");
}
Result<DegreesCelsius> UnsupportedThermalBackend::query_temperature_limit(DeviceId) {
    return unsupported_result<DegreesCelsius>("temperature limit is unsupported");
}
Result<DegreesCelsius> UnsupportedThermalBackend::query_shutdown_limit(DeviceId) {
    return unsupported_result<DegreesCelsius>("shutdown limit is unsupported");
}
Result<ThrottleObservation> UnsupportedThermalBackend::query_throttle_reasons(DeviceId) {
    return unsupported_result<ThrottleObservation>("throttle reasons are unsupported");
}
Result<MegaHertz> UnsupportedThermalBackend::query_current_clock(DeviceId) {
    return unsupported_result<MegaHertz>("current clock is unsupported");
}
Result<MegaHertz> UnsupportedThermalBackend::query_max_clock(DeviceId) {
    return unsupported_result<MegaHertz>("maximum clock is unsupported");
}
Result<FanEvidence> UnsupportedThermalBackend::query_fan_state(DeviceId) {
    return unsupported_result<FanEvidence>("fan telemetry is unsupported");
}
Result<CoolingEvidence> UnsupportedThermalBackend::query_cooling_state(DeviceId) {
    return unsupported_result<CoolingEvidence>("cooling telemetry is unsupported");
}
Result<DegreesCelsius> UnsupportedThermalBackend::query_node_temperature(NodeId) {
    return unsupported_result<DegreesCelsius>("node temperature is unsupported");
}
Result<DegreesCelsius> UnsupportedThermalBackend::query_rack_temperature(RackId) {
    return unsupported_result<DegreesCelsius>("rack temperature is unsupported");
}

// --- SyntheticThermalBackend ---------------------------------------------

SyntheticThermalBackend::SyntheticThermalBackend() {
    for (std::size_t i = 0; i < kThermalCapabilityCount; ++i) {
        declared_.set(static_cast<ThermalCapability>(i), CapabilityState::UNKNOWN);
    }
}

void SyntheticThermalBackend::add_device(DeviceIdentity identity, DegreesCelsius temperature,
                                         Provenance provenance) {
    Entry entry;
    entry.identity = std::move(identity);
    entry.temperature = temperature;
    // A synthetic backend may model a REAL device, but anything it produces
    // is modelled evidence unless it explicitly claims otherwise.
    entry.provenance = provenance == Provenance::REAL ? Provenance::SYNTHETIC : provenance;
    entries_.push_back(std::move(entry));
}

SyntheticThermalBackend::Entry* SyntheticThermalBackend::find(DeviceId device) {
    for (auto& entry : entries_) {
        if (entry.identity.device == device) {
            return &entry;
        }
    }
    return nullptr;
}

const SyntheticThermalBackend::Entry* SyntheticThermalBackend::find(DeviceId device) const {
    for (const auto& entry : entries_) {
        if (entry.identity.device == device) {
            return &entry;
        }
    }
    return nullptr;
}

void SyntheticThermalBackend::set_temperature(DeviceId device, DegreesCelsius temperature) {
    if (auto* entry = find(device); entry != nullptr) {
        entry->temperature = temperature;
    }
}

void SyntheticThermalBackend::set_throttle(DeviceId device, ThrottleObservation observation) {
    if (auto* entry = find(device); entry != nullptr) {
        entry->throttle = observation;
    }
}

void SyntheticThermalBackend::set_temperature_limit(DeviceId device, DegreesCelsius limit) {
    if (auto* entry = find(device); entry != nullptr) {
        entry->limit = limit;
    }
}

void SyntheticThermalBackend::set_current_clock(DeviceId device, MegaHertz clock) {
    if (auto* entry = find(device); entry != nullptr) {
        entry->clock = clock;
    }
}

void SyntheticThermalBackend::set_max_clock(DeviceId device, MegaHertz clock) {
    if (auto* entry = find(device); entry != nullptr) {
        entry->max_clock = clock;
    }
}

void SyntheticThermalBackend::set_fan_state(DeviceId device, FanEvidence fan) {
    if (auto* entry = find(device); entry != nullptr) {
        entry->fan = fan;
    }
}

void SyntheticThermalBackend::set_cooling_state(DeviceId device, CoolingEvidence cooling) {
    if (auto* entry = find(device); entry != nullptr) {
        entry->cooling = cooling;
    }
}

void SyntheticThermalBackend::set_capability(ThermalCapability capability,
                                             CapabilityState state) {
    declared_explicit_ = true;
    declared_.set(capability, state);
}

void SyntheticThermalBackend::clear_devices() {
    entries_.clear();
}

BackendCapabilities SyntheticThermalBackend::capabilities() {
    BackendCapabilities out;
    out.backend_name = name();
    out.backend_version = "1.0.0";
    out.provenance = Provenance::SYNTHETIC;

    CapabilitySet set;
    for (std::size_t i = 0; i < kThermalCapabilityCount; ++i) {
        const auto capability = static_cast<ThermalCapability>(i);
        if (declared_explicit_ && declared_.get(capability) != CapabilityState::UNKNOWN) {
            set.set(capability, declared_.get(capability));
            continue;
        }
        // Derive from what the synthetic devices actually expose: a
        // capability is only supported when at least one device models it.
        bool present = false;
        for (const auto& entry : entries_) {
            switch (capability) {
                case ThermalCapability::DEVICE_IDENTITY: present = true; break;
                case ThermalCapability::TEMPERATURE: present = true; break;
                case ThermalCapability::TEMPERATURE_LIMIT: present = entry.limit.has_value(); break;
                case ThermalCapability::TEMPERATURE_THRESHOLD_SHUTDOWN:
                    present = entry.shutdown.has_value();
                    break;
                case ThermalCapability::TEMPERATURE_THRESHOLD_SLOWDOWN: present = false; break;
                case ThermalCapability::THROTTLE_REASONS: present = entry.throttle.has_value(); break;
                case ThermalCapability::CURRENT_CLOCK: present = entry.clock.has_value(); break;
                case ThermalCapability::MAX_CLOCK: present = entry.max_clock.has_value(); break;
                case ThermalCapability::FAN_TELEMETRY: present = entry.fan.has_value(); break;
                case ThermalCapability::COOLING_TELEMETRY:
                    present = entry.cooling.has_value();
                    break;
                case ThermalCapability::THERMAL_DOMAIN_METADATA: present = true; break;
                case ThermalCapability::NODE_THERMAL_METADATA: present = false; break;
                case ThermalCapability::RACK_THERMAL_METADATA: present = false; break;
                case ThermalCapability::ACTUAL_THROTTLE_ENFORCEMENT: present = false; break;
                case ThermalCapability::CLOCK_REDUCTION_ENFORCEMENT: present = false; break;
                case ThermalCapability::COUNT: present = false; break;
            }
            if (present) {
                break;
            }
        }
        set.set(capability, present ? CapabilityState::SUPPORTED_SYNTHETIC
                                    : CapabilityState::UNSUPPORTED);
    }
    out.capabilities = std::move(set);
    return out;
}

Result<std::vector<DeviceIdentity>> SyntheticThermalBackend::query_device_identity() {
    std::vector<DeviceIdentity> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        out.push_back(entry.identity);
    }
    return out;
}

Result<TemperatureReading> SyntheticThermalBackend::query_temperature(DeviceId device) {
    const auto* entry = find(device);
    if (entry == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "synthetic device is not modelled"};
    }
    TemperatureReading reading;
    reading.temperature = entry->temperature;
    reading.source = MeasurementSource::SYNTHETIC_MODEL;
    return reading;
}

Result<DegreesCelsius> SyntheticThermalBackend::query_temperature_limit(DeviceId device) {
    const auto* entry = find(device);
    if (entry == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "synthetic device is not modelled"};
    }
    if (!entry->limit.has_value()) {
        return unsupported_result<DegreesCelsius>("synthetic temperature limit is not modelled");
    }
    return *entry->limit;
}

Result<DegreesCelsius> SyntheticThermalBackend::query_shutdown_limit(DeviceId device) {
    const auto* entry = find(device);
    if (entry == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "synthetic device is not modelled"};
    }
    if (!entry->shutdown.has_value()) {
        return unsupported_result<DegreesCelsius>("synthetic shutdown limit is not modelled");
    }
    return *entry->shutdown;
}

Result<ThrottleObservation> SyntheticThermalBackend::query_throttle_reasons(DeviceId device) {
    const auto* entry = find(device);
    if (entry == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "synthetic device is not modelled"};
    }
    if (!entry->throttle.has_value()) {
        return unsupported_result<ThrottleObservation>("synthetic throttle evidence is not modelled");
    }
    return *entry->throttle;
}

Result<MegaHertz> SyntheticThermalBackend::query_current_clock(DeviceId device) {
    const auto* entry = find(device);
    if (entry == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "synthetic device is not modelled"};
    }
    if (!entry->clock.has_value()) {
        return unsupported_result<MegaHertz>("synthetic current clock is not modelled");
    }
    return *entry->clock;
}

Result<MegaHertz> SyntheticThermalBackend::query_max_clock(DeviceId device) {
    const auto* entry = find(device);
    if (entry == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "synthetic device is not modelled"};
    }
    if (!entry->max_clock.has_value()) {
        return unsupported_result<MegaHertz>("synthetic maximum clock is not modelled");
    }
    return *entry->max_clock;
}

Result<FanEvidence> SyntheticThermalBackend::query_fan_state(DeviceId device) {
    const auto* entry = find(device);
    if (entry == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "synthetic device is not modelled"};
    }
    if (!entry->fan.has_value()) {
        return unsupported_result<FanEvidence>("synthetic fan telemetry is not modelled");
    }
    return *entry->fan;
}

Result<CoolingEvidence> SyntheticThermalBackend::query_cooling_state(DeviceId device) {
    const auto* entry = find(device);
    if (entry == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "synthetic device is not modelled"};
    }
    if (!entry->cooling.has_value()) {
        return unsupported_result<CoolingEvidence>("synthetic cooling telemetry is not modelled");
    }
    return *entry->cooling;
}

Result<DegreesCelsius> SyntheticThermalBackend::query_node_temperature(NodeId) {
    return unsupported_result<DegreesCelsius>("synthetic node temperature is not modelled");
}

Result<DegreesCelsius> SyntheticThermalBackend::query_rack_temperature(RackId) {
    return unsupported_result<DegreesCelsius>("synthetic rack temperature is not modelled");
}

}  // namespace thermal_governor
