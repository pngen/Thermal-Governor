// Thermal Governor — typed error semantics.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_ERROR_HPP
#define THERMAL_GOVERNOR_ERROR_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace thermal_governor {

/// Typed thermal-governance error codes.
///
/// Policy outcomes are never collapsed into bool. Every rejection carries a
/// stable machine-readable code plus a deterministic human string.
enum class ThermalErrorCode : std::uint32_t {
    NONE = 0,

    // Generation / authority fencing.
    STALE_EPOCH = 1,
    STALE_WORKER = 2,
    STALE_DEVICE_GENERATION = 3,
    STALE_DOMAIN_GENERATION = 4,
    STALE_TELEMETRY = 5,
    STALE_POLICY = 6,
    STALE_ACTION = 7,
    STALE_TOPOLOGY = 8,
    STALE_CAPABILITY = 9,
    STALE_VERIFICATION = 10,

    // Identity resolution.
    UNKNOWN_DEVICE = 20,
    UNKNOWN_DOMAIN = 21,
    UNKNOWN_WORKLOAD = 22,
    UNKNOWN_NODE = 23,
    UNKNOWN_RACK = 24,
    UNKNOWN_ACTION = 25,
    UNKNOWN_POLICY = 26,
    UNKNOWN_COORDINATOR = 27,

    // Capability / evidence.
    CAPABILITY_UNSUPPORTED = 40,
    EVIDENCE_UNKNOWN = 41,
    EVIDENCE_STALE = 42,
    EVIDENCE_CONFLICT = 43,
    TEMPERATURE_INVALID = 44,
    EVIDENCE_DUPLICATE = 45,

    // Policy.
    POLICY_INVALID = 60,
    POLICY_CYCLE = 61,
    POLICY_LIMIT_EXCEEDED = 62,

    // Thermal outcomes.
    THERMAL_WARNING = 80,
    THERMAL_DERATING_REQUIRED = 81,
    THERMAL_CRITICAL = 82,
    NO_SAFE_THERMAL_ENVELOPE = 83,
    RECOVERY_FORBIDDEN = 84,
    REVALIDATION_REQUIRED = 85,

    // Action / verification.
    ACTION_INFEASIBLE = 100,
    ACTION_INEFFECTIVE = 101,
    ACTION_SUPERSEDED = 102,
    ACTION_CANCELLED = 103,
    BACKEND_FAILURE = 104,
    SECONDARY_VIOLATION = 105,

    // Persistence.
    PERSISTENCE_CORRUPT = 120,
    PERSISTENCE_UNSUPPORTED_VERSION = 121,
    PERSISTENCE_IO = 122,
    PERSISTENCE_TRUNCATED = 123,
    PERSISTENCE_TRAILING_GARBAGE = 124,
    PERSISTENCE_INTEGRITY = 125,

    // Transport.
    FRAME_CORRUPT = 140,
    FRAME_TOO_LARGE = 141,
    FRAME_TRUNCATED = 142,
    PROTOCOL_UNSUPPORTED = 143,
    TRANSPORT_IO = 144,
    TRANSPORT_CLOSED = 145,

    // Generic.
    DUPLICATE_CONFLICT = 160,
    RESOURCE_EXHAUSTED = 161,
    OUTCOME_UNKNOWN = 162,
    INVALID_ARGUMENT = 163,
    NOT_IMPLEMENTED = 164,
    INTERNAL = 165,
};

