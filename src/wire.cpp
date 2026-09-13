// Thermal Governor — wire codec implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/wire.hpp"

#include "binary.hpp"
#include "thermal_governor/capability.hpp"
#include "thermal_governor/decision.hpp"
#include "thermal_governor/thermal_state.hpp"

namespace thermal_governor::wire {
namespace {

using detail::BinaryReader;
using detail::BinaryWriter;

constexpr std::size_t kMaxCollection = 1U << 20;

[[nodiscard]] ThermalError corrupt(const char* what) {
    return ThermalError{ThermalErrorCode::FRAME_CORRUPT, what};
}

template <class T>
[[nodiscard]] Result<T> decode_with(const std::uint8_t* data, std::size_t length,
                                    const WireLimits& limits, T (*fn)(BinaryReader&,
                                                                      const WireLimits&)) {
    if (data == nullptr && length != 0) {
        return ThermalError{ThermalErrorCode::INVALID_ARGUMENT, "null payload pointer"};
    }
    BinaryReader reader(data, length);
    T value = fn(reader, limits);
    if (!reader.ok() || !reader.exhausted()) {
        return corrupt("wire payload is malformed or carries trailing bytes");
    }
    return value;
}

void write_capability_set(BinaryWriter& writer, const CapabilitySet& set) {
    for (std::size_t i = 0; i < kThermalCapabilityCount; ++i) {
        const auto capability = static_cast<ThermalCapability>(i);
        writer.u8(static_cast<std::uint8_t>(set.get(capability)));
        writer.text(set.detail(capability));
    }
    writer.u64(set.generation.value());
}

[[nodiscard]] bool read_capability_set(BinaryReader& reader, CapabilitySet& set,
                                       const WireLimits& limits) {
    for (std::size_t i = 0; i < kThermalCapabilityCount; ++i) {
        const std::uint8_t state = reader.u8();
        const std::string detail = reader.text(limits.max_string_bytes);
        if (!reader.ok() || state > static_cast<std::uint8_t>(CapabilityState::UNSUPPORTED)) {
            return false;
        }
        set.set(static_cast<ThermalCapability>(i), static_cast<CapabilityState>(state), detail);
    }
    set.generation = CapabilityGeneration{StrongId<CapabilityGenerationTag>{reader.u64()}};
    return reader.ok();
}

void write_subject(BinaryWriter& writer, const SubjectRef& subject) {
    writer.u8(static_cast<std::uint8_t>(subject.kind));
    writer.u64(subject.device.value());
    writer.u64(subject.device_generation.value());
    writer.u64(subject.node.value());
    writer.u64(subject.node_generation.value());
    writer.u64(subject.rack.value());
    writer.u64(subject.rack_generation.value());
    writer.u64(subject.cooling_zone.value());
    writer.u64(subject.chassis.value());
    writer.u64(subject.domain.value());
    writer.u64(subject.domain_generation.value());
}

[[nodiscard]] bool read_subject(BinaryReader& reader, SubjectRef& subject) {
    const std::uint8_t kind = reader.u8();
    if (!reader.ok() || kind > static_cast<std::uint8_t>(SubjectKind::THERMAL_DOMAIN)) {
        return false;
    }
    subject.kind = static_cast<SubjectKind>(kind);
    subject.device = DeviceId{StrongId<DeviceIdTag>{reader.u64()}};
    subject.device_generation = DeviceGeneration{StrongId<DeviceGenerationTag>{reader.u64()}};
    subject.node = NodeId{StrongId<NodeIdTag>{reader.u64()}};
    subject.node_generation = NodeGeneration{StrongId<NodeGenerationTag>{reader.u64()}};
    subject.rack = RackId{StrongId<RackIdTag>{reader.u64()}};
    subject.rack_generation = RackGeneration{StrongId<RackGenerationTag>{reader.u64()}};
    subject.cooling_zone = CoolingZoneId{StrongId<CoolingZoneIdTag>{reader.u64()}};
    subject.chassis = ChassisId{StrongId<ChassisIdTag>{reader.u64()}};
    subject.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{reader.u64()}};
    subject.domain_generation =
        ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{reader.u64()}};
    return reader.ok();
}

