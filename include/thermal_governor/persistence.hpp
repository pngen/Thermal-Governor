// Thermal Governor — versioned, integrity-checked durable state.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_PERSISTENCE_HPP
#define THERMAL_GOVERNOR_PERSISTENCE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "thermal_governor/action.hpp"
#include "thermal_governor/coupling.hpp"
#include "thermal_governor/domain.hpp"
#include "thermal_governor/error.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/policy.hpp"
#include "thermal_governor/time.hpp"
#include "thermal_governor/verification.hpp"

namespace thermal_governor {

/// Container magic: "TGOVPERS".
inline constexpr std::uint64_t kPersistenceMagic = 0x5352455056474F54ULL;

/// Per-record type tags.
enum class PersistenceRecordType : std::uint16_t {
    COORDINATOR_STATE = 1,
    POLICY = 2,
    DOMAIN_DEFINITION = 3,
    COUPLING = 4,
    STATE_TRANSITION = 5,
    ACTION_RECORD = 6,
    VERIFICATION_RECORD = 7,
    OPERATOR_CONFIG = 8,
    DEVICE_REGISTRATION = 9,
};

/// A durable historical thermal state transition.
struct StateTransitionRecord {
    ThermalDomainId domain{};
    ThermalDomainGeneration domain_generation{};
    ThermalState from = ThermalState::UNKNOWN;
    ThermalState to = ThermalState::UNKNOWN;
    DeratingLevel derating_from = DeratingLevel::UNKNOWN;
    DeratingLevel derating_to = DeratingLevel::UNKNOWN;
    ThermalDecision decision = ThermalDecision::UNKNOWN;
    DegreesCelsius temperature{};
    ThermalPolicyGeneration policy_generation{};
    TelemetryGeneration telemetry_generation{};
    CoordinatorEpoch coordinator_epoch{};
    std::uint64_t sequence = 0;
};

/// Bounds applied to every decoded structure. Untrusted bytes reach the
/// decoder, so every count and length is arithmetic-checked.
struct PersistenceLimits {
    std::size_t max_file_bytes = 64U * 1024U * 1024U;
    std::size_t max_records = 2'000'000;
    std::size_t max_record_bytes = 4U * 1024U * 1024U;
    std::size_t max_string_bytes = 64U * 1024U;
    std::size_t max_policies = 4096;
    std::size_t max_devices = 100000;
    std::size_t max_domains = 100000;
    std::size_t max_couplings = 200000;
    std::size_t max_transitions = 1'000'000;
    std::size_t max_actions = 500000;
    std::size_t max_verifications = 500000;
};

/// The complete durable state owned by Thermal Governor.
///
/// Dynamic telemetry is deliberately absent: it must never silently become
/// fresh merely because a coordinator restarted.
struct DurableState {
    std::uint32_t format_version = 0;
    CoordinatorId coordinator{};
    CoordinatorEpoch coordinator_epoch{};
    ThermalPolicyGeneration policy_generation{};
    TopologyGeneration topology_generation{};
    std::uint64_t sequence = 0;

    std::vector<ThermalPolicy> policies;
    std::vector<DeviceRegistration> devices;
    std::vector<ThermalDomainDefinition> domains;
    std::vector<CouplingRelation> couplings;
    std::vector<StateTransitionRecord> transitions;
    std::vector<ThermalAction> actions;
    std::vector<ActionVerification> verifications;
};

/// Result of decoding a durable container, including what was preserved.
struct PersistenceLoadResult {
    DurableState state;
    std::uint64_t records_read = 0;
    std::size_t bytes_read = 0;
};

/// Encode durable state into a self-describing, integrity-checked buffer.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_durable_state(
    const DurableState& state, const PersistenceLimits& limits = {});

/// Decode a durable container. Rejects bad magic, unsupported versions,
/// truncation, trailing garbage, integrity mismatch, absurd counts,
/// invalid enumerations and duplicate identities where duplication is
/// illegal.
[[nodiscard]] Result<PersistenceLoadResult> decode_durable_state(
    const std::vector<std::uint8_t>& bytes, const PersistenceLimits& limits = {});

/// Transactional durable store.
///
/// save(): serialise -> write temporary file -> flush -> close -> safely
/// replace the authoritative file -> remove temporary state. An intended
/// durable mutation is never acknowledged before the replacement completes.
class DurableStore {
public:
    explicit DurableStore(std::string path, PersistenceLimits limits = {})
        : path_(std::move(path)), limits_(limits) {}

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    [[nodiscard]] Status save(const DurableState& state);
    [[nodiscard]] Result<PersistenceLoadResult> load() const;

    /// True when the authoritative file exists.
    [[nodiscard]] bool exists() const;

    /// Remove the authoritative file and any temporary sibling.
    [[nodiscard]] Status remove_all();

    /// Windows-correct replacement of the authoritative file. Uses
    /// ReplaceFileW semantics when available and falls back to a
    /// same-volume MoveFileEx replace.
    [[nodiscard]] static Status atomic_replace(const std::string& temp_path,
                                               const std::string& target_path);

    [[nodiscard]] const PersistenceLimits& limits() const noexcept { return limits_; }

private:
    std::string path_;
    PersistenceLimits limits_;
};

/// CRC-32C (Castagnoli), used for persistence and frame integrity.
[[nodiscard]] std::uint32_t crc32c(const std::uint8_t* data, std::size_t length,
                                   std::uint32_t seed = 0) noexcept;

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_PERSISTENCE_HPP
