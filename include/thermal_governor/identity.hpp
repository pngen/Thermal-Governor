// Thermal Governor — strongly typed identities.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_IDENTITY_HPP
#define THERMAL_GOVERNOR_IDENTITY_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

namespace thermal_governor {

/// Compile-time-tagged scalar identity.
///
/// Distinct tags make accidental cross-domain mixing (a DeviceId where a
/// ThermalDomainId is expected) a compile error rather than a silent bug.
template <class Tag, class Rep = std::uint64_t>
class StrongId {
public:
    using tag_type = Tag;
    using rep_type = Rep;

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(Rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != Rep{}; }

    friend constexpr bool operator==(StrongId, StrongId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(StrongId, StrongId) noexcept = default;

private:
    Rep value_{};
};

#define TG_DECLARE_ID(Name)                       \
    struct Name##Tag {};                          \
    using Name = StrongId<Name##Tag>

#define TG_DECLARE_GENERATION(Name)               \
    struct Name##Tag {};                          \
    using Name = StrongId<Name##Tag>

// --- Authority identities -------------------------------------------------
TG_DECLARE_ID(CoordinatorId);
TG_DECLARE_ID(WorkerId);
TG_DECLARE_ID(WorkloadId);
TG_DECLARE_ID(ExecutionId);
TG_DECLARE_ID(ActionId);
TG_DECLARE_ID(RestrictionId);

// --- Physical / modelled topology ----------------------------------------
TG_DECLARE_ID(DeviceId);
TG_DECLARE_ID(AcceleratorId);
TG_DECLARE_ID(NodeId);
TG_DECLARE_ID(RackId);
TG_DECLARE_ID(CoolingZoneId);
TG_DECLARE_ID(ChassisId);
TG_DECLARE_ID(ThermalDomainId);
TG_DECLARE_ID(ThermalPolicyId);
TG_DECLARE_ID(EvidenceId);
TG_DECLARE_ID(CouplingId);

// --- Generations ----------------------------------------------------------
TG_DECLARE_GENERATION(CoordinatorEpoch);
TG_DECLARE_GENERATION(WorkerBootId);
TG_DECLARE_GENERATION(DeviceGeneration);
TG_DECLARE_GENERATION(NodeGeneration);
TG_DECLARE_GENERATION(RackGeneration);
TG_DECLARE_GENERATION(ThermalDomainGeneration);
TG_DECLARE_GENERATION(ThermalPolicyGeneration);
TG_DECLARE_GENERATION(TelemetryGeneration);
TG_DECLARE_GENERATION(ActionGeneration);
TG_DECLARE_GENERATION(VerificationGeneration);
TG_DECLARE_GENERATION(CapabilityGeneration);
TG_DECLARE_GENERATION(HealthGeneration);
TG_DECLARE_GENERATION(TopologyGeneration);
TG_DECLARE_GENERATION(RestrictionGeneration);
TG_DECLARE_GENERATION(RecoveryGeneration);
TG_DECLARE_GENERATION(CouplingGeneration);

#undef TG_DECLARE_ID
#undef TG_DECLARE_GENERATION

/// Monotonic generation counter with explicit advance-only semantics.
template <class Id>
class GenerationCounter {
public:
    using id_type = Id;

    constexpr GenerationCounter() noexcept = default;
    constexpr explicit GenerationCounter(Id current) noexcept : current_(current) {}

    [[nodiscard]] constexpr Id current() const noexcept { return current_; }

    /// Advance to the next generation. Returns the new current generation.
    constexpr Id advance() noexcept {
        current_ = Id{current_.value() + 1};
        return current_;
    }

    /// Adopt an observed generation only when it is strictly newer.
    /// Returns true when the counter advanced.
    constexpr bool observe(Id observed) noexcept {
        if (observed.value() > current_.value()) {
            current_ = observed;
            return true;
        }
        return false;
    }

private:
    Id current_{};
};

/// Stable, human-readable label. Never used as a semantic identity.
class Label {
public:
    Label() = default;
    explicit Label(std::string text) : text_(std::move(text)) {}

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
    [[nodiscard]] std::string_view view() const noexcept { return text_; }

    friend bool operator==(const Label&, const Label&) = default;
    friend auto operator<=>(const Label&, const Label&) = default;

private:
    std::string text_;
};

}  // namespace thermal_governor

namespace std {

template <class Tag, class Rep>
struct hash<thermal_governor::StrongId<Tag, Rep>> {
    [[nodiscard]] size_t operator()(const thermal_governor::StrongId<Tag, Rep>& id) const noexcept {
        return std::hash<Rep>{}(id.value());
    }
};

}  // namespace std

#endif  // THERMAL_GOVERNOR_IDENTITY_HPP
