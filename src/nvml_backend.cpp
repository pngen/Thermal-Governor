// Thermal Governor — NVIDIA NVML thermal backend.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/nvml_backend.hpp"

#include <array>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace thermal_governor {
namespace {

// Minimal, self-contained NVML ABI declarations. Thermal Governor does not
// depend on the NVML SDK being installed to build, and no NVML type ever
// crosses the public header boundary.

using nvmlDevice_st = void*;
using nvmlReturn_t = int;

constexpr nvmlReturn_t kSuccess = 0;
constexpr nvmlReturn_t kErrorNotSupported = 3;

constexpr int kTemperatureGpu = 0;
constexpr int kThresholdShutdown = 0;
constexpr int kThresholdSlowdown = 1;
constexpr int kThresholdGpuMax = 3;
constexpr int kClockGraphics = 0;
constexpr int kClockSm = 1;

#pragma pack(push, 1)
struct NvmlPciInfo {
    char bus_id_legacy[16];
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pci_device_id;
    unsigned int pci_subsystem_id;
    char bus_id[32];
};
#pragma pack(pop)

#ifdef _WIN32
using LibraryHandle = HMODULE;

LibraryHandle open_library(const char* name) { return ::LoadLibraryA(name); }
void close_library(LibraryHandle handle) {
    if (handle != nullptr) {
        ::FreeLibrary(handle);
    }
}
template <class Fn>
Fn resolve(LibraryHandle handle, const char* name) {
    return reinterpret_cast<Fn>(::GetProcAddress(handle, name));
}
constexpr const char* kNvmlLibraryName = "nvml.dll";
#else
using LibraryHandle = void*;
LibraryHandle open_library(const char* name) { return ::dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void close_library(LibraryHandle handle) {
    if (handle != nullptr) {
        ::dlclose(handle);
    }
}
template <class Fn>
Fn resolve(LibraryHandle handle, const char* name) {
    return reinterpret_cast<Fn>(::dlsym(handle, name));
}
constexpr const char* kNvmlLibraryName = "libnvidia-ml.so.1";
#endif

struct NvmlApi {
    nvmlReturn_t (*init)() = nullptr;
    nvmlReturn_t (*shutdown)() = nullptr;
    const char* (*error_string)(nvmlReturn_t) = nullptr;
    nvmlReturn_t (*system_get_driver_version)(char*, unsigned int) = nullptr;
    nvmlReturn_t (*system_get_nvml_version)(char*, unsigned int) = nullptr;
    nvmlReturn_t (*device_get_count)(unsigned int*) = nullptr;
    nvmlReturn_t (*device_get_handle_by_index)(unsigned int, nvmlDevice_st*) = nullptr;
    nvmlReturn_t (*device_get_uuid)(nvmlDevice_st, char*, unsigned int) = nullptr;
    nvmlReturn_t (*device_get_name)(nvmlDevice_st, char*, unsigned int) = nullptr;
    nvmlReturn_t (*device_get_pci_info)(nvmlDevice_st, NvmlPciInfo*) = nullptr;
    nvmlReturn_t (*device_get_temperature)(nvmlDevice_st, int, unsigned int*) = nullptr;
    nvmlReturn_t (*device_get_temperature_threshold)(nvmlDevice_st, int, unsigned int*) = nullptr;
    nvmlReturn_t (*device_get_current_clocks_throttle_reasons)(nvmlDevice_st,
                                                               unsigned long long*) = nullptr;
    nvmlReturn_t (*device_get_clock_info)(nvmlDevice_st, int, unsigned int*) = nullptr;
    nvmlReturn_t (*device_get_max_clock_info)(nvmlDevice_st, int, unsigned int*) = nullptr;
    nvmlReturn_t (*device_get_num_fans)(nvmlDevice_st, unsigned int*) = nullptr;
    nvmlReturn_t (*device_get_fan_speed)(nvmlDevice_st, unsigned int*) = nullptr;
};

[[nodiscard]] bool all_present(const NvmlApi& api) {
    return api.init != nullptr && api.shutdown != nullptr && api.error_string != nullptr &&
           api.device_get_count != nullptr && api.device_get_handle_by_index != nullptr &&
           api.device_get_uuid != nullptr && api.device_get_name != nullptr &&
           api.device_get_temperature != nullptr;
}

}  // namespace

struct NvmlThermalBackend::Impl {
    LibraryHandle library = nullptr;
    NvmlApi api;
    bool initialised = false;
    std::vector<nvmlDevice_st> handles;
    std::vector<std::string> uuids;
    std::vector<std::string> names;
    std::vector<std::string> pci_ids;
    BackendCapabilities capabilities;
    bool capabilities_resolved = false;