[[nodiscard]] constexpr std::string_view to_string(ThermalErrorCode code) noexcept {
    switch (code) {
        case ThermalErrorCode::NONE: return "NONE";
        case ThermalErrorCode::STALE_EPOCH: return "STALE_EPOCH";
        case ThermalErrorCode::STALE_WORKER: return "STALE_WORKER";
        case ThermalErrorCode::STALE_DEVICE_GENERATION: return "STALE_DEVICE_GENERATION";
        case ThermalErrorCode::STALE_DOMAIN_GENERATION: return "STALE_DOMAIN_GENERATION";
        case ThermalErrorCode::STALE_TELEMETRY: return "STALE_TELEMETRY";
        case ThermalErrorCode::STALE_POLICY: return "STALE_POLICY";
        case ThermalErrorCode::STALE_ACTION: return "STALE_ACTION";
        case ThermalErrorCode::STALE_TOPOLOGY: return "STALE_TOPOLOGY";
        case ThermalErrorCode::STALE_CAPABILITY: return "STALE_CAPABILITY";
        case ThermalErrorCode::STALE_VERIFICATION: return "STALE_VERIFICATION";
        case ThermalErrorCode::UNKNOWN_DEVICE: return "UNKNOWN_DEVICE";
        case ThermalErrorCode::UNKNOWN_DOMAIN: return "UNKNOWN_DOMAIN";
        case ThermalErrorCode::UNKNOWN_WORKLOAD: return "UNKNOWN_WORKLOAD";
        case ThermalErrorCode::UNKNOWN_NODE: return "UNKNOWN_NODE";
        case ThermalErrorCode::UNKNOWN_RACK: return "UNKNOWN_RACK";
        case ThermalErrorCode::UNKNOWN_ACTION: return "UNKNOWN_ACTION";
        case ThermalErrorCode::UNKNOWN_POLICY: return "UNKNOWN_POLICY";
        case ThermalErrorCode::UNKNOWN_COORDINATOR: return "UNKNOWN_COORDINATOR";
        case ThermalErrorCode::CAPABILITY_UNSUPPORTED: return "CAPABILITY_UNSUPPORTED";
        case ThermalErrorCode::EVIDENCE_UNKNOWN: return "EVIDENCE_UNKNOWN";
        case ThermalErrorCode::EVIDENCE_STALE: return "EVIDENCE_STALE";
        case ThermalErrorCode::EVIDENCE_CONFLICT: return "EVIDENCE_CONFLICT";
        case ThermalErrorCode::TEMPERATURE_INVALID: return "TEMPERATURE_INVALID";
        case ThermalErrorCode::EVIDENCE_DUPLICATE: return "EVIDENCE_DUPLICATE";
        case ThermalErrorCode::POLICY_INVALID: return "POLICY_INVALID";
        case ThermalErrorCode::POLICY_CYCLE: return "POLICY_CYCLE";
        case ThermalErrorCode::POLICY_LIMIT_EXCEEDED: return "POLICY_LIMIT_EXCEEDED";
        case ThermalErrorCode::THERMAL_WARNING: return "THERMAL_WARNING";
        case ThermalErrorCode::THERMAL_DERATING_REQUIRED: return "THERMAL_DERATING_REQUIRED";
        case ThermalErrorCode::THERMAL_CRITICAL: return "THERMAL_CRITICAL";
        case ThermalErrorCode::NO_SAFE_THERMAL_ENVELOPE: return "NO_SAFE_THERMAL_ENVELOPE";
        case ThermalErrorCode::RECOVERY_FORBIDDEN: return "RECOVERY_FORBIDDEN";
        case ThermalErrorCode::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
        case ThermalErrorCode::ACTION_INFEASIBLE: return "ACTION_INFEASIBLE";
        case ThermalErrorCode::ACTION_INEFFECTIVE: return "ACTION_INEFFECTIVE";
        case ThermalErrorCode::ACTION_SUPERSEDED: return "ACTION_SUPERSEDED";
        case ThermalErrorCode::ACTION_CANCELLED: return "ACTION_CANCELLED";
        case ThermalErrorCode::BACKEND_FAILURE: return "BACKEND_FAILURE";
        case ThermalErrorCode::SECONDARY_VIOLATION: return "SECONDARY_VIOLATION";
        case ThermalErrorCode::PERSISTENCE_CORRUPT: return "PERSISTENCE_CORRUPT";
        case ThermalErrorCode::PERSISTENCE_UNSUPPORTED_VERSION: return "PERSISTENCE_UNSUPPORTED_VERSION";
        case ThermalErrorCode::PERSISTENCE_IO: return "PERSISTENCE_IO";
        case ThermalErrorCode::PERSISTENCE_TRUNCATED: return "PERSISTENCE_TRUNCATED";
        case ThermalErrorCode::PERSISTENCE_TRAILING_GARBAGE: return "PERSISTENCE_TRAILING_GARBAGE";
        case ThermalErrorCode::PERSISTENCE_INTEGRITY: return "PERSISTENCE_INTEGRITY";
        case ThermalErrorCode::FRAME_CORRUPT: return "FRAME_CORRUPT";
        case ThermalErrorCode::FRAME_TOO_LARGE: return "FRAME_TOO_LARGE";
        case ThermalErrorCode::FRAME_TRUNCATED: return "FRAME_TRUNCATED";
        case ThermalErrorCode::PROTOCOL_UNSUPPORTED: return "PROTOCOL_UNSUPPORTED";
        case ThermalErrorCode::TRANSPORT_IO: return "TRANSPORT_IO";
        case ThermalErrorCode::TRANSPORT_CLOSED: return "TRANSPORT_CLOSED";
        case ThermalErrorCode::DUPLICATE_CONFLICT: return "DUPLICATE_CONFLICT";
        case ThermalErrorCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
        case ThermalErrorCode::OUTCOME_UNKNOWN: return "OUTCOME_UNKNOWN";
        case ThermalErrorCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case ThermalErrorCode::NOT_IMPLEMENTED: return "NOT_IMPLEMENTED";
        case ThermalErrorCode::INTERNAL: return "INTERNAL";
    }
    return "UNRECOGNISED_ERROR_CODE";
}

/// A typed error value. Cheap to copy, deterministic to render.
struct ThermalError {
    ThermalErrorCode code = ThermalErrorCode::NONE;
    std::string detail;

    ThermalError() = default;
    explicit ThermalError(ThermalErrorCode c, std::string d = {})
        : code(c), detail(std::move(d)) {}

    [[nodiscard]] bool ok() const noexcept { return code == ThermalErrorCode::NONE; }
    [[nodiscard]] std::string render() const;
};

/// Result carrier used for operations that can fail with a typed error.
///
/// Ordinary policy outcomes (ALLOW / DERATE / DENY ...) are *values*, not
/// errors, and travel in the value channel.
///
/// Lifetime note: as with std::expected, value() returns a reference into
/// the carrier. Binding a reference to the result of a function call does
/// not extend the lifetime of the temporary it came from, so
/// "const auto& v = lookup().value();" dangles immediately. Copy the value,
/// keep the carrier alive, or consume it within the same expression.
template <class T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}
    Result(ThermalError error) : storage_(std::move(error)) {}

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }

    [[nodiscard]] const ThermalError& error() const& { return std::get<ThermalError>(storage_); }
    [[nodiscard]] ThermalError& error() & { return std::get<ThermalError>(storage_); }

    [[nodiscard]] T value_or(T fallback) const {
        return has_value() ? std::get<T>(storage_) : std::move(fallback);
    }

private:
    std::variant<T, ThermalError> storage_;
};

/// Result carrier for operations with no payload.
class Status {
public:
    Status() = default;
    Status(ThermalError error) : error_(std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const ThermalError& error() const noexcept { return error_; }
    [[nodiscard]] ThermalErrorCode code() const noexcept { return error_.code; }

    static Status success() { return Status{}; }
    static Status failure(ThermalErrorCode code, std::string detail = {}) {
        return Status{ThermalError{code, std::move(detail)}};
    }

private:
    ThermalError error_{};
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_ERROR_HPP
