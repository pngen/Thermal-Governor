// Thermal Governor — generation-bound thermal evidence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef THERMAL_GOVERNOR_EVIDENCE_HPP
#define THERMAL_GOVERNOR_EVIDENCE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "thermal_governor/identity.hpp"
#include "thermal_governor/provenance.hpp"
#include "thermal_governor/quantity.hpp"
#include "thermal_governor/throttle.hpp"
#include "thermal_governor/time.hpp"

namespace thermal_governor {

/// What kind of thing an observation is about.
enum class SubjectKind : std::uint8_t {
    DEVICE = 0,
    NODE,
    RACK,
    COOLING_ZONE,
    CHASSIS,
    THERMAL_DOMAIN,
};

[[nodiscard]] constexpr std::string_view to_string(SubjectKind k) noexcept {
    switch (k) {
        case SubjectKind::DEVICE: return "DEVICE";
        case SubjectKind::NODE: return "NODE";
        case SubjectKind::RACK: return "RACK";
        case SubjectKind::COOLING_ZONE: return "COOLING_ZONE";
        case SubjectKind::CHASSIS: return "CHASSIS";
        case SubjectKind::THERMAL_DOMAIN: return "THERMAL_DOMAIN";
    }
    return "UNRECOGNISED_SUBJECT_KIND";
}

/// A typed reference to the subject of an observation. Exactly one identity
/// family is populated, selected by the kind discriminator.
struct SubjectRef {
    SubjectKind kind = SubjectKind::DEVICE;

    DeviceId device{};
    DeviceGeneration device_generation{};

    NodeId node{};
    NodeGeneration node_generation{};

    RackId rack{};
    RackGeneration rack_generation{};

    CoolingZoneId cooling_zone{};
    ChassisId chassis{};

    ThermalDomainId domain{};
    ThermalDomainGeneration domain_generation{};

    [[nodiscard]] static SubjectRef for_device(DeviceId id, DeviceGeneration gen) noexcept {
        SubjectRef r;
        r.kind = SubjectKind::DEVICE;
        r.device = id;
        r.device_generation = gen;
        return r;
    }
    [[nodiscard]] static SubjectRef for_node(NodeId id, NodeGeneration gen) noexcept {
        SubjectRef r;
        r.kind = SubjectKind::NODE;
        r.node = id;
        r.node_generation = gen;
        return r;
    }
    [[nodiscard]] static SubjectRef for_rack(RackId id, RackGeneration gen) noexcept {
        SubjectRef r;
        r.kind = SubjectKind::RACK;
        r.rack = id;
        r.rack_generation = gen;
        return r;
    }
    [[nodiscard]] static SubjectRef for_cooling_zone(CoolingZoneId id) noexcept {
        SubjectRef r;
        r.kind = SubjectKind::COOLING_ZONE;
        r.cooling_zone = id;
        return r;
    }
    [[nodiscard]] static SubjectRef for_chassis(ChassisId id) noexcept {
        SubjectRef r;
        r.kind = SubjectKind::CHASSIS;
        r.chassis = id;
        return r;
    }
    [[nodiscard]] static SubjectRef for_domain(ThermalDomainId id,
                                               ThermalDomainGeneration gen) noexcept {
        SubjectRef r;
        r.kind = SubjectKind::THERMAL_DOMAIN;
        r.domain = id;
        r.domain_generation = gen;
        return r;
    }

    /// Stable identity key used for indexing and for duplicate detection.
    [[nodiscard]] std::string key() const;

    friend bool operator==(const SubjectRef&, const SubjectRef&) = default;
};

/// Optional cooling evidence. Absence of cooling evidence is never treated
/// as zero cooling: it is UNSUPPORTED or UNKNOWN, explicitly.
struct CoolingEvidence {
    CoolingZoneId zone{};
    std::optional<DegreesCelsius> air_inlet;
    std::optional<DegreesCelsius> air_outlet;
    std::optional<DegreesCelsius> liquid_inlet;
    std::optional<DegreesCelsius> liquid_outlet;
    std::optional<double> coolant_flow_litres_per_minute;
    Provenance provenance = Provenance::UNKNOWN;

    [[nodiscard]] bool has_any() const noexcept {
        return air_inlet.has_value() || air_outlet.has_value() || liquid_inlet.has_value() ||
               liquid_outlet.has_value() || coolant_flow_litres_per_minute.has_value();
    }