    ~Impl() {
        if (initialised && api.shutdown != nullptr) {
            api.shutdown();
        }
        close_library(library);
    }

    [[nodiscard]] nvmlDevice_st handle_for(DeviceId device) const {
        const std::uint64_t index = device.value();
        if (index == 0 || index > handles.size()) {
            return nullptr;
        }
        return handles[static_cast<std::size_t>(index - 1)];
    }

    [[nodiscard]] ThermalError error_from(nvmlReturn_t code, const char* what) const {
        std::string detail(what);
        if (api.error_string != nullptr) {
            detail += ": ";
            detail += api.error_string(code);
        }
        return ThermalError{ThermalErrorCode::BACKEND_FAILURE, std::move(detail)};
    }
};

NvmlThermalBackend::NvmlThermalBackend() : impl_(std::make_unique<Impl>()) {}
NvmlThermalBackend::~NvmlThermalBackend() = default;

bool nvml_available() noexcept {
    auto backend = NvmlThermalBackend::create();
    return backend.has_value();
}

Result<std::unique_ptr<NvmlThermalBackend>> NvmlThermalBackend::create() {
#ifndef THERMAL_GOVERNOR_HAS_NVML
    return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                        "this build was configured without the NVML backend"};
#else
    std::unique_ptr<NvmlThermalBackend> backend(new NvmlThermalBackend());
    Impl& impl = *backend->impl_;

