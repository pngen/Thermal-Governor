// Thermal Governor — generation-bound thermal action authority.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_ACTION_HPP
#define THERMAL_GOVERNOR_ACTION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "thermal_governor/decision.hpp"
#include "thermal_governor/error.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/mitigation.hpp"
#include "thermal_governor/provenance.hpp"
#include "thermal_governor/quantity.hpp"
#include "thermal_governor/time.hpp"

namespace thermal_governor {

/// Lifecycle position of a thermal mitigation action.
///
/// ACKNOWLEDGED is not EFFECTIVE. A backend returning success does not prove
/// the thermal condition improved.
enum class ActionLifecycle : std::uint8_t {
    PLANNED = 0,
    AUTHORIZED,
    DISPATCHED,
    ACKNOWLEDGED,
    VERIFYING,
    EFFECTIVE,
    PARTIALLY_EFFECTIVE,
    INEFFECTIVE,
    WORSENED,
    FAILED,
    SUPERSEDED,
    CANCELLED,
    OUTCOME_UNKNOWN,
};

inline constexpr std::uint32_t kActionLifecycleCount = 13;

[[nodiscard]] constexpr std::string_view to_string(ActionLifecycle l) noexcept {
    switch (l) {
        case ActionLifecycle::PLANNED: return "PLANNED";
        case ActionLifecycle::AUTHORIZED: return "AUTHORIZED";
        case ActionLifecycle::DISPATCHED: return "DISPATCHED";
        case ActionLifecycle::ACKNOWLEDGED: return "ACKNOWLEDGED";
        case ActionLifecycle::VERIFYING: return "VERIFYING";
        case ActionLifecycle::EFFECTIVE: return "EFFECTIVE";
        case ActionLifecycle::PARTIALLY_EFFECTIVE: return "PARTIALLY_EFFECTIVE";
        case ActionLifecycle::INEFFECTIVE: return "INEFFECTIVE";
        case ActionLifecycle::WORSENED: return "WORSENED";
        case ActionLifecycle::FAILED: return "FAILED";
        case ActionLifecycle::SUPERSEDED: return "SUPERSEDED";
        case ActionLifecycle::CANCELLED: return "CANCELLED";
        case ActionLifecycle::OUTCOME_UNKNOWN: return "OUTCOME_UNKNOWN";
    }
    return "UNRECOGNISED_LIFECYCLE";
}

/// True when no further transition may leave this lifecycle position.
[[nodiscard]] constexpr bool is_terminal(ActionLifecycle l) noexcept {
    switch (l) {
        case ActionLifecycle::EFFECTIVE:
        case ActionLifecycle::PARTIALLY_EFFECTIVE:
        case ActionLifecycle::INEFFECTIVE:
        case ActionLifecycle::WORSENED:
        case ActionLifecycle::FAILED:
        case ActionLifecycle::SUPERSEDED:
        case ActionLifecycle::CANCELLED:
        case ActionLifecycle::OUTCOME_UNKNOWN:
            return true;
        default:
            return false;
    }
}

/// The generation binding of a thermal action.
///
/// Immediately before dispatch the action is revalidated against current
/// state. A stale plan must not execute.
struct ActionAuthority {
    CoordinatorEpoch coordinator_epoch{};
    ActionId action_id{};
    ActionGeneration action_generation{};

    ThermalDomainId domain{};
    ThermalDomainGeneration domain_generation{};

    DeviceId device{};
    DeviceGeneration device_generation{};

    ThermalPolicyGeneration policy_generation{};
    TelemetryGeneration telemetry_generation{};

    WorkerId worker{};
    WorkerBootId worker_boot{};

    TopologyGeneration topology_generation{};
    CapabilityGeneration capability_generation{};

    friend bool operator==(const ActionAuthority&, const ActionAuthority&) = default;
};

/// The authoritative current generations an action is checked against.
struct AuthoritySnapshot {
    CoordinatorEpoch coordinator_epoch{};
    ThermalDomainGeneration domain_generation{};
    DeviceGeneration device_generation{};
    ThermalPolicyGeneration policy_generation{};
    TelemetryGeneration telemetry_generation{};
    WorkerBootId worker_boot{};
    TopologyGeneration topology_generation{};
    CapabilityGeneration capability_generation{};
    ActionGeneration action_generation{};
};

/// Revalidate an action immediately before dispatch.
///
/// Rejects stale coordinator epoch, worker boot, device generation, thermal
/// domain generation, policy generation, telemetry generation, topology
/// generation, capability generation and action generation.
[[nodiscard]] Status revalidate_action_authority(const ActionAuthority& authority,
                                                 const AuthoritySnapshot& current);

/// A materialised mitigation action record.
struct ThermalAction {
    ActionId id{};
    ActionGeneration generation{};
    ActionAuthority authority{};

    MitigationIntent intent = MitigationIntent::NO_ACTION;
    std::optional<MegaHertz> requested_clock_ceiling;
    std::optional<Percent> requested_concurrency;
    std::optional<Percent> requested_admission;

    ActionLifecycle lifecycle = ActionLifecycle::PLANNED;
    ReasonCodeSet reasons;

    Provenance provenance = Provenance::UNKNOWN;
    std::string detail;

    SteadyTimePoint planned_at{};
    SteadyTimePoint authorized_at{};
    SteadyTimePoint dispatched_at{};
    SteadyTimePoint acknowledged_at{};
    SteadyTimePoint terminal_at{};

    VerificationGeneration verification_generation{};
    TelemetryGeneration verification_telemetry_generation{};

    /// Superseding action, when this action was replaced.
    std::optional<ActionId> superseded_by;
    /// Entity that authorised the action; zero for policy-driven actions.
    std::string authorization_source;
};

/// Honest cancellation outcome. A cancelled, never-dispatched action must
/// not later report success, and a dispatched action may still require
/// verification because hardware state was already touched.
enum class CancellationResult : std::uint8_t {
    CANCELLED_BEFORE_DISPATCH = 0,
    CANCELLED_AFTER_DISPATCH,
    VERIFICATION_REQUIRED,
    ALREADY_EFFECTIVE,
    OUTCOME_UNKNOWN,
};

[[nodiscard]] constexpr std::string_view to_string(CancellationResult r) noexcept {
    switch (r) {
        case CancellationResult::CANCELLED_BEFORE_DISPATCH: return "CANCELLED_BEFORE_DISPATCH";
        case CancellationResult::CANCELLED_AFTER_DISPATCH: return "CANCELLED_AFTER_DISPATCH";
        case CancellationResult::VERIFICATION_REQUIRED: return "VERIFICATION_REQUIRED";
        case CancellationResult::ALREADY_EFFECTIVE: return "ALREADY_EFFECTIVE";
        case CancellationResult::OUTCOME_UNKNOWN: return "OUTCOME_UNKNOWN";
    }
    return "UNRECOGNISED_CANCELLATION_RESULT";
}

/// Canonical rendering of an action record.
[[nodiscard]] std::string render_action(const ThermalAction& action);

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_ACTION_HPP
