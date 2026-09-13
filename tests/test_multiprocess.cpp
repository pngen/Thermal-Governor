// Thermal Governor — real multiprocess proof.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This suite spawns genuine operating system processes: one coordinator, two
// workers and this test binary acting as the control driver. Nothing here
// substitutes threads for process isolation, and no case uses a timeout.
//
// Startup rendezvous is an explicit filesystem handshake: each child writes
// its bound port to a ready file and the driver reads it. The driver never
// guesses a duration.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "framework.hpp"
#include "process_util.hpp"
#include "thermal_governor/persistence.hpp"
#include "thermal_governor/thermal_governor.hpp"
#include "thermal_governor/wire.hpp"

namespace {

using namespace thermal_governor;

constexpr int kMaxRendezvousAttempts = 200000;

/// Parse an unsigned decimal argument. Returns false on any malformed input.
[[nodiscard]] bool parse_u64(const std::string& text, std::uint64_t& out);

[[nodiscard]] std::string tool_executable(const char* name) {
    const std::filesystem::path directory(tg::executable_directory());
    const std::filesystem::path candidate = directory.parent_path() / "tools" / name;
    if (std::filesystem::exists(candidate)) {
        return candidate.string();
    }
    return (directory / name).string();
}

/// Read a ready file produced by a child process. This is the explicit
/// synchronisation point between the driver and a freshly started process.
[[nodiscard]] bool await_ready_file(const std::string& path, tg::ChildProcess& process,
                                    std::string& content) {
    for (int attempt = 0; attempt < kMaxRendezvousAttempts; ++attempt) {
        if (tg::read_file(path, content) && !content.empty()) {
            return true;
        }
        if (!process.running()) {
            return false;
        }
        std::this_thread::yield();
    }
    return false;
}

[[nodiscard]] Status send_group(TcpSocket& socket, MessageKind kind,
                                const std::vector<std::uint8_t>& payload) {
    auto status = socket.send_frame(kind, 0, payload.data(), payload.size());
    if (!status.ok()) {
        return status;
    }
    return socket.send_frame(MessageKind::RESPONSE_END, 0, nullptr, 0);
}

[[nodiscard]] Status read_group(TcpSocket& socket, std::vector<Frame>& frames) {
    for (;;) {
        auto frame = socket.recv_frame();
        if (!frame.has_value()) {
            return Status(frame.error());
        }
        if (frame.value().kind == MessageKind::RESPONSE_END) {
            return Status::success();
        }
        frames.push_back(std::move(frame.value()));
    }
}

[[nodiscard]] Result<wire::CommandResponse> run_command(
    std::uint16_t port, const std::string& verb, std::vector<std::string> arguments = {},
    const std::string& payload = {}) {
    auto connected = TcpSocket::connect_to("127.0.0.1", port);
    if (!connected.has_value()) {
        return connected.error();
    }
    TcpSocket socket = std::move(connected.value());

    wire::CommandRequest request;
    request.verb = verb;
    request.arguments = std::move(arguments);
    request.payload = payload;
    auto encoded = wire::encode_command(request);
    auto sent = send_group(socket, MessageKind::QUERY_REQUEST, encoded);
    if (!sent.ok()) {
        return ThermalError{sent.code(), sent.error().detail};
    }
    std::vector<Frame> frames;
    auto group = read_group(socket, frames);
    if (!group.ok()) {
        return ThermalError{group.code(), group.error().detail};
    }
    for (const auto& frame : frames) {
        if (frame.kind == MessageKind::QUERY_RESPONSE) {
            auto response = wire::decode_response(frame.payload.data(), frame.payload.size());
            if (!response.has_value()) {
                return response.error();
            }
            return response.value();
        }
    }
    return ThermalError{ThermalErrorCode::TRANSPORT_CLOSED, "no response frame was received"};
}

[[nodiscard]] std::string hex_of(const std::vector<std::uint8_t>& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(digits[(byte >> 4) & 0x0FU]);
        out.push_back(digits[byte & 0x0FU]);
    }
    return out;
}