    impl.library = open_library(kNvmlLibraryName);
    if (impl.library == nullptr) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML library could not be loaded on this host"};
    }

    NvmlApi& api = impl.api;
    api.init = resolve<decltype(api.init)>(impl.library, "nvmlInit_v2");
    if (api.init == nullptr) {
        api.init = resolve<decltype(api.init)>(impl.library, "nvmlInit");
    }
    api.shutdown = resolve<decltype(api.shutdown)>(impl.library, "nvmlShutdown");
    api.error_string = resolve<decltype(api.error_string)>(impl.library, "nvmlErrorString");
    api.system_get_driver_version =
        resolve<decltype(api.system_get_driver_version)>(impl.library,
                                                         "nvmlSystemGetDriverVersion");
    api.system_get_nvml_version =
        resolve<decltype(api.system_get_nvml_version)>(impl.library, "nvmlSystemGetNVMLVersion");
    api.device_get_count =
        resolve<decltype(api.device_get_count)>(impl.library, "nvmlDeviceGetCount_v2");
    if (api.device_get_count == nullptr) {
        api.device_get_count =
            resolve<decltype(api.device_get_count)>(impl.library, "nvmlDeviceGetCount");
    }
    api.device_get_handle_by_index = resolve<decltype(api.device_get_handle_by_index)>(
        impl.library, "nvmlDeviceGetHandleByIndex_v2");
    if (api.device_get_handle_by_index == nullptr) {
        api.device_get_handle_by_index = resolve<decltype(api.device_get_handle_by_index)>(
            impl.library, "nvmlDeviceGetHandleByIndex");
    }
    api.device_get_uuid = resolve<decltype(api.device_get_uuid)>(impl.library, "nvmlDeviceGetUUID");
    api.device_get_name = resolve<decltype(api.device_get_name)>(impl.library, "nvmlDeviceGetName");
    api.device_get_pci_info = resolve<decltype(api.device_get_pci_info)>(
        impl.library, "nvmlDeviceGetPciInfo_v3");
    if (api.device_get_pci_info == nullptr) {
        api.device_get_pci_info = resolve<decltype(api.device_get_pci_info)>(
            impl.library, "nvmlDeviceGetPciInfo");
    }
    api.device_get_temperature = resolve<decltype(api.device_get_temperature)>(
        impl.library, "nvmlDeviceGetTemperature");
    api.device_get_temperature_threshold = resolve<decltype(api.device_get_temperature_threshold)>(
        impl.library, "nvmlDeviceGetTemperatureThreshold");
    api.device_get_current_clocks_throttle_reasons =
        resolve<decltype(api.device_get_current_clocks_throttle_reasons)>(
            impl.library, "nvmlDeviceGetCurrentClocksThrottleReasons");
    api.device_get_clock_info = resolve<decltype(api.device_get_clock_info)>(
        impl.library, "nvmlDeviceGetClockInfo");
    api.device_get_max_clock_info = resolve<decltype(api.device_get_max_clock_info)>(
        impl.library, "nvmlDeviceGetMaxClockInfo");
    api.device_get_num_fans =
        resolve<decltype(api.device_get_num_fans)>(impl.library, "nvmlDeviceGetNumFans");
    api.device_get_fan_speed =
        resolve<decltype(api.device_get_fan_speed)>(impl.library, "nvmlDeviceGetFanSpeed");

    if (!all_present(api)) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML library is missing required entry points"};
    }

    const nvmlReturn_t init_code = api.init();
    if (init_code != kSuccess) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML initialisation failed on this host"};
    }
    impl.initialised = true;

    unsigned int count = 0;
    if (api.device_get_count(&count) != kSuccess) {
        return ThermalError{ThermalErrorCode::BACKEND_FAILURE, "NVML device enumeration failed"};
    }

    for (unsigned int index = 0; index < count; ++index) {
        nvmlDevice_st handle = nullptr;
        if (api.device_get_handle_by_index(index, &handle) != kSuccess || handle == nullptr) {
            continue;
        }
        std::array<char, 96> uuid{};
        std::array<char, 96> name{};
        NvmlPciInfo pci{};
        api.device_get_uuid(handle, uuid.data(), static_cast<unsigned int>(uuid.size()));
        api.device_get_name(handle, name.data(), static_cast<unsigned int>(name.size()));
        if (api.device_get_pci_info != nullptr) {
            api.device_get_pci_info(handle, &pci);
        }
        impl.handles.push_back(handle);
        impl.uuids.emplace_back(uuid.data());
        impl.names.emplace_back(name.data());
        impl.pci_ids.emplace_back(pci.bus_id);
    }

    // Sanity probe: the handle list must be usable even before capabilities
    // are resolved.
    if (!impl.handles.empty()) {
        unsigned int probe = 0;
        if (api.device_get_temperature(impl.handles.front(), kTemperatureGpu, &probe) != kSuccess) {
            // Enumeration succeeded but reads fail: report honestly rather
            // than degrading silently.
            impl.capabilities_resolved = false;
        }
    }
    return backend;
#endif
}

std::string NvmlThermalBackend::name() const { return "nvml"; }

std::size_t NvmlThermalBackend::device_count() const noexcept { return impl_->handles.size(); }

std::string NvmlThermalBackend::driver_version() const {
    std::array<char, 80> buffer{};
    if (impl_->api.system_get_driver_version != nullptr &&
        impl_->api.system_get_driver_version(buffer.data(),
                                             static_cast<unsigned int>(buffer.size())) == kSuccess) {
        return std::string(buffer.data());
    }
    return {};
}

std::string NvmlThermalBackend::nvml_version() const {
    std::array<char, 80> buffer{};
    if (impl_->api.system_get_nvml_version != nullptr &&
        impl_->api.system_get_nvml_version(buffer.data(),
                                           static_cast<unsigned int>(buffer.size())) == kSuccess) {
        return std::string(buffer.data());
    }
    return {};
}