[[nodiscard]] bool read_temperature(BinaryReader& reader, DegreesCelsius& out) {
    const double raw = reader.real();
    auto parsed = DegreesCelsius::try_from(raw);
    if (!parsed.has_value()) {
        return false;
    }
    out = *parsed;
    return true;
}

[[nodiscard]] bool read_delta(BinaryReader& reader, TemperatureDelta& out) {
    const double raw = reader.real();
    auto parsed = TemperatureDelta::try_from(raw);
    if (!parsed.has_value()) {
        return false;
    }
    out = *parsed;
    return true;
}

// --- Registration ---------------------------------------------------------

void write_registration(BinaryWriter& writer, const WorkerRegistration& registration,
                        const WireLimits& limits) {
    writer.u64(registration.worker.value());
    writer.u64(registration.boot.value());
    writer.text(registration.label);
    writer.u64(registration.coordinator_epoch.value());
    writer.u32(static_cast<std::uint32_t>(registration.devices.size()));
    for (const auto& device : registration.devices) {
        writer.u64(device.device.value());
        writer.u64(device.generation.value());
        writer.u64(device.node.value());
        writer.u64(device.node_generation.value());
        writer.u64(device.rack.value());
        writer.u64(device.rack_generation.value());
        writer.text(device.label.text());
        writer.u8(static_cast<std::uint8_t>(device.provenance));
        write_capability_set(writer, device.capabilities);
    }
    (void)limits;
}

[[nodiscard]] WorkerRegistration read_registration(BinaryReader& reader,
                                                   const WireLimits& limits) {
    WorkerRegistration registration;
    registration.worker = WorkerId{StrongId<WorkerIdTag>{reader.u64()}};
    registration.boot = WorkerBootId{StrongId<WorkerBootIdTag>{reader.u64()}};
    registration.label = reader.text(limits.max_string_bytes);
    registration.coordinator_epoch =
        CoordinatorEpoch{StrongId<CoordinatorEpochTag>{reader.u64()}};
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > limits.max_devices) {
        reader.fail();
        return registration;
    }
    registration.devices.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        DeviceRegistration device;
        device.device = DeviceId{StrongId<DeviceIdTag>{reader.u64()}};
        device.generation = DeviceGeneration{StrongId<DeviceGenerationTag>{reader.u64()}};
        device.node = NodeId{StrongId<NodeIdTag>{reader.u64()}};
        device.node_generation = NodeGeneration{StrongId<NodeGenerationTag>{reader.u64()}};
        device.rack = RackId{StrongId<RackIdTag>{reader.u64()}};
        device.rack_generation = RackGeneration{StrongId<RackGenerationTag>{reader.u64()}};
        device.label = Label{reader.text(limits.max_string_bytes)};
        const std::uint8_t provenance = reader.u8();
        if (!reader.ok() || provenance > static_cast<std::uint8_t>(Provenance::UNSUPPORTED)) {
            reader.fail();
            return registration;
        }
        device.provenance = static_cast<Provenance>(provenance);
        if (!read_capability_set(reader, device.capabilities, limits)) {
            reader.fail();
            return registration;
        }
        registration.devices.push_back(std::move(device));
    }
    return registration;
}

// --- Evidence -------------------------------------------------------------