/// Build evidence for a device as a specific worker incarnation.
[[nodiscard]] ThermalEvidence make_evidence(std::uint64_t device, std::uint64_t domain,
                                            double temperature, std::uint64_t sequence,
                                            CoordinatorEpoch epoch, std::uint64_t worker,
                                            std::uint64_t boot) {
    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{sequence}};
    evidence.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{device}},
                                              DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    evidence.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{domain}};
    evidence.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    evidence.temperature = DegreesCelsius{temperature};
    evidence.source = MeasurementSource::SYNTHETIC_MODEL;
    evidence.provenance = Provenance::SYNTHETIC;
    evidence.measurement_sequence = sequence;
    evidence.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{sequence}};
    evidence.worker = WorkerId{StrongId<WorkerIdTag>{worker}};
    evidence.worker_boot = WorkerBootId{StrongId<WorkerBootIdTag>{boot}};
    evidence.coordinator_epoch = epoch;
    evidence.measured_at = std::chrono::steady_clock::now();
    evidence.measured_wall_at = std::chrono::system_clock::now();
    return evidence;
}

struct CoordinatorHandle {
    tg::ChildProcess process;
    std::uint16_t port = 0;
    std::string state_path;
    std::string ready_path;
    std::uint64_t epoch = 0;
};

[[nodiscard]] bool start_coordinator(CoordinatorHandle& handle) {
    const std::string ready = tg::unique_temp_path("tg-coord-ready");
    tg::remove_file(ready);
    const std::string arguments = "--port 0 --state \"" + handle.state_path + "\" --ready-file \"" +
                                  ready + "\"";
    if (!tg::ChildProcess::launch(tool_executable("thermal_governor_coordinator.exe"), arguments,
                                  handle.process)) {
        return false;
    }
    std::string content;
    if (!await_ready_file(ready, handle.process, content)) {
        return false;
    }
    std::uint64_t port = 0;
    // The child may still be mid-write; a short explicit retry keeps the
    // rendezvous exact without assuming a duration.
    for (int attempt = 0; attempt < 64 && !parse_u64(content, port); ++attempt) {
        std::this_thread::yield();
        (void)tg::read_file(ready, content);
    }
    if (!parse_u64(content, port) || port == 0 || port > 65535) {
        return false;
    }
    handle.port = static_cast<std::uint16_t>(port);
    handle.ready_path = ready;
    tg::remove_file(ready);
    return true;
}

[[nodiscard]] bool start_worker(tg::ChildProcess& process, std::uint16_t port,
                                std::uint64_t worker, std::uint64_t boot, std::uint64_t device,
                                std::uint64_t domain, const std::string& temperatures) {
    const std::string ready = tg::unique_temp_path("tg-worker-ready");
    tg::remove_file(ready);
    const std::string arguments =
        "--port " + std::to_string(port) + " --worker-id " + std::to_string(worker) +
        " --boot " + std::to_string(boot) + " --device " + std::to_string(device) +
        " --domain " + std::to_string(domain) + " --label worker-" + std::to_string(worker) +
        " --temps \"" + temperatures + "\" --repeat 1 --hold --ready-file \"" + ready + "\"";
    if (!tg::ChildProcess::launch(tool_executable("thermal_governor_worker.exe"), arguments,
                                  process)) {
        return false;
    }
    std::string content;
    if (!await_ready_file(ready, process, content)) {
        return false;
    }
    tg::remove_file(ready);
    return true;
}

/// Stop a worker and reap it. A worker that is already gone is not an error.
void stop_process(tg::ChildProcess& process) {
    if (!process.valid()) {
        return;
    }
    if (process.running()) {
        (void)process.terminate_now();
    }
    std::uint32_t code = 0;
    (void)process.wait(code);
    process.close();
}

[[nodiscard]] bool parse_u64(const std::string& text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c == '\r' || c == '\n' || c == ' ') {
            continue;
        }
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    out = value;
    return true;
}

[[nodiscard]] std::uint64_t epoch_of(std::uint16_t port) {
    auto response = run_command(port, "epoch");
    if (!response.has_value() || !response.value().status.ok()) {
        return 0;
    }
    std::uint64_t value = 0;
    if (!parse_u64(response.value().payload, value)) {
        return 0;
    }
    return value;
}

[[nodiscard]] std::string evaluate_domain(std::uint16_t port, std::uint64_t domain) {
    auto response = run_command(port, "evaluate", {std::to_string(domain)});
    if (!response.has_value() || !response.value().status.ok()) {
        return "<error>";
    }
    return response.value().payload;
}

[[nodiscard]] bool payload_contains(const std::string& payload, const std::string& needle) {
    return payload.find(needle) != std::string::npos;
}

}  // namespace