BackendCapabilities NvmlThermalBackend::capabilities() {
    BackendCapabilities out;
    out.backend_name = name();
    out.backend_version = nvml_version();
    out.provenance = Provenance::REAL;

    CapabilitySet set;
    const nvmlDevice_st probe = impl_->handles.empty() ? nullptr : impl_->handles.front();

    const auto classify = [](nvmlReturn_t code) {
        if (code == kSuccess) {
            return CapabilityState::SUPPORTED_REAL;
        }
        if (code == kErrorNotSupported) {
            return CapabilityState::UNSUPPORTED;
        }
        return CapabilityState::UNKNOWN;
    };

    set.set(ThermalCapability::DEVICE_IDENTITY,
            impl_->handles.empty() ? CapabilityState::UNSUPPORTED : CapabilityState::SUPPORTED_REAL);
    set.set(ThermalCapability::THERMAL_DOMAIN_METADATA, CapabilityState::UNSUPPORTED);
    set.set(ThermalCapability::NODE_THERMAL_METADATA, CapabilityState::UNSUPPORTED);
    set.set(ThermalCapability::RACK_THERMAL_METADATA, CapabilityState::UNSUPPORTED);
    set.set(ThermalCapability::COOLING_TELEMETRY, CapabilityState::UNSUPPORTED);
    set.set(ThermalCapability::ACTUAL_THROTTLE_ENFORCEMENT, CapabilityState::UNSUPPORTED);
    set.set(ThermalCapability::CLOCK_REDUCTION_ENFORCEMENT, CapabilityState::UNSUPPORTED);
    set.set(ThermalCapability::NODE_THERMAL_METADATA, CapabilityState::UNSUPPORTED);

    if (probe == nullptr) {
        for (std::size_t i = 0; i < kThermalCapabilityCount; ++i) {
            const auto capability = static_cast<ThermalCapability>(i);
            if (set.get(capability) == CapabilityState::UNKNOWN) {
                set.set(capability, CapabilityState::UNSUPPORTED);
            }
        }
        out.capabilities = std::move(set);
        return out;
    }

    unsigned int scratch = 0;
    set.set(ThermalCapability::TEMPERATURE,
            classify(impl_->api.device_get_temperature(probe, kTemperatureGpu, &scratch)));

    if (impl_->api.device_get_temperature_threshold != nullptr) {
        set.set(ThermalCapability::TEMPERATURE_LIMIT,
                classify(impl_->api.device_get_temperature_threshold(probe, kThresholdGpuMax,
                                                                     &scratch)));
        set.set(ThermalCapability::TEMPERATURE_THRESHOLD_SHUTDOWN,
                classify(impl_->api.device_get_temperature_threshold(probe, kThresholdShutdown,
                                                                     &scratch)));
        set.set(ThermalCapability::TEMPERATURE_THRESHOLD_SLOWDOWN,
                classify(impl_->api.device_get_temperature_threshold(probe, kThresholdSlowdown,
                                                                     &scratch)));
    } else {
        set.set(ThermalCapability::TEMPERATURE_LIMIT, CapabilityState::UNSUPPORTED);
        set.set(ThermalCapability::TEMPERATURE_THRESHOLD_SHUTDOWN, CapabilityState::UNSUPPORTED);
        set.set(ThermalCapability::TEMPERATURE_THRESHOLD_SLOWDOWN, CapabilityState::UNSUPPORTED);
    }

    if (impl_->api.device_get_current_clocks_throttle_reasons != nullptr) {
        unsigned long long reasons = 0;
        set.set(ThermalCapability::THROTTLE_REASONS,
                classify(impl_->api.device_get_current_clocks_throttle_reasons(probe, &reasons)));
    } else {
        set.set(ThermalCapability::THROTTLE_REASONS, CapabilityState::UNSUPPORTED);
    }

    if (impl_->api.device_get_clock_info != nullptr) {
        set.set(ThermalCapability::CURRENT_CLOCK,
                classify(impl_->api.device_get_clock_info(probe, kClockGraphics, &scratch)));
    } else {
        set.set(ThermalCapability::CURRENT_CLOCK, CapabilityState::UNSUPPORTED);
    }
    if (impl_->api.device_get_max_clock_info != nullptr) {
        set.set(ThermalCapability::MAX_CLOCK,
                classify(impl_->api.device_get_max_clock_info(probe, kClockGraphics, &scratch)));
    } else {
        set.set(ThermalCapability::MAX_CLOCK, CapabilityState::UNSUPPORTED);
    }

    if (impl_->api.device_get_num_fans != nullptr && impl_->api.device_get_fan_speed != nullptr) {
        unsigned int fans = 0;
        const nvmlReturn_t fan_code = impl_->api.device_get_num_fans(probe, &fans);
        if (fan_code == kSuccess && fans > 0) {
            set.set(ThermalCapability::FAN_TELEMETRY,
                    classify(impl_->api.device_get_fan_speed(probe, &scratch)));
        } else {
            set.set(ThermalCapability::FAN_TELEMETRY, classify(fan_code));
        }
    } else {
        set.set(ThermalCapability::FAN_TELEMETRY, CapabilityState::UNSUPPORTED);
    }

    for (std::size_t i = 0; i < kThermalCapabilityCount; ++i) {
        const auto capability = static_cast<ThermalCapability>(i);
        if (set.get(capability) == CapabilityState::UNKNOWN) {
            set.set(capability, CapabilityState::UNKNOWN);
        }
    }

    out.capabilities = std::move(set);
    impl_->capabilities = out;
    impl_->capabilities_resolved = true;
    return out;
}