void write_evidence(BinaryWriter& writer, const ThermalEvidence& evidence) {
    writer.u64(evidence.evidence_id.value());
    write_subject(writer, evidence.subject);
    writer.u64(evidence.domain.value());
    writer.u64(evidence.domain_generation.value());
    writer.real(evidence.temperature.value());
    writer.u8(static_cast<std::uint8_t>(evidence.source));
    writer.u8(static_cast<std::uint8_t>(evidence.provenance));
    writer.u64(evidence.measurement_sequence);
    writer.u64(evidence.telemetry_generation.value());
    writer.u64(evidence.worker.value());
    writer.u64(evidence.worker_boot.value());
    writer.u64(evidence.coordinator_epoch.value());
    writer.i64(evidence.measured_at.time_since_epoch().count());
    writer.i64(evidence.measured_wall_at.time_since_epoch().count());
    writer.u64(evidence.capability_generation.value());
    writer.u64(evidence.topology_generation.value());
    writer.u8(static_cast<std::uint8_t>(evidence.integrity));
    writer.optional<ThrottleObservation>(evidence.throttle,
                                         [&writer](const ThrottleObservation& observation) {
                                             writer.u64(observation.raw_reasons);
                                             writer.u8(static_cast<std::uint8_t>(
                                                 observation.classification));
                                         });
    writer.optional<DegreesCelsius>(evidence.temperature_limit,
                                    [&writer](DegreesCelsius value) {
                                        writer.real(value.value());
                                    });
    writer.optional<DegreesCelsius>(evidence.shutdown_limit,
                                    [&writer](DegreesCelsius value) {
                                        writer.real(value.value());
                                    });
    writer.optional<MegaHertz>(evidence.current_clock,
                               [&writer](MegaHertz value) { writer.u32(value.value()); });
    writer.optional<MegaHertz>(evidence.max_clock,
                               [&writer](MegaHertz value) { writer.u32(value.value()); });
    writer.optional<CoolingEvidence>(evidence.cooling,
                                     [&writer](const CoolingEvidence& cooling) {
                                         writer.u64(cooling.zone.value());
                                         writer.optional<DegreesCelsius>(
                                             cooling.air_inlet, [&writer](DegreesCelsius v) {
                                                 writer.real(v.value());
                                             });
                                         writer.optional<DegreesCelsius>(
                                             cooling.air_outlet, [&writer](DegreesCelsius v) {
                                                 writer.real(v.value());
                                             });
                                         writer.optional<DegreesCelsius>(
                                             cooling.liquid_inlet, [&writer](DegreesCelsius v) {
                                                 writer.real(v.value());
                                             });
                                         writer.optional<DegreesCelsius>(
                                             cooling.liquid_outlet, [&writer](DegreesCelsius v) {
                                                 writer.real(v.value());
                                             });
                                         writer.optional<double>(
                                             cooling.coolant_flow_litres_per_minute,
                                             [&writer](double v) { writer.real(v); });
                                         writer.u8(static_cast<std::uint8_t>(
                                             cooling.provenance));
                                     });
    writer.optional<FanEvidence>(evidence.fan, [&writer](const FanEvidence& fan) {
        writer.u32(fan.fan_count);
        writer.optional<double>(fan.speed_percent,
                                [&writer](double v) { writer.real(v); });
        writer.optional<std::uint32_t>(fan.speed_rpm,
                                       [&writer](std::uint32_t v) { writer.u32(v); });
        writer.u8(static_cast<std::uint8_t>(fan.provenance));
    });
    writer.optional<double>(evidence.confidence, [&writer](double v) { writer.real(v); });
}