TG_CASE(multiprocess, full_worker_and_coordinator_lifecycle) {
    CoordinatorHandle coordinator;
    coordinator.state_path = tg::unique_temp_path("tg-mp-state");
    tg::remove_file(coordinator.state_path);

    TG_PHASE("COORDINATOR_START");
    TG_CHECK(start_coordinator(coordinator));
    coordinator.epoch = epoch_of(coordinator.port);
    TG_CHECK(coordinator.epoch >= 1);

    TG_PHASE("REGISTER");
    // A synthetic-threshold policy: the mechanism is real, the thresholds are
    // deliberately synthetic so that derating can be exercised safely.
    auto thresholds = run_command(coordinator.port, "set-thresholds",
                                  {"75", "80", "88", "72", "3", "2", "1", "1", "2", "60000", "3", "0"});
    TG_CHECK(thresholds.has_value() && thresholds.value().status.ok());
    auto thresholds_probe = run_command(coordinator.port, "status");
    if (!thresholds_probe.has_value()) {
        std::printf("NOTE coordinator status failed: %s\n",
                    thresholds_probe.error().render().c_str());
        std::fflush(stdout);
    }
    auto domain_one = run_command(coordinator.port, "register-domain",
                                  {"1", "1", "ACCELERATOR", "SYNTHETIC", "HOTTEST_MEMBER_GOVERNS",
                                   "GPU0", "1", "d:1:1"});
    if (!domain_one.has_value()) {
        std::printf("NOTE register-domain transport failed: %s\n",
                    domain_one.error().render().c_str());
        std::fflush(stdout);
    } else if (!domain_one.value().status.ok()) {
        std::printf("NOTE register-domain rejected: %s %s\n",
                    std::string(to_string(domain_one.value().status.code)).c_str(),
                    domain_one.value().status.detail.c_str());
        std::fflush(stdout);
    }
    TG_CHECK(domain_one.has_value() && domain_one.value().status.ok());
    auto domain_two = run_command(coordinator.port, "register-domain",
                                  {"2", "1", "ACCELERATOR", "SYNTHETIC", "HOTTEST_MEMBER_GOVERNS",
                                   "GPU1", "1", "d:2:1"});
    TG_CHECK(domain_two.has_value() && domain_two.value().status.ok());

    TG_PHASE("WORKER_A_REGISTER");
    tg::ChildProcess worker_a;
    TG_CHECK(start_worker(worker_a, coordinator.port, 1, 1, 1, 1, "70,85"));
    TG_PHASE("WORKER_B_REGISTER");
    tg::ChildProcess worker_b;
    TG_CHECK(start_worker(worker_b, coordinator.port, 2, 1, 2, 2, "50,55"));

    TG_PHASE("EVALUATE");
    // Workers publish synchronously before reporting ready, so both domains
    // have current evidence by the time this runs.
    const std::string a_derated = evaluate_domain(coordinator.port, 1);
    TG_CHECK(payload_contains(a_derated, "DERATED"));
    const std::string b_legal = evaluate_domain(coordinator.port, 2);
    TG_CHECK(payload_contains(b_legal, "NORMAL"));

    TG_PHASE("KILL");
    TG_CHECK(worker_a.running());
    TG_CHECK(worker_a.terminate_now());
    std::uint32_t exit_code = 0;
    TG_CHECK(worker_a.wait(exit_code));
    worker_a.close();

    TG_PHASE("REVOKE");
    auto death = run_command(coordinator.port, "worker-death", {"1", "1"});
    TG_CHECK(death.has_value() && death.value().status.ok());

    const std::string after_death = evaluate_domain(coordinator.port, 1);
    TG_CHECK(payload_contains(after_death, "REVALIDATION_REQUIRED"));
    // The independent worker is unaffected.
    const std::string still_legal = evaluate_domain(coordinator.port, 2);
    TG_CHECK(payload_contains(still_legal, "NORMAL"));

    TG_PHASE("REJECT_STALE_A1");
    const std::uint64_t epoch_now = epoch_of(coordinator.port);
    auto stale = wire::encode_evidence(
        make_evidence(1, 1, 60.0, 900001, CoordinatorEpoch{StrongId<CoordinatorEpochTag>{epoch_now}}, 1, 1));
    auto stale_result = run_command(coordinator.port, "publish", {}, hex_of(stale));
    TG_CHECK(!stale_result.has_value() ||
              stale_result.value().status.code == ThermalErrorCode::STALE_WORKER);

    TG_PHASE("REINCARNATE");
    tg::ChildProcess worker_a2;
    TG_CHECK(start_worker(worker_a2, coordinator.port, 1, 2, 1, 1, "62"));
    const std::string recovered = evaluate_domain(coordinator.port, 1);
    if (!payload_contains(recovered, "NORMAL")) {
        std::printf("NOTE reincarnated evaluation:\n%s\n", recovered.c_str());
        std::fflush(stdout);
    }
    TG_CHECK(payload_contains(recovered, "NORMAL"));

    TG_PHASE("A2_HAS_NO_INHERITED_AUTHORITY");
    // Evidence from the retired incarnation is still refused even though the
    // reincarnated worker now holds live authority.
    const std::uint64_t epoch_after = epoch_of(coordinator.port);
    auto still_stale = wire::encode_evidence(
        make_evidence(1, 1, 20.0, 900002,
                      CoordinatorEpoch{StrongId<CoordinatorEpochTag>{epoch_after}}, 1, 1));
    auto still_stale_result = run_command(coordinator.port, "publish", {}, hex_of(still_stale));
    TG_CHECK(!still_stale_result.has_value() ||
              still_stale_result.value().status.code == ThermalErrorCode::STALE_WORKER);

    TG_PHASE("HISTORY");
    auto history = run_command(coordinator.port, "history");
    TG_CHECK(history.has_value() && history.value().status.ok());
    TG_CHECK(payload_contains(history.value().payload, "DERATED"));

    TG_PHASE("PERSIST");
    auto persisted = run_command(coordinator.port, "persist");
    TG_CHECK(persisted.has_value() && persisted.value().status.ok());

    TG_PHASE("CRASH");
    TG_CHECK(coordinator.process.running());
    TG_CHECK(coordinator.process.terminate_now());
    std::uint32_t coordinator_exit = 0;
    TG_CHECK(coordinator.process.wait(coordinator_exit));
    coordinator.process.close();

    TG_PHASE("RESTART");
    CoordinatorHandle restarted;
    restarted.state_path = coordinator.state_path;
    TG_CHECK(start_coordinator(restarted));
    const std::uint64_t new_epoch = epoch_of(restarted.port);
    TG_CHECK(new_epoch > coordinator.epoch);

    TG_PHASE("REVALIDATE");
    const std::string after_restart = evaluate_domain(restarted.port, 1);
    TG_CHECK(payload_contains(after_restart, "REVALIDATION_REQUIRED"));

    TG_PHASE("REJECT_OLD_EPOCH");
    auto old_epoch = wire::encode_evidence(make_evidence(
        1, 1, 60.0, 900003, CoordinatorEpoch{StrongId<CoordinatorEpochTag>{coordinator.epoch}}, 1, 2));
    auto old_epoch_result = run_command(restarted.port, "publish", {}, hex_of(old_epoch));
    TG_CHECK(!old_epoch_result.has_value() ||
              old_epoch_result.value().status.code == ThermalErrorCode::STALE_EPOCH);

    TG_PHASE("RECONNECT");
    tg::ChildProcess worker_a3;
    TG_CHECK(start_worker(worker_a3, restarted.port, 1, 3, 1, 1, "63"));
    const std::string reestablished = evaluate_domain(restarted.port, 1);
    TG_CHECK(payload_contains(reestablished, "NORMAL"));

    TG_PHASE("HISTORY_SURVIVES");
    auto restored_history = run_command(restarted.port, "history");
    TG_CHECK(restored_history.has_value() && restored_history.value().status.ok());
    TG_CHECK(!restored_history.value().payload.empty());

    TG_PHASE("SHUTDOWN");
    auto shutdown = run_command(restarted.port, "shutdown");
    (void)shutdown;  // The coordinator closes the connection as it stops.
    std::uint32_t final_exit = 0;
    TG_CHECK(restarted.process.wait(final_exit));
    TG_CHECK_EQ(final_exit, 0U);
    restarted.process.close();

    stop_process(worker_b);
    stop_process(worker_a2);
    stop_process(worker_a3);
    tg::remove_file(coordinator.state_path);
}