    friend bool operator==(const CoolingEvidence&, const CoolingEvidence&) = default;
};

/// Optional fan telemetry. Present only when a backend genuinely exposes it.
struct FanEvidence {
    std::uint32_t fan_count = 0;
    std::optional<double> speed_percent;
    std::optional<std::uint32_t> speed_rpm;
    Provenance provenance = Provenance::UNKNOWN;

    [[nodiscard]] bool has_any() const noexcept {
        return speed_percent.has_value() || speed_rpm.has_value();
    }

    friend bool operator==(const FanEvidence&, const FanEvidence&) = default;
};

/// Freshness classification of a piece of evidence relative to a policy and
/// an evaluation instant. Never inferred from arrival order.
enum class FreshnessState : std::uint8_t {
    UNKNOWN = 0,
    FRESH,
    STALE,
    GENERATION_INVALIDATED,
};

[[nodiscard]] constexpr std::string_view to_string(FreshnessState s) noexcept {
    switch (s) {
        case FreshnessState::UNKNOWN: return "UNKNOWN";
        case FreshnessState::FRESH: return "FRESH";
        case FreshnessState::STALE: return "STALE";
        case FreshnessState::GENERATION_INVALIDATED: return "GENERATION_INVALIDATED";
    }
    return "UNRECOGNISED_FRESHNESS";
}

/// A single temperature observation with full authority metadata.
///
/// Temperature is evidence, not authority. This record carries everything
/// needed to decide whether that evidence is admissible now.
struct ThermalEvidence {
    EvidenceId evidence_id{};

    SubjectRef subject{};
    ThermalDomainId domain{};
    ThermalDomainGeneration domain_generation{};

    DegreesCelsius temperature{};

    MeasurementSource source = MeasurementSource::UNKNOWN;
    Provenance provenance = Provenance::UNKNOWN;

    std::uint64_t measurement_sequence = 0;
    TelemetryGeneration telemetry_generation{};

    WorkerId worker{};
    WorkerBootId worker_boot{};

    CoordinatorEpoch coordinator_epoch{};

    SteadyTimePoint measured_at{};
    WallTimePoint measured_wall_at{};

    CapabilityGeneration capability_generation{};
    TopologyGeneration topology_generation{};

    IntegrityStatus integrity = IntegrityStatus::OK;

    // Optional secondary evidence.
    std::optional<ThrottleObservation> throttle;
    std::optional<DegreesCelsius> temperature_limit;
    std::optional<DegreesCelsius> shutdown_limit;
    std::optional<MegaHertz> current_clock;
    std::optional<MegaHertz> max_clock;
    std::optional<CoolingEvidence> cooling;
    std::optional<FanEvidence> fan;
    std::optional<double> confidence;

    /// Classification assigned at ingestion time by the evaluator; not part
    /// of the wire payload authority metadata.
    FreshnessState freshness = FreshnessState::UNKNOWN;

    [[nodiscard]] bool has_throttle() const noexcept { return throttle.has_value(); }
};

/// Result of comparing a newly received evidence record against the record
/// currently held for the same subject and generation position.
enum class EvidenceComparison : std::uint8_t {
    /// Different evidence identity: an ordinary new observation.
    INDEPENDENT = 0,
    /// Equal measurement content: idempotent duplicate.
    IDENTICAL_DUPLICATE,
    /// Same identity and sequence but divergent measurement content.
    CONFLICTING_DUPLICATE,
    /// Strictly older evidence than the one currently held.
    SUPERSEDED,
};

[[nodiscard]] constexpr std::string_view to_string(EvidenceComparison c) noexcept {
    switch (c) {
        case EvidenceComparison::INDEPENDENT: return "INDEPENDENT";
        case EvidenceComparison::IDENTICAL_DUPLICATE: return "IDENTICAL_DUPLICATE";
        case EvidenceComparison::CONFLICTING_DUPLICATE: return "CONFLICTING_DUPLICATE";
        case EvidenceComparison::SUPERSEDED: return "SUPERSEDED";
    }
    return "UNRECOGNISED_COMPARISON";
}

/// Structural validation of an evidence record, before any freshness or
/// generation check. Untrusted input reaches the runtime through here.
[[nodiscard]] Status validate_evidence_structure(const ThermalEvidence& evidence);

/// Compare two evidence records for the same subject position.
[[nodiscard]] EvidenceComparison compare_evidence(const ThermalEvidence& held,
                                                  const ThermalEvidence& incoming);

/// Deterministic canonical rendering used for duplicate detection and for
/// explanation output.
[[nodiscard]] std::string canonical_evidence_digest(const ThermalEvidence& evidence);

}  // namespace thermal_governor

#endif  // THERMAL_GOVERNOR_EVIDENCE_HPP