Result<std::vector<DeviceIdentity>> NvmlThermalBackend::query_device_identity() {
    std::vector<DeviceIdentity> out;
    out.reserve(impl_->handles.size());
    for (std::size_t i = 0; i < impl_->handles.size(); ++i) {
        DeviceIdentity identity;
        // NVML index zero is valid, so Thermal Governor device ids are
        // one-based to keep "invalid" unambiguous.
        identity.device = DeviceId{StrongId<DeviceIdTag>{static_cast<std::uint64_t>(i + 1)}};
        identity.uuid = impl_->uuids[i];
        identity.pci_bus_id = impl_->pci_ids[i];
        identity.name = impl_->names[i];
        identity.index = static_cast<std::uint32_t>(i);
        out.push_back(std::move(identity));
    }
    return out;
}

Result<TemperatureReading> NvmlThermalBackend::query_temperature(DeviceId device) {
    const nvmlDevice_st handle = impl_->handle_for(device);
    if (handle == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "NVML device index is not present"};
    }
    unsigned int value = 0;
    const nvmlReturn_t code = impl_->api.device_get_temperature(handle, kTemperatureGpu, &value);
    if (code == kErrorNotSupported) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML reports no GPU temperature sensor"};
    }
    if (code != kSuccess) {
        return impl_->error_from(code, "NVML temperature query failed");
    }
    auto parsed = DegreesCelsius::try_from(static_cast<double>(value));
    if (!parsed.has_value()) {
        return ThermalError{ThermalErrorCode::TEMPERATURE_INVALID,
                            "NVML reported an implausible temperature"};
    }
    TemperatureReading reading;
    reading.temperature = *parsed;
    reading.source = MeasurementSource::NVML_GPU_TEMPERATURE;
    return reading;
}

Result<DegreesCelsius> NvmlThermalBackend::query_temperature_limit(DeviceId device) {
    const nvmlDevice_st handle = impl_->handle_for(device);
    if (handle == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "NVML device index is not present"};
    }
    if (impl_->api.device_get_temperature_threshold == nullptr) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML temperature thresholds are unavailable"};
    }
    unsigned int value = 0;
    const nvmlReturn_t code =
        impl_->api.device_get_temperature_threshold(handle, kThresholdGpuMax, &value);
    if (code == kErrorNotSupported) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML reports no maximum operating temperature"};
    }
    if (code != kSuccess) {
        return impl_->error_from(code, "NVML temperature limit query failed");
    }
    auto parsed = DegreesCelsius::try_from(static_cast<double>(value));
    if (!parsed.has_value()) {
        return ThermalError{ThermalErrorCode::TEMPERATURE_INVALID,
                            "NVML reported an implausible temperature limit"};
    }
    return *parsed;
}

Result<DegreesCelsius> NvmlThermalBackend::query_shutdown_limit(DeviceId device) {
    const nvmlDevice_st handle = impl_->handle_for(device);
    if (handle == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "NVML device index is not present"};
    }
    if (impl_->api.device_get_temperature_threshold == nullptr) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML temperature thresholds are unavailable"};
    }
    unsigned int value = 0;
    const nvmlReturn_t code =
        impl_->api.device_get_temperature_threshold(handle, kThresholdShutdown, &value);
    if (code == kErrorNotSupported) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML reports no shutdown temperature"};
    }
    if (code != kSuccess) {
        return impl_->error_from(code, "NVML shutdown limit query failed");
    }
    auto parsed = DegreesCelsius::try_from(static_cast<double>(value));
    if (!parsed.has_value()) {
        return ThermalError{ThermalErrorCode::TEMPERATURE_INVALID,
                            "NVML reported an implausible shutdown limit"};
    }
    return *parsed;
}