TG_CASE(multiprocess, repeated_reconnect_is_stable) {
    CoordinatorHandle coordinator;
    coordinator.state_path = tg::unique_temp_path("tg-mp-reconnect");
    tg::remove_file(coordinator.state_path);
    TG_PHASE("COORDINATOR_START");
    TG_CHECK(start_coordinator(coordinator));

    TG_PHASE("RECONNECT");
    for (int attempt = 0; attempt < 25; ++attempt) {
        auto status = run_command(coordinator.port, "status");
        if (!status.has_value()) {
            std::printf("NOTE run_command failed: %s port=%u\n",
                        status.error().render().c_str(),
                        static_cast<unsigned>(coordinator.port));
            std::fflush(stdout);
        }
        TG_CHECK(status.has_value());
        TG_CHECK(status.value().status.ok());
        TG_CHECK(payload_contains(status.value().payload, "CoordinatorEpoch"));
        // Half-close without a graceful disconnect frame, as an abrupt client
        // failure would.
        auto connected = TcpSocket::connect_to("127.0.0.1", coordinator.port);
        TG_CHECK(connected.has_value());
        connected.value().shutdown_send();
        connected.value().close();
    }

    TG_PHASE("SHUTDOWN");
    (void)run_command(coordinator.port, "shutdown");
    std::uint32_t exit_code = 0;
    TG_CHECK(coordinator.process.wait(exit_code));
    TG_CHECK_EQ(exit_code, 0U);
    coordinator.process.close();
    tg::remove_file(coordinator.state_path);
}

