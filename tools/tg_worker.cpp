// Thermal Governor — worker process.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "session.hpp"
#include "thermal_governor/thermal_governor.hpp"
#include "thermal_governor/wire.hpp"

namespace thermal_governor::tools {
namespace {

using wire::StatusPayload;
using wire::WorkerRegistration;

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::uint64_t worker_id = 1;
    std::uint64_t boot = 1;
    std::string label = "worker";
    std::uint64_t device_id = 1;
    std::uint64_t device_generation = 1;
    std::uint64_t node_id = 1;
    std::uint64_t node_generation = 1;
    std::uint64_t rack_id = 1;
    std::uint64_t rack_generation = 1;
    std::uint64_t domain_id = 1;
    std::uint64_t domain_generation = 1;
    std::vector<double> temperatures;
    std::uint32_t repeat = 1;
    std::string ready_file;
    bool hold = false;
    double mitigation_delta = -8.0;
    bool real_telemetry = false;
};

[[nodiscard]] bool option_value(int argc, char** argv, int& index, std::string& out) {
    if (index + 1 >= argc) {
        return false;
    }
    out = argv[++index];
    return true;
}

[[nodiscard]] bool parse_u64_argument(const std::string& text, std::uint64_t& out) {
    return parse_u64(text, out);
}

/// Build an observation for the current synthetic temperature.
[[nodiscard]] ThermalEvidence make_evidence(const Options& options, CoordinatorEpoch epoch,
                                            TelemetryGeneration telemetry,
                                            std::uint64_t sequence, DegreesCelsius temperature,
                                            std::optional<ThrottleObservation> throttle) {
    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{sequence}};
    evidence.subject = SubjectRef::for_device(
        DeviceId{StrongId<DeviceIdTag>{options.device_id}},
        DeviceGeneration{StrongId<DeviceGenerationTag>{options.device_generation}});
    evidence.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{options.domain_id}};
    evidence.domain_generation =
        ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{options.domain_generation}};
    evidence.temperature = temperature;
    evidence.source = options.real_telemetry ? MeasurementSource::NVML_GPU_TEMPERATURE
                                             : MeasurementSource::SYNTHETIC_MODEL;
    // The provenance always matches the real measurement source. A worker
    // never upgrades its own evidence classification.
    evidence.provenance = options.real_telemetry ? Provenance::REAL : Provenance::SYNTHETIC;
    evidence.measurement_sequence = sequence;
    evidence.telemetry_generation = telemetry;
    evidence.worker = WorkerId{StrongId<WorkerIdTag>{options.worker_id}};
    evidence.worker_boot = WorkerBootId{StrongId<WorkerBootIdTag>{options.boot}};
    evidence.coordinator_epoch = epoch;
    evidence.measured_at = std::chrono::steady_clock::now();
    evidence.measured_wall_at = std::chrono::system_clock::now();
    evidence.capability_generation = CapabilityGeneration{StrongId<CapabilityGenerationTag>{1}};
    evidence.topology_generation = TopologyGeneration{StrongId<TopologyGenerationTag>{1}};
    evidence.integrity = IntegrityStatus::OK;
    evidence.throttle = throttle;
    return evidence;
}

}  // namespace
}  // namespace thermal_governor::tools