[[nodiscard]] ThermalEvidence read_evidence(BinaryReader& reader, const WireLimits& limits) {
    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{reader.u64()}};
    if (!read_subject(reader, evidence.subject)) {
        return evidence;
    }
    evidence.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{reader.u64()}};
    evidence.domain_generation =
        ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{reader.u64()}};
    if (!read_temperature(reader, evidence.temperature)) {
        reader.fail();
        return evidence;
    }
    const std::uint8_t source = reader.u8();
    const std::uint8_t provenance = reader.u8();
    if (!reader.ok() || source > static_cast<std::uint8_t>(MeasurementSource::SYNTHETIC_MODEL) ||
        provenance > static_cast<std::uint8_t>(Provenance::UNSUPPORTED)) {
        reader.fail();
        return evidence;
    }
    evidence.source = static_cast<MeasurementSource>(source);
    evidence.provenance = static_cast<Provenance>(provenance);
    evidence.measurement_sequence = reader.u64();
    evidence.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{reader.u64()}};
    evidence.worker = WorkerId{StrongId<WorkerIdTag>{reader.u64()}};
    evidence.worker_boot = WorkerBootId{StrongId<WorkerBootIdTag>{reader.u64()}};
    evidence.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{reader.u64()}};
    evidence.measured_at =
        SteadyTimePoint{SteadyTimePoint::duration{reader.i64()}};
    evidence.measured_wall_at =
        WallTimePoint{WallTimePoint::duration{reader.i64()}};
    evidence.capability_generation = CapabilityGeneration{StrongId<CapabilityGenerationTag>{reader.u64()}};
    evidence.topology_generation = TopologyGeneration{StrongId<TopologyGenerationTag>{reader.u64()}};
    const std::uint8_t integrity = reader.u8();
    if (!reader.ok() || integrity > static_cast<std::uint8_t>(IntegrityStatus::FAILED)) {
        reader.fail();
        return evidence;
    }
    evidence.integrity = static_cast<IntegrityStatus>(integrity);

    if (reader.presence()) {
        ThrottleObservation observation;
        observation.raw_reasons = reader.u64();
        const std::uint8_t classification = reader.u8();
        if (!reader.ok() ||
            classification > static_cast<std::uint8_t>(ThrottleClass::THROTTLE_UNSUPPORTED)) {
            reader.fail();
            return evidence;
        }
        observation.classification = static_cast<ThrottleClass>(classification);
        evidence.throttle = observation;
    }
    if (reader.presence()) {
        DegreesCelsius value{};
        if (!read_temperature(reader, value)) {
            reader.fail();
            return evidence;
        }
        evidence.temperature_limit = value;
    }
    if (reader.presence()) {
        DegreesCelsius value{};
        if (!read_temperature(reader, value)) {
            reader.fail();
            return evidence;
        }
        evidence.shutdown_limit = value;
    }
    if (reader.presence()) {
        evidence.current_clock = MegaHertz{reader.u32()};
    }
    if (reader.presence()) {
        evidence.max_clock = MegaHertz{reader.u32()};
    }
    if (reader.presence()) {
        CoolingEvidence cooling;
        cooling.zone = CoolingZoneId{StrongId<CoolingZoneIdTag>{reader.u64()}};
        DegreesCelsius value{};
        if (reader.presence()) {
            if (!read_temperature(reader, value)) {
                reader.fail();
                return evidence;
            }
            cooling.air_inlet = value;
        }
        if (reader.presence()) {
            if (!read_temperature(reader, value)) {
                reader.fail();
                return evidence;
            }
            cooling.air_outlet = value;
        }
        if (reader.presence()) {
            if (!read_temperature(reader, value)) {
                reader.fail();
                return evidence;
            }
            cooling.liquid_inlet = value;
        }
        if (reader.presence()) {
            if (!read_temperature(reader, value)) {
                reader.fail();
                return evidence;
            }
            cooling.liquid_outlet = value;
        }
        if (reader.presence()) {
            cooling.coolant_flow_litres_per_minute = reader.real();
        }
        const std::uint8_t cooling_provenance = reader.u8();
        if (!reader.ok() ||
            cooling_provenance > static_cast<std::uint8_t>(Provenance::UNSUPPORTED)) {
            reader.fail();
            return evidence;
        }
        cooling.provenance = static_cast<Provenance>(cooling_provenance);
        evidence.cooling = cooling;
    }
    if (reader.presence()) {
        FanEvidence fan;
        fan.fan_count = reader.u32();
        if (reader.presence()) {
            fan.speed_percent = reader.real();
        }
        if (reader.presence()) {
            fan.speed_rpm = reader.u32();
        }
        const std::uint8_t fan_provenance = reader.u8();
        if (!reader.ok() || fan_provenance > static_cast<std::uint8_t>(Provenance::UNSUPPORTED)) {
            reader.fail();
            return evidence;
        }
        fan.provenance = static_cast<Provenance>(fan_provenance);
        evidence.fan = fan;
    }
    if (reader.presence()) {
        evidence.confidence = reader.real();
    }
    (void)limits;
    return evidence;
}

// --- Actions --------------------------------------------------------------