TG_CASE(multiprocess, malformed_frames_do_not_kill_the_coordinator) {
    CoordinatorHandle coordinator;
    coordinator.state_path = tg::unique_temp_path("tg-mp-malformed");
    tg::remove_file(coordinator.state_path);
    TG_PHASE("COORDINATOR_START");
    TG_CHECK(start_coordinator(coordinator));

    TG_PHASE("MALFORMED_FRAME");
    {
        auto connected = TcpSocket::connect_to("127.0.0.1", coordinator.port);
        TG_CHECK(connected.has_value());
        TcpSocket socket = std::move(connected.value());
        // A well-formed frame whose payload checksum is wrong.
        auto encoded = encode_frame(MessageKind::QUERY_REQUEST, 0, nullptr, 0);
        TG_CHECK(encoded.has_value());
        std::vector<std::uint8_t> bytes = encoded.value();
        bytes[bytes.size() - 1] = static_cast<std::uint8_t>(bytes[bytes.size() - 1] ^ 0xFFU);
        auto sent = socket.send_all(bytes.data(), bytes.size());
        TG_CHECK(sent.ok());
        // The coordinator must refuse the frame rather than act on it.
        auto frame = socket.recv_frame();
        if (frame.has_value()) {
            TG_CHECK_EQ(frame.value().kind, MessageKind::ERROR_RESPONSE);
        }
        socket.close();
    }

    TG_PHASE("PARTIAL_FRAME");
    {
        auto connected = TcpSocket::connect_to("127.0.0.1", coordinator.port);
        TG_CHECK(connected.has_value());
        TcpSocket socket = std::move(connected.value());
        auto encoded = encode_frame(MessageKind::QUERY_REQUEST, 0, nullptr, 0);
        TG_CHECK(encoded.has_value());
        // Only half the header, then an abrupt close.
        TG_CHECK(socket.send_all(encoded.value().data(), 9).ok());
        socket.close();
    }

    TG_PHASE("UNKNOWN_FRAME_KIND");
    {
        auto connected = TcpSocket::connect_to("127.0.0.1", coordinator.port);
        TG_CHECK(connected.has_value());
        TcpSocket socket = std::move(connected.value());
        std::vector<std::uint8_t> header(20, 0);
        header[0] = 0x54;
        header[1] = 0x47;
        header[2] = 0x4F;
        header[3] = 0x56;
        header[4] = 1;
        header[5] = 0;
        header[6] = 0xEE;
        header[7] = 0xEE;
        const std::uint32_t crc = crc32c(header.data(), 16);
        header[16] = static_cast<std::uint8_t>(crc & 0xFFU);
        header[17] = static_cast<std::uint8_t>((crc >> 8) & 0xFFU);
        header[18] = static_cast<std::uint8_t>((crc >> 16) & 0xFFU);
        header[19] = static_cast<std::uint8_t>((crc >> 24) & 0xFFU);
        TG_CHECK(socket.send_all(header.data(), header.size()).ok());
        socket.close();
    }

    TG_PHASE("STILL_ALIVE");
    // None of the attacks may have killed or wedged the coordinator.
    auto status = run_command(coordinator.port, "status");
    TG_CHECK(status.has_value());
    TG_CHECK(status.value().status.ok());

    TG_PHASE("SHUTDOWN");
    (void)run_command(coordinator.port, "shutdown");
    std::uint32_t exit_code = 0;
    TG_CHECK(coordinator.process.wait(exit_code));
    TG_CHECK_EQ(exit_code, 0U);
    coordinator.process.close();
    tg::remove_file(coordinator.state_path);
}
