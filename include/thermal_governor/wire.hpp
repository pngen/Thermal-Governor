// Thermal Governor — explicit wire codec for the framed transport.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_WIRE_HPP
#define THERMAL_GOVERNOR_WIRE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "thermal_governor/action.hpp"
#include "thermal_governor/evidence.hpp"
#include "thermal_governor/error.hpp"
#include "thermal_governor/governance.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/policy.hpp"

namespace thermal_governor::wire {

/// Bounds applied to every decoded wire structure. Untrusted bytes reach
/// these decoders, so every count and length is arithmetic-checked.
struct WireLimits {
    std::size_t max_string_bytes = 64U * 1024U;
    std::size_t max_devices = 4096;
    std::size_t max_domain_members = 65536;
    std::size_t max_intents = 4096;
};

/// Worker announcement and registration.
struct WorkerRegistration {
    WorkerId worker{};
    WorkerBootId boot{};
    std::string label;
    CoordinatorEpoch coordinator_epoch{};
    std::vector<DeviceRegistration> devices;
};

/// A compact evaluation summary suitable for control-plane reporting.
struct EvaluationSummary {
    ThermalDomainId domain{};
    ThermalDomainGeneration domain_generation{};
    DeviceId device{};
    DeviceGeneration device_generation{};

    ThermalState state = ThermalState::UNKNOWN;
    DeratingLevel derating = DeratingLevel::UNKNOWN;
    ThermalDecision decision = ThermalDecision::UNKNOWN;
    ThrottleClass throttle_class = ThrottleClass::THROTTLE_UNKNOWN;
    Provenance provenance = Provenance::UNKNOWN;

    DegreesCelsius temperature{};
    DegreesCelsius governing_limit{};
    TemperatureDelta effective_headroom{};
    Percent permitted_concurrency{0.0};

    bool recovery_allowed = false;
    std::uint32_t qualifying_samples = 0;
    std::uint32_t required_samples = 0;

    CoordinatorEpoch coordinator_epoch{};
    ThermalPolicyGeneration policy_generation{};
    TelemetryGeneration telemetry_generation{};

    std::vector<MitigationIntent> required_intents;
    std::vector<ThermalReasonCode> reasons;
};

/// A typed error transferred across the wire.
struct StatusPayload {
    ThermalErrorCode code = ThermalErrorCode::NONE;
    std::string detail;

    [[nodiscard]] bool ok() const noexcept { return code == ThermalErrorCode::NONE; }
};

/// A control command issued by the CLI or a test driver.
struct CommandRequest {
    std::string verb;
    std::vector<std::string> arguments;
    std::string payload;
};

/// A control response.
struct CommandResponse {
    StatusPayload status;
    std::string payload;
};

// --- Registration ---------------------------------------------------------

[[nodiscard]] std::vector<std::uint8_t> encode_worker_registration(
    const WorkerRegistration& registration);
[[nodiscard]] Result<WorkerRegistration> decode_worker_registration(const std::uint8_t* data,
                                                                    std::size_t length,
                                                                    const WireLimits& limits = {});

// --- Evidence -------------------------------------------------------------

[[nodiscard]] std::vector<std::uint8_t> encode_evidence(const ThermalEvidence& evidence);
[[nodiscard]] Result<ThermalEvidence> decode_evidence(const std::uint8_t* data, std::size_t length,
                                                      const WireLimits& limits = {});

// --- Actions --------------------------------------------------------------

[[nodiscard]] std::vector<std::uint8_t> encode_action(const ThermalAction& action);
[[nodiscard]] Result<ThermalAction> decode_action(const std::uint8_t* data, std::size_t length,
                                                  const WireLimits& limits = {});

[[nodiscard]] std::vector<std::uint8_t> encode_action_ack(const ActionAck& ack);
[[nodiscard]] Result<ActionAck> decode_action_ack(const std::uint8_t* data, std::size_t length,
                                                  const WireLimits& limits = {});

[[nodiscard]] std::vector<std::uint8_t> encode_action_result(const ActionResult& result);
[[nodiscard]] Result<ActionResult> decode_action_result(const std::uint8_t* data,
                                                        std::size_t length,
                                                        const WireLimits& limits = {});

// --- Reporting ------------------------------------------------------------

[[nodiscard]] std::vector<std::uint8_t> encode_evaluation_summary(
    const EvaluationSummary& summary);
[[nodiscard]] Result<EvaluationSummary> decode_evaluation_summary(const std::uint8_t* data,
                                                                  std::size_t length,
                                                                  const WireLimits& limits = {});

// --- Control --------------------------------------------------------------

[[nodiscard]] std::vector<std::uint8_t> encode_status(const StatusPayload& status);
[[nodiscard]] Result<StatusPayload> decode_status(const std::uint8_t* data, std::size_t length,
                                                  const WireLimits& limits = {});

[[nodiscard]] std::vector<std::uint8_t> encode_command(const CommandRequest& request);
[[nodiscard]] Result<CommandRequest> decode_command(const std::uint8_t* data, std::size_t length,
                                                    const WireLimits& limits = {});

[[nodiscard]] std::vector<std::uint8_t> encode_response(const CommandResponse& response);
[[nodiscard]] Result<CommandResponse> decode_response(const std::uint8_t* data, std::size_t length,
                                                      const WireLimits& limits = {});

/// Build an evaluation summary from a full evaluation.
[[nodiscard]] EvaluationSummary summarise(const ThermalEvaluation& evaluation);

}  // namespace thermal_governor::wire

#endif  // THERMAL_GOVERNOR_WIRE_HPP