void write_action(BinaryWriter& writer, const ThermalAction& action) {
    writer.u64(action.id.value());
    writer.u64(action.generation.value());
    const ActionAuthority& authority = action.authority;
    writer.u64(authority.coordinator_epoch.value());
    writer.u64(authority.action_id.value());
    writer.u64(authority.action_generation.value());
    writer.u64(authority.domain.value());
    writer.u64(authority.domain_generation.value());
    writer.u64(authority.device.value());
    writer.u64(authority.device_generation.value());
    writer.u64(authority.policy_generation.value());
    writer.u64(authority.telemetry_generation.value());
    writer.u64(authority.worker.value());
    writer.u64(authority.worker_boot.value());
    writer.u64(authority.topology_generation.value());
    writer.u64(authority.capability_generation.value());
    writer.u8(static_cast<std::uint8_t>(action.intent));
    writer.u8(static_cast<std::uint8_t>(action.lifecycle));
    writer.u8(static_cast<std::uint8_t>(action.provenance));
    writer.optional<MegaHertz>(action.requested_clock_ceiling,
                               [&writer](MegaHertz v) { writer.u32(v.value()); });
    writer.optional<Percent>(action.requested_concurrency,
                             [&writer](Percent v) { writer.real(v.value()); });
    writer.optional<Percent>(action.requested_admission,
                             [&writer](Percent v) { writer.real(v.value()); });
    writer.text(action.detail);
}

[[nodiscard]] ThermalAction read_action(BinaryReader& reader, const WireLimits& limits) {
    ThermalAction action;
    action.id = ActionId{StrongId<ActionIdTag>{reader.u64()}};
    action.generation = ActionGeneration{StrongId<ActionGenerationTag>{reader.u64()}};
    action.authority.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{reader.u64()}};
    action.authority.action_id = ActionId{StrongId<ActionIdTag>{reader.u64()}};
    action.authority.action_generation = ActionGeneration{StrongId<ActionGenerationTag>{reader.u64()}};
    action.authority.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{reader.u64()}};
    action.authority.domain_generation =
        ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{reader.u64()}};
    action.authority.device = DeviceId{StrongId<DeviceIdTag>{reader.u64()}};
    action.authority.device_generation =
        DeviceGeneration{StrongId<DeviceGenerationTag>{reader.u64()}};
    action.authority.policy_generation =
        ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{reader.u64()}};
    action.authority.telemetry_generation =
        TelemetryGeneration{StrongId<TelemetryGenerationTag>{reader.u64()}};
    action.authority.worker = WorkerId{StrongId<WorkerIdTag>{reader.u64()}};
    action.authority.worker_boot = WorkerBootId{StrongId<WorkerBootIdTag>{reader.u64()}};
    action.authority.topology_generation =
        TopologyGeneration{StrongId<TopologyGenerationTag>{reader.u64()}};
    action.authority.capability_generation =
        CapabilityGeneration{StrongId<CapabilityGenerationTag>{reader.u64()}};

    const std::uint8_t intent = reader.u8();
    const std::uint8_t lifecycle = reader.u8();
    const std::uint8_t provenance = reader.u8();
    if (!reader.ok() || intent >= kMitigationIntentCount ||
        lifecycle >= kActionLifecycleCount ||
        provenance > static_cast<std::uint8_t>(Provenance::UNSUPPORTED)) {
        reader.fail();
        return action;
    }
    action.intent = static_cast<MitigationIntent>(intent);
    action.lifecycle = static_cast<ActionLifecycle>(lifecycle);
    action.provenance = static_cast<Provenance>(provenance);

    if (reader.presence()) {
        action.requested_clock_ceiling = MegaHertz{reader.u32()};
    }
    if (reader.presence()) {
        auto parsed = Percent::try_from(reader.real());
        if (!parsed.has_value()) {
            reader.fail();
            return action;
        }
        action.requested_concurrency = *parsed;
    }
    if (reader.presence()) {
        auto parsed = Percent::try_from(reader.real());
        if (!parsed.has_value()) {
            reader.fail();
            return action;
        }
        action.requested_admission = *parsed;
    }
    action.detail = reader.text(limits.max_string_bytes);
    return action;
}

// --- Ack / result ---------------------------------------------------------

void write_ack(BinaryWriter& writer, const ActionAck& ack) {
    writer.u64(ack.action_id.value());
    writer.u64(ack.action_generation.value());
    writer.u64(ack.coordinator_epoch.value());
    writer.u64(ack.worker_boot.value());
    writer.u64(ack.device_generation.value());
    writer.u64(ack.domain_generation.value());
    writer.u8(ack.accepted ? 1U : 0U);
    writer.text(ack.detail);
}