int main(int argc, char** argv) {
    using namespace thermal_governor;
    using namespace thermal_governor::tools;

    Options options;
    std::string temperature_script;

    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        std::string value;
        if (argument == "--host") {
            if (!option_value(argc, argv, i, value)) return 2;
            options.host = value;
        } else if (argument == "--port") {
            if (!option_value(argc, argv, i, value)) return 2;
            std::uint64_t port = 0;
            if (!parse_u64(value, port) || port > 65535) return 2;
            options.port = static_cast<std::uint16_t>(port);
        } else if (argument == "--worker-id") {
            if (!option_value(argc, argv, i, value) || !parse_u64(value, options.worker_id)) return 2;
        } else if (argument == "--boot") {
            if (!option_value(argc, argv, i, value) || !parse_u64(value, options.boot)) return 2;
        } else if (argument == "--label") {
            if (!option_value(argc, argv, i, value)) return 2;
            options.label = value;
        } else if (argument == "--device") {
            if (!option_value(argc, argv, i, value) || !parse_u64(value, options.device_id)) return 2;
        } else if (argument == "--device-generation") {
            if (!option_value(argc, argv, i, value) ||
                !parse_u64(value, options.device_generation)) return 2;
        } else if (argument == "--node") {
            if (!option_value(argc, argv, i, value) || !parse_u64(value, options.node_id)) return 2;
        } else if (argument == "--node-generation") {
            if (!option_value(argc, argv, i, value) ||
                !parse_u64(value, options.node_generation)) return 2;
        } else if (argument == "--rack") {
            if (!option_value(argc, argv, i, value) || !parse_u64(value, options.rack_id)) return 2;
        } else if (argument == "--rack-generation") {
            if (!option_value(argc, argv, i, value) ||
                !parse_u64(value, options.rack_generation)) return 2;
        } else if (argument == "--domain") {
            if (!option_value(argc, argv, i, value) || !parse_u64(value, options.domain_id)) return 2;
        } else if (argument == "--domain-generation") {
            if (!option_value(argc, argv, i, value) ||
                !parse_u64(value, options.domain_generation)) return 2;
        } else if (argument == "--temps") {
            if (!option_value(argc, argv, i, value)) return 2;
            temperature_script = value;
        } else if (argument == "--repeat") {
            if (!option_value(argc, argv, i, value)) return 2;
            std::uint64_t repeat = 0;
            if (!parse_u64(value, repeat)) return 2;
            options.repeat = static_cast<std::uint32_t>(repeat);
        } else if (argument == "--ready-file") {
            if (!option_value(argc, argv, i, value)) return 2;
            options.ready_file = value;
        } else if (argument == "--mitigation-delta") {
            if (!option_value(argc, argv, i, value) || !parse_double(value, options.mitigation_delta))
                return 2;
        } else if (argument == "--hold") {
            options.hold = true;
        } else if (argument == "--real-telemetry") {
            options.real_telemetry = true;
        } else {
            std::fprintf(stderr, "unrecognised argument: %s\n", argument.c_str());
            return 2;
        }
    }

    for (const auto& token : split(temperature_script, ',')) {
        if (token.empty()) {
            continue;
        }
        double value = 0.0;
        if (!parse_double(token, value)) {
            std::fprintf(stderr, "malformed temperature token: %s\n", token.c_str());
            return 2;
        }
        options.temperatures.push_back(value);
    }
    if (options.temperatures.empty()) {
        options.temperatures.push_back(0.0);
    }

    auto socket_result = TcpSocket::connect_to(options.host, options.port);
    if (!socket_result.has_value()) {
        std::fprintf(stderr, "worker connect failed: %s\n",
                     socket_result.error().render().c_str());
        return 1;
    }
    TcpSocket socket = std::move(socket_result.value());

    wire::WorkerRegistration registration;
    registration.worker = WorkerId{StrongId<WorkerIdTag>{options.worker_id}};
    registration.boot = WorkerBootId{StrongId<WorkerBootIdTag>{options.boot}};
    registration.label = options.label;
    registration.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{1}};

    DeviceRegistration device;
    device.device = DeviceId{StrongId<DeviceIdTag>{options.device_id}};
    device.generation = DeviceGeneration{StrongId<DeviceGenerationTag>{options.device_generation}};
    device.node = NodeId{StrongId<NodeIdTag>{options.node_id}};
    device.node_generation = NodeGeneration{StrongId<NodeGenerationTag>{options.node_generation}};
    device.rack = RackId{StrongId<RackIdTag>{options.rack_id}};
    device.rack_generation = RackGeneration{StrongId<RackGenerationTag>{options.rack_generation}};
    device.label = Label{options.label};
    device.provenance = options.real_telemetry ? Provenance::REAL : Provenance::SYNTHETIC;
    device.capabilities.set(ThermalCapability::DEVICE_IDENTITY,
                            options.real_telemetry ? CapabilityState::SUPPORTED_REAL
                                                   : CapabilityState::SUPPORTED_SYNTHETIC);
    device.capabilities.set(ThermalCapability::TEMPERATURE, device.provenance == Provenance::REAL
                                                                ? CapabilityState::SUPPORTED_REAL
                                                                : CapabilityState::SUPPORTED_SYNTHETIC);
    device.capabilities.set(ThermalCapability::TEMPERATURE_LIMIT,
                            CapabilityState::SUPPORTED_SYNTHETIC);
    device.capabilities.set(ThermalCapability::THROTTLE_REASONS,
                            CapabilityState::SUPPORTED_SYNTHETIC);
    device.capabilities.set(ThermalCapability::CURRENT_CLOCK,
                            CapabilityState::SUPPORTED_SYNTHETIC);
    device.capabilities.set(ThermalCapability::MAX_CLOCK, CapabilityState::SUPPORTED_SYNTHETIC);
    registration.devices.push_back(device);

    auto hello_bytes = wire::encode_worker_registration(registration);
    auto status = send_bytes(socket, MessageKind::HELLO, 0, hello_bytes);
    if (!status.ok()) {
        std::fprintf(stderr, "worker hello send failed: %s\n", status.error().render().c_str());
        return 1;
    }
    (void)send_end(socket);

    std::vector<Frame> group;
    status = read_group(socket, group);
    if (!status.ok()) {
        std::fprintf(stderr, "worker hello response failed: %s\n",
                     status.error().render().c_str());
        return 1;
    }
    CoordinatorEpoch epoch{StrongId<CoordinatorEpochTag>{1}};
    for (const auto& frame : group) {
        if (frame.kind != MessageKind::HELLO_ACK) {
            continue;
        }
        auto response = wire::decode_response(frame.payload.data(), frame.payload.size());
        if (!response.has_value() || !response.value().status.ok()) {
            std::fprintf(stderr, "worker registration rejected: %s\n",
                         response.has_value() ? response.value().status.detail.c_str()
                                              : "malformed response");
            return 1;
        }
        const std::string& payload = response.value().payload;
        const auto position = payload.find("epoch=");
        if (position != std::string::npos) {
            std::uint64_t parsed = 0;
            if (parse_u64(payload.substr(position + 6), parsed)) {
                epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{parsed}};
            }
        }
    }

    std::printf("WORKER REGISTERED worker=%llu boot=%llu epoch=%llu\n",
                static_cast<unsigned long long>(options.worker_id),
                static_cast<unsigned long long>(options.boot),
                static_cast<unsigned long long>(epoch.value()));
    std::fflush(stdout);

    double current = options.temperatures.front();
    std::uint64_t sequence = 0;
    bool connected = true;

    const auto publish = [&](double temperature, std::optional<ThrottleObservation> throttle) {
        ++sequence;
        TelemetryGeneration telemetry{
            StrongId<TelemetryGenerationTag>{static_cast<std::uint64_t>(sequence)}};
        ThermalEvidence evidence = make_evidence(
            options, epoch, telemetry, sequence, DegreesCelsius{temperature}, throttle);
        auto bytes = wire::encode_evidence(evidence);
        auto send_status = send_bytes(socket, MessageKind::PUBLISH_EVIDENCE, 0, bytes);
        if (!send_status.ok()) {
            return false;
        }
        (void)send_end(socket);

        std::vector<Frame> frames;
        auto group_status = read_group(socket, frames);
        if (!group_status.ok()) {
            return false;
        }
        for (const auto& frame : frames) {
            if (frame.kind == MessageKind::PUBLISH_ACK) {
                auto response = wire::decode_response(frame.payload.data(), frame.payload.size());
                if (response.has_value() && response.value().status.ok()) {
                    std::printf("PUBLISHED seq=%llu temp=%.1f\n",
                                static_cast<unsigned long long>(sequence), temperature);
                } else {
                    std::printf("REJECTED seq=%llu code=%s\n",
                                static_cast<unsigned long long>(sequence),
                                response.has_value()
                                    ? std::string(to_string(response.value().status.code)).c_str()
                                    : "malformed");
                }
                std::fflush(stdout);
            }
            if (frame.kind == MessageKind::EVALUATE_RESPONSE) {
                auto summary = wire::decode_evaluation_summary(frame.payload.data(),
                                                               frame.payload.size());
                if (summary.has_value()) {
                    std::printf("EVALUATED domain=%llu state=%s derating=%s decision=%s\n",
                                static_cast<unsigned long long>(summary.value().domain.value()),
                                std::string(to_string(summary.value().state)).c_str(),
                                std::string(to_string(summary.value().derating)).c_str(),
                                std::string(to_string(summary.value().decision)).c_str());
                    std::fflush(stdout);
                }
            }
            if (frame.kind == MessageKind::DISPATCH_INTENT) {
                auto action = wire::decode_action(frame.payload.data(), frame.payload.size());
                if (!action.has_value()) {
                    continue;
                }
                ActionAck ack;
                ack.action_id = action.value().id;
                ack.action_generation = action.value().generation;
                ack.coordinator_epoch = action.value().authority.coordinator_epoch;
                ack.worker_boot = action.value().authority.worker_boot;
                ack.device_generation = action.value().authority.device_generation;
                ack.domain_generation = action.value().authority.domain_generation;
                ack.accepted = true;
                ack.detail = "synthetic backend accepted the instruction";
                auto ack_bytes = wire::encode_action_ack(ack);
                if (!send_bytes(socket, MessageKind::INTENT_ACK, 0, ack_bytes).ok()) {
                    return false;
                }
                (void)send_end(socket);
                std::vector<Frame> ack_frames;
                (void)read_group(socket, ack_frames);

                // The synthetic backend models the mitigation taking effect.
                current += options.mitigation_delta;

                ActionResult result;
                result.action_id = action.value().id;
                result.action_generation = action.value().generation;
                result.coordinator_epoch = action.value().authority.coordinator_epoch;
                result.worker_boot = action.value().authority.worker_boot;
                result.device_generation = action.value().authority.device_generation;
                result.domain_generation = action.value().authority.domain_generation;
                result.policy_generation =
                    ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{1}};
                result.telemetry_generation = TelemetryGeneration{
                    StrongId<TelemetryGenerationTag>{static_cast<std::uint64_t>(sequence)}};
                result.backend_succeeded = true;
                result.detail = "synthetic backend applied the instruction";
                auto result_bytes = wire::encode_action_result(result);
                if (!send_bytes(socket, MessageKind::INTENT_RESULT, 0, result_bytes).ok()) {
                    return false;
                }
                (void)send_end(socket);
                std::vector<Frame> result_frames;
                (void)read_group(socket, result_frames);
                std::printf("MITIGATION action=%llu intent=%s\n",
                            static_cast<unsigned long long>(action.value().id.value()),
                            std::string(to_string(action.value().intent)).c_str());
                std::fflush(stdout);
            }
            if (frame.kind == MessageKind::SHUTDOWN) {
                return false;
            }
        }
        return true;
    };

    for (std::uint32_t round = 0; round < options.repeat && connected; ++round) {
        for (const double temperature : options.temperatures) {
            current = temperature;
            if (!publish(current, std::nullopt)) {
                connected = false;
                break;
            }
        }
    }

    if (connected) {
        // The ready file is the driver rendezvous. It is written only after
        // every scripted observation has been published AND acknowledged by
        // the coordinator, so a reader of this file never has to guess how
        // long a publication takes.
        if (!options.ready_file.empty() && !write_ready_file(options.ready_file, "ready")) {
            std::fprintf(stderr, "cannot publish the worker ready file\n");
            return 1;
        }
        std::printf("WORKER READY worker=%llu boot=%llu published=%llu\n",
                    static_cast<unsigned long long>(options.worker_id),
                    static_cast<unsigned long long>(options.boot),
                    static_cast<unsigned long long>(sequence));
        std::fflush(stdout);
    }

    if (connected && options.hold) {
        // Idle until the coordinator closes the connection or asks the
        // worker to stop. No polling and no sleeps.
        for (;;) {
            auto frame = socket.recv_frame();
            if (!frame.has_value()) {
                break;
            }
            if (frame.value().kind == MessageKind::SHUTDOWN ||
                frame.value().kind == MessageKind::DISCONNECT) {
                break;
            }
            if (frame.value().kind == MessageKind::HEARTBEAT) {
                auto status_bytes = wire::encode_status(StatusPayload{});
                if (!send_bytes(socket, MessageKind::HEARTBEAT, 0, status_bytes).ok()) {
                    break;
                }
                (void)send_end(socket);
            }
        }
    }

    std::printf("WORKER EXIT\n");
    std::fflush(stdout);
    return 0;
}
