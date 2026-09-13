// Thermal Governor — explicit thermal coupling relationships.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_COUPLING_HPP
#define THERMAL_GOVERNOR_COUPLING_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "thermal_governor/identity.hpp"
#include "thermal_governor/provenance.hpp"
#include "thermal_governor/time.hpp"

namespace thermal_governor {

/// Relationship type between two thermal domains.
///
/// Real physical coupling is never inferred from proximity alone.
enum class CouplingType : std::uint8_t {
    UNKNOWN_COUPLING = 0,
    SHARES_CHASSIS,
    SHARES_AIRFLOW_PATH,
    SHARES_COOLING_ZONE,
    SHARES_HEATSINK,
    THERMALLY_COUPLED,
    SYNTHETIC_COUPLING,
};

[[nodiscard]] constexpr std::string_view to_string(CouplingType t) noexcept {
    switch (t) {
        case CouplingType::UNKNOWN_COUPLING: return "UNKNOWN_COUPLING";
        case CouplingType::SHARES_CHASSIS: return "SHARES_CHASSIS";
        case CouplingType::SHARES_AIRFLOW_PATH: return "SHARES_AIRFLOW_PATH";
        case CouplingType::SHARES_COOLING_ZONE: return "SHARES_COOLING_ZONE";
        case CouplingType::SHARES_HEATSINK: return "SHARES_HEATSINK";
        case CouplingType::THERMALLY_COUPLED: return "THERMALLY_COUPLED";
        case CouplingType::SYNTHETIC_COUPLING: return "SYNTHETIC_COUPLING";
    }
    return "UNRECOGNISED_COUPLING_TYPE";
}

/// How far thermal pressure propagates from a hot domain.
/// The enumerators deliberately avoid the names NONE and DOMAIN, which the
/// platform C mathematics headers define as macros.
enum class PropagationClass : std::uint8_t {
    NO_PROPAGATION = 0,
    LOCAL,
    DOMAIN_WIDE,
    COUPLED_DOMAIN,
    UNKNOWN,
};

[[nodiscard]] constexpr std::string_view to_string(PropagationClass c) noexcept {
    switch (c) {
        case PropagationClass::NO_PROPAGATION: return "NONE";
        case PropagationClass::LOCAL: return "LOCAL";
        case PropagationClass::DOMAIN_WIDE: return "DOMAIN";
        case PropagationClass::COUPLED_DOMAIN: return "COUPLED_DOMAIN";
        case PropagationClass::UNKNOWN: return "UNKNOWN";
    }
    return "UNRECOGNISED_PROPAGATION_CLASS";
}

/// A directed coupling edge between two thermal domains.
struct CouplingRelation {
    CouplingId id{};
    ThermalDomainId source{};
    ThermalDomainId destination{};
    CouplingType type = CouplingType::UNKNOWN_COUPLING;
    CouplingGeneration generation{};
    Provenance provenance = Provenance::UNKNOWN;
    std::string evidence_source;

    /// Relative strength in [0, 1] used to scale propagated pressure. This
    /// is a policy input, not a physical measurement.
    double weight = 1.0;

    /// When true, pressure propagates source -> destination only: the source
    bool directed = true;

    /// Optional expiry for dynamic coupling observations.
    std::optional<SteadyTimePoint> valid_until;

    friend bool operator==(const CouplingRelation&, const CouplingRelation&) = default;
};

/// Deterministic coupling graph with cycle tolerance.
class CouplingGraph {
public:
    void add(CouplingRelation relation);
    [[nodiscard]] bool remove(CouplingId id);

    [[nodiscard]] const std::vector<CouplingRelation>& edges() const noexcept { return edges_; }

    /// Neighbours reachable outward from a domain, breadth-first, ordered by
    /// (depth, domain id) so results are deterministic regardless of
    /// insertion order. Cycles terminate.
    [[nodiscard]] std::vector<std::pair<ThermalDomainId, std::uint32_t>> reachable_from(
        ThermalDomainId origin, std::uint32_t max_depth) const;

    /// Neighbours whose thermal pressure reaches the origin.
    ///
    /// Traversal follows edges backwards, because an edge
    /// source -> destination means the source heats the destination. A
    /// domain is therefore pressured by the domains that point at it.
    [[nodiscard]] std::vector<std::pair<ThermalDomainId, std::uint32_t>> incoming_reachable(
        ThermalDomainId origin, std::uint32_t max_depth) const;

    /// True when the graph contains a cycle.
    [[nodiscard]] bool has_cycle() const;

    [[nodiscard]] std::size_t size() const noexcept { return edges_.size(); }

private:
    std::vector<CouplingRelation> edges_;
};

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_COUPLING_HPP