Result<ThrottleObservation> NvmlThermalBackend::query_throttle_reasons(DeviceId device) {
    const nvmlDevice_st handle = impl_->handle_for(device);
    if (handle == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "NVML device index is not present"};
    }
    if (impl_->api.device_get_current_clocks_throttle_reasons == nullptr) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML throttle reason query is unavailable"};
    }
    unsigned long long reasons = 0;
    const nvmlReturn_t code =
        impl_->api.device_get_current_clocks_throttle_reasons(handle, &reasons);
    if (code == kErrorNotSupported) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML reports no throttle reason source"};
    }
    if (code != kSuccess) {
        return impl_->error_from(code, "NVML throttle reason query failed");
    }
    return ThrottleObservation::from_raw(static_cast<std::uint64_t>(reasons));
}

Result<MegaHertz> NvmlThermalBackend::query_current_clock(DeviceId device) {
    const nvmlDevice_st handle = impl_->handle_for(device);
    if (handle == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "NVML device index is not present"};
    }
    if (impl_->api.device_get_clock_info == nullptr) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML clock query is unavailable"};
    }
    unsigned int value = 0;
    const nvmlReturn_t code = impl_->api.device_get_clock_info(handle, kClockGraphics, &value);
    if (code == kErrorNotSupported) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML reports no graphics clock"};
    }
    if (code != kSuccess) {
        return impl_->error_from(code, "NVML current clock query failed");
    }
    return MegaHertz{value};
}

Result<MegaHertz> NvmlThermalBackend::query_max_clock(DeviceId device) {
    const nvmlDevice_st handle = impl_->handle_for(device);
    if (handle == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "NVML device index is not present"};
    }
    if (impl_->api.device_get_max_clock_info == nullptr) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML maximum clock query is unavailable"};
    }
    unsigned int value = 0;
    const nvmlReturn_t code = impl_->api.device_get_max_clock_info(handle, kClockGraphics, &value);
    if (code == kErrorNotSupported) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML reports no maximum graphics clock"};
    }
    if (code != kSuccess) {
        return impl_->error_from(code, "NVML maximum clock query failed");
    }
    return MegaHertz{value};
}

Result<FanEvidence> NvmlThermalBackend::query_fan_state(DeviceId device) {
    const nvmlDevice_st handle = impl_->handle_for(device);
    if (handle == nullptr) {
        return ThermalError{ThermalErrorCode::UNKNOWN_DEVICE, "NVML device index is not present"};
    }
    if (impl_->api.device_get_num_fans == nullptr || impl_->api.device_get_fan_speed == nullptr) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML fan telemetry is unavailable"};
    }
    unsigned int fans = 0;
    nvmlReturn_t code = impl_->api.device_get_num_fans(handle, &fans);
    if (code != kSuccess || fans == 0) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML reports no controllable fans on this device"};
    }
    unsigned int speed = 0;
    code = impl_->api.device_get_fan_speed(handle, &speed);
    if (code == kErrorNotSupported) {
        return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                            "NVML fan speed is not reported"};
    }
    if (code != kSuccess) {
        return impl_->error_from(code, "NVML fan speed query failed");
    }
    FanEvidence evidence;
    evidence.fan_count = fans;
    evidence.speed_percent = static_cast<double>(speed);
    evidence.provenance = Provenance::REAL;
    return evidence;
}

Result<CoolingEvidence> NvmlThermalBackend::query_cooling_state(DeviceId) {
    // NVML exposes no coolant inlet/outlet or airflow telemetry. Thermal
    // Governor does not fabricate it.
    return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                        "NVML exposes no cooling-loop telemetry"};
}

Result<DegreesCelsius> NvmlThermalBackend::query_node_temperature(NodeId) {
    return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                        "NVML exposes no node temperature sensor"};
}

Result<DegreesCelsius> NvmlThermalBackend::query_rack_temperature(RackId) {
    return ThermalError{ThermalErrorCode::CAPABILITY_UNSUPPORTED,
                        "NVML exposes no rack temperature sensor"};
}

}  // namespace thermal_governor