void write_result(BinaryWriter& writer, const ActionResult& result) {
    writer.u64(result.action_id.value());
    writer.u64(result.action_generation.value());
    writer.u64(result.coordinator_epoch.value());
    writer.u64(result.worker_boot.value());
    writer.u64(result.device_generation.value());
    writer.u64(result.domain_generation.value());
    writer.u64(result.policy_generation.value());
    writer.u64(result.telemetry_generation.value());
    writer.u8(result.backend_succeeded ? 1U : 0U);
    writer.text(result.detail);
}

}  // namespace

std::vector<std::uint8_t> encode_worker_registration(const WorkerRegistration& registration) {
    BinaryWriter writer;
    write_registration(writer, registration, WireLimits{});
    return writer.take();
}

Result<WorkerRegistration> decode_worker_registration(const std::uint8_t* data, std::size_t length,
                                                      const WireLimits& limits) {
    return decode_with<WorkerRegistration>(data, length, limits, read_registration);
}

std::vector<std::uint8_t> encode_evidence(const ThermalEvidence& evidence) {
    BinaryWriter writer;
    write_evidence(writer, evidence);
    return writer.take();
}

Result<ThermalEvidence> decode_evidence(const std::uint8_t* data, std::size_t length,
                                        const WireLimits& limits) {
    return decode_with<ThermalEvidence>(data, length, limits, read_evidence);
}

std::vector<std::uint8_t> encode_action(const ThermalAction& action) {
    BinaryWriter writer;
    write_action(writer, action);
    return writer.take();
}

Result<ThermalAction> decode_action(const std::uint8_t* data, std::size_t length,
                                    const WireLimits& limits) {
    return decode_with<ThermalAction>(data, length, limits, read_action);
}

std::vector<std::uint8_t> encode_action_ack(const ActionAck& ack) {
    BinaryWriter writer;
    write_ack(writer, ack);
    return writer.take();
}

Result<ActionAck> decode_action_ack(const std::uint8_t* data, std::size_t length,
                                    const WireLimits& limits) {
    ActionAck ack;
    BinaryReader reader(data, length);
    ack.action_id = ActionId{StrongId<ActionIdTag>{reader.u64()}};
    ack.action_generation = ActionGeneration{StrongId<ActionGenerationTag>{reader.u64()}};
    ack.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{reader.u64()}};
    ack.worker_boot = WorkerBootId{StrongId<WorkerBootIdTag>{reader.u64()}};
    ack.device_generation = DeviceGeneration{StrongId<DeviceGenerationTag>{reader.u64()}};
    ack.domain_generation =
        ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{reader.u64()}};
    const std::uint8_t accepted = reader.u8();
    ack.detail = reader.text(limits.max_string_bytes);
    if (!reader.ok() || !reader.exhausted() || accepted > 1U) {
        return corrupt("action acknowledgement payload is malformed");
    }
    ack.accepted = accepted != 0U;
    return ack;
}

std::vector<std::uint8_t> encode_action_result(const ActionResult& result) {
    BinaryWriter writer;
    write_result(writer, result);
    return writer.take();
}

Result<ActionResult> decode_action_result(const std::uint8_t* data, std::size_t length,
                                          const WireLimits& limits) {
    ActionResult result;
    BinaryReader reader(data, length);
    result.action_id = ActionId{StrongId<ActionIdTag>{reader.u64()}};
    result.action_generation = ActionGeneration{StrongId<ActionGenerationTag>{reader.u64()}};
    result.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{reader.u64()}};
    result.worker_boot = WorkerBootId{StrongId<WorkerBootIdTag>{reader.u64()}};
    result.device_generation = DeviceGeneration{StrongId<DeviceGenerationTag>{reader.u64()}};
    result.domain_generation =
        ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{reader.u64()}};
    result.policy_generation =
        ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{reader.u64()}};
    result.telemetry_generation =
        TelemetryGeneration{StrongId<TelemetryGenerationTag>{reader.u64()}};
    const std::uint8_t succeeded = reader.u8();
    result.detail = reader.text(limits.max_string_bytes);
    if (!reader.ok() || !reader.exhausted() || succeeded > 1U) {
        return corrupt("action result payload is malformed");
    }
    result.backend_succeeded = succeeded != 0U;
    return result;
}

