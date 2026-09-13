// Thermal Governor — typed mitigation intents.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_MITIGATION_HPP
#define THERMAL_GOVERNOR_MITIGATION_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "thermal_governor/identity.hpp"
#include "thermal_governor/quantity.hpp"

namespace thermal_governor {

/// Typed intents emitted toward adjacent systems.
///
/// These are *intents*, not ownership. Thermal Governor owns the thermal
/// decision; unless it legitimately owns an enforcement backend it does not
/// absorb the scheduler, the power manager, the migrator or containment.
enum class MitigationIntent : std::uint8_t {
    NO_ACTION = 0,
    REQUEST_CLOCK_REDUCTION,
    REQUEST_CLOCK_RESTORE,
    REQUEST_POWER_REDUCTION,
    REQUEST_POWER_RESTORE,
    REQUEST_ADMISSION_REDUCTION,
    REQUEST_ADMISSION_RESTORE,
    REQUEST_CONCURRENCY_REDUCTION,
    REQUEST_CONCURRENCY_RESTORE,
    REQUEST_WORKLOAD_MIGRATION,
    REQUEST_CONTAINMENT,
    REQUEST_COOLING_INTERVENTION,
};

inline constexpr std::uint32_t kMitigationIntentCount = 12;

[[nodiscard]] constexpr std::string_view to_string(MitigationIntent i) noexcept {
    switch (i) {
        case MitigationIntent::NO_ACTION: return "NO_ACTION";
        case MitigationIntent::REQUEST_CLOCK_REDUCTION: return "REQUEST_CLOCK_REDUCTION";
        case MitigationIntent::REQUEST_CLOCK_RESTORE: return "REQUEST_CLOCK_RESTORE";
        case MitigationIntent::REQUEST_POWER_REDUCTION: return "REQUEST_POWER_REDUCTION";
        case MitigationIntent::REQUEST_POWER_RESTORE: return "REQUEST_POWER_RESTORE";
        case MitigationIntent::REQUEST_ADMISSION_REDUCTION: return "REQUEST_ADMISSION_REDUCTION";
        case MitigationIntent::REQUEST_ADMISSION_RESTORE: return "REQUEST_ADMISSION_RESTORE";
        case MitigationIntent::REQUEST_CONCURRENCY_REDUCTION: return "REQUEST_CONCURRENCY_REDUCTION";
        case MitigationIntent::REQUEST_CONCURRENCY_RESTORE: return "REQUEST_CONCURRENCY_RESTORE";
        case MitigationIntent::REQUEST_WORKLOAD_MIGRATION: return "REQUEST_WORKLOAD_MIGRATION";
        case MitigationIntent::REQUEST_CONTAINMENT: return "REQUEST_CONTAINMENT";
        case MitigationIntent::REQUEST_COOLING_INTERVENTION: return "REQUEST_COOLING_INTERVENTION";
    }
    return "UNRECOGNISED_MITIGATION_INTENT";
}

[[nodiscard]] constexpr std::uint32_t intent_bit(MitigationIntent i) noexcept {
    return 1U << static_cast<std::uint32_t>(i);
}

/// True when the intent relaxes a restriction rather than tightening one.
[[nodiscard]] constexpr bool is_restoring_intent(MitigationIntent i) noexcept {
    switch (i) {
        case MitigationIntent::REQUEST_CLOCK_RESTORE:
        case MitigationIntent::REQUEST_POWER_RESTORE:
        case MitigationIntent::REQUEST_ADMISSION_RESTORE:
        case MitigationIntent::REQUEST_CONCURRENCY_RESTORE:
            return true;
        default:
            return false;
    }
}

/// A concrete, generation-bound mitigation instruction.
struct MitigationIntentRecord {
    MitigationIntent intent = MitigationIntent::NO_ACTION;
    ThermalDomainId domain{};
    ThermalDomainGeneration domain_generation{};
    DeviceId device{};
    DeviceGeneration device_generation{};
    RestrictionId restriction{};
    RestrictionGeneration restriction_generation{};

    /// Requested clock ceiling when the intent is a clock reduction.
    std::optional<MegaHertz> clock_ceiling;
    /// Requested concurrency / admission ceiling as a percentage.
    std::optional<Percent> concurrency_ceiling;
    std::optional<Percent> admission_ceiling;

    std::string rationale;
};

/// Canonical ordered set of intents produced by one evaluation.
///
/// Ordering is by intent enumeration value, so two evaluations over equal
/// inputs render byte-identical intent lists.
class MitigationIntentSet {
public:
    MitigationIntentSet() = default;

    void add(MitigationIntentRecord record) {
        records_.push_back(std::move(record));
    }
    void add(MitigationIntent intent, ThermalDomainId domain, std::string rationale) {
        MitigationIntentRecord r;
        r.intent = intent;
        r.domain = domain;
        r.rationale = std::move(rationale);
        add(std::move(r));
    }

    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }
    [[nodiscard]] const std::vector<MitigationIntentRecord>& records() const noexcept {
        return records_;
    }

    /// Sort into canonical order and drop exact duplicates.
    void canonicalise();

    [[nodiscard]] bool contains(MitigationIntent intent) const noexcept;

private:
    std::vector<MitigationIntentRecord> records_;
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_MITIGATION_HPP