EvaluationSummary summarise(const ThermalEvaluation& evaluation) {
    EvaluationSummary summary;
    summary.domain = evaluation.domain;
    summary.domain_generation = evaluation.domain_generation;
    summary.state = evaluation.state;
    summary.derating = evaluation.derating;
    summary.decision = evaluation.decision;
    summary.throttle_class = evaluation.throttle_class;
    summary.provenance = evaluation.provenance;
    summary.temperature = evaluation.headroom.current_temperature;
    summary.governing_limit = evaluation.headroom.governing_limit;
    summary.effective_headroom = evaluation.headroom.effective_headroom;
    summary.permitted_concurrency = evaluation.permitted_concurrency;
    summary.recovery_allowed = evaluation.recovery.allowed;
    summary.qualifying_samples = evaluation.recovery.qualifying_samples;
    summary.required_samples = evaluation.recovery.required_samples;
    summary.coordinator_epoch = evaluation.coordinator_epoch;
    summary.policy_generation = evaluation.policy_generation;
    summary.telemetry_generation = evaluation.telemetry_generation;
    for (const auto& record : evaluation.intents.records()) {
        summary.required_intents.push_back(record.intent);
    }
    for (const auto code : evaluation.reasons.codes()) {
        summary.reasons.push_back(code);
    }
    return summary;
}

std::vector<std::uint8_t> encode_evaluation_summary(const EvaluationSummary& summary) {
    BinaryWriter writer;
    writer.u64(summary.domain.value());
    writer.u64(summary.domain_generation.value());
    writer.u64(summary.device.value());
    writer.u64(summary.device_generation.value());
    writer.u8(static_cast<std::uint8_t>(summary.state));
    writer.u8(static_cast<std::uint8_t>(summary.derating));
    writer.u8(static_cast<std::uint8_t>(summary.decision));
    writer.u8(static_cast<std::uint8_t>(summary.throttle_class));
    writer.u8(static_cast<std::uint8_t>(summary.provenance));
    writer.real(summary.temperature.value());
    writer.real(summary.governing_limit.value());
    writer.real(summary.effective_headroom.value());
    writer.real(summary.permitted_concurrency.value());
    writer.u8(summary.recovery_allowed ? 1U : 0U);
    writer.u32(summary.qualifying_samples);
    writer.u32(summary.required_samples);
    writer.u64(summary.coordinator_epoch.value());
    writer.u64(summary.policy_generation.value());
    writer.u64(summary.telemetry_generation.value());
    writer.u32(static_cast<std::uint32_t>(summary.required_intents.size()));
    for (const auto intent : summary.required_intents) {
        writer.u8(static_cast<std::uint8_t>(intent));
    }
    writer.u32(static_cast<std::uint32_t>(summary.reasons.size()));
    for (const auto code : summary.reasons) {
        writer.u16(static_cast<std::uint16_t>(code));
    }
    return writer.take();
}

Result<EvaluationSummary> decode_evaluation_summary(const std::uint8_t* data, std::size_t length,
                                                    const WireLimits& limits) {
    EvaluationSummary summary;
    BinaryReader reader(data, length);
    summary.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{reader.u64()}};
    summary.domain_generation =
        ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{reader.u64()}};
    summary.device = DeviceId{StrongId<DeviceIdTag>{reader.u64()}};
    summary.device_generation = DeviceGeneration{StrongId<DeviceGenerationTag>{reader.u64()}};
    const std::uint8_t state = reader.u8();
    const std::uint8_t derating = reader.u8();
    const std::uint8_t decision = reader.u8();
    const std::uint8_t throttle = reader.u8();
    const std::uint8_t provenance = reader.u8();
    if (!reader.ok() || state >= kThermalStateCount || derating > 9U || decision > 6U ||
        throttle > static_cast<std::uint8_t>(ThrottleClass::THROTTLE_UNSUPPORTED) ||
        provenance > static_cast<std::uint8_t>(Provenance::UNSUPPORTED)) {
        return corrupt("evaluation summary carries an invalid enumeration");
    }
    summary.state = static_cast<ThermalState>(state);
    summary.derating = static_cast<DeratingLevel>(derating);
    summary.decision = static_cast<ThermalDecision>(decision);
    summary.throttle_class = static_cast<ThrottleClass>(throttle);
    summary.provenance = static_cast<Provenance>(provenance);
    if (!read_temperature(reader, summary.temperature) ||
        !read_temperature(reader, summary.governing_limit) ||
        !read_delta(reader, summary.effective_headroom)) {
        return corrupt("evaluation summary carries an invalid temperature");
    }
    summary.permitted_concurrency = Percent{reader.real()};
    const std::uint8_t recovery = reader.u8();
    summary.qualifying_samples = reader.u32();
    summary.required_samples = reader.u32();
    summary.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{reader.u64()}};
    summary.policy_generation =
        ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{reader.u64()}};
    summary.telemetry_generation =
        TelemetryGeneration{StrongId<TelemetryGenerationTag>{reader.u64()}};

    const std::uint32_t intent_count = reader.u32();
    if (!reader.ok() || intent_count > limits.max_intents) {
        return corrupt("evaluation summary intent count is out of range");
    }
    for (std::uint32_t i = 0; i < intent_count; ++i) {
        const std::uint8_t intent = reader.u8();
        if (!reader.ok() || intent >= kMitigationIntentCount) {
            return corrupt("evaluation summary carries an invalid intent");
        }
        summary.required_intents.push_back(static_cast<MitigationIntent>(intent));
    }
    const std::uint32_t reason_count = reader.u32();
    if (!reader.ok() || reason_count > limits.max_intents) {
        return corrupt("evaluation summary reason count is out of range");
    }
    for (std::uint32_t i = 0; i < reason_count; ++i) {
        summary.reasons.push_back(static_cast<ThermalReasonCode>(reader.u16()));
    }
    if (!reader.ok() || !reader.exhausted() || recovery > 1U) {
        return corrupt("evaluation summary payload is malformed");
    }
    summary.recovery_allowed = recovery != 0U;
    return summary;
}

std::vector<std::uint8_t> encode_status(const StatusPayload& status) {
    BinaryWriter writer;
    writer.u32(static_cast<std::uint32_t>(status.code));
    writer.text(status.detail);
    return writer.take();
}

Result<StatusPayload> decode_status(const std::uint8_t* data, std::size_t length,
                                    const WireLimits& limits) {
    StatusPayload status;
    BinaryReader reader(data, length);
    const std::uint32_t code = reader.u32();
    status.detail = reader.text(limits.max_string_bytes);
    if (!reader.ok() || !reader.exhausted()) {
        return corrupt("status payload is malformed");
    }
    status.code = static_cast<ThermalErrorCode>(code);
    return status;
}

std::vector<std::uint8_t> encode_command(const CommandRequest& request) {
    BinaryWriter writer;
    writer.text(request.verb);
    writer.u32(static_cast<std::uint32_t>(request.arguments.size()));
    for (const auto& argument : request.arguments) {
        writer.text(argument);
    }
    writer.text(request.payload);
    return writer.take();
}

Result<CommandRequest> decode_command(const std::uint8_t* data, std::size_t length,
                                      const WireLimits& limits) {
    CommandRequest request;
    BinaryReader reader(data, length);
    request.verb = reader.text(limits.max_string_bytes);
    const std::uint32_t count = reader.u32();
    if (!reader.ok() || count > limits.max_intents) {
        return corrupt("command argument count is out of range");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        request.arguments.push_back(reader.text(limits.max_string_bytes));
    }
    request.payload = reader.text(kMaxCollection);
    if (!reader.ok() || !reader.exhausted()) {
        return corrupt("command payload is malformed");
    }
    return request;
}

std::vector<std::uint8_t> encode_response(const CommandResponse& response) {
    BinaryWriter writer;
    writer.u32(static_cast<std::uint32_t>(response.status.code));
    writer.text(response.status.detail);
    writer.text(response.payload);
    return writer.take();
}

Result<CommandResponse> decode_response(const std::uint8_t* data, std::size_t length,
                                        const WireLimits& limits) {
    CommandResponse response;
    BinaryReader reader(data, length);
    const std::uint32_t code = reader.u32();
    response.status.detail = reader.text(limits.max_string_bytes);
    response.payload = reader.text(kMaxCollection);
    if (!reader.ok() || !reader.exhausted()) {
        return corrupt("response payload is malformed");
    }
    response.status.code = static_cast<ThermalErrorCode>(code);
    return response;
}

}  // namespace thermal_governor::wire
