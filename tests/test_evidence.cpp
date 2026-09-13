// Thermal Governor — thermal evidence structure and comparison tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "framework.hpp"

#include "thermal_governor/evidence.hpp"
#include "thermal_governor/error.hpp"
#include "thermal_governor/identity.hpp"
#include "thermal_governor/provenance.hpp"
#include "thermal_governor/quantity.hpp"
#include "thermal_governor/throttle.hpp"

using thermal_governor::CapabilityGeneration;
using thermal_governor::ChassisId;
using thermal_governor::CoolingEvidence;
using thermal_governor::CoolingZoneId;
using thermal_governor::CoordinatorEpoch;
using thermal_governor::DegreesCelsius;
using thermal_governor::DeviceGeneration;
using thermal_governor::DeviceId;
using thermal_governor::EvidenceComparison;
using thermal_governor::EvidenceId;
using thermal_governor::IntegrityStatus;
using thermal_governor::MeasurementSource;
using thermal_governor::NodeGeneration;
using thermal_governor::NodeId;
using thermal_governor::Provenance;
using thermal_governor::RackGeneration;
using thermal_governor::RackId;
using thermal_governor::StrongId;
using thermal_governor::SubjectKind;
using thermal_governor::SubjectRef;
using thermal_governor::TelemetryGeneration;
using thermal_governor::ThermalDomainGeneration;
using thermal_governor::ThermalDomainId;
using thermal_governor::ThermalErrorCode;
using thermal_governor::ThermalEvidence;
using thermal_governor::TopologyGeneration;
using thermal_governor::WorkerBootId;
using thermal_governor::WorkerId;
using thermal_governor::canonical_evidence_digest;
using thermal_governor::compare_evidence;
using thermal_governor::validate_evidence_structure;

namespace {

constexpr std::uint64_t kDevice = 11;
constexpr std::uint64_t kDomain = 5;

[[nodiscard]] SubjectRef complete_subject(SubjectKind kind) {
    switch (kind) {
        case SubjectKind::DEVICE:
            return SubjectRef::for_device(
                DeviceId{StrongId<thermal_governor::DeviceIdTag>{kDevice}},
                DeviceGeneration{StrongId<thermal_governor::DeviceGenerationTag>{3}});
        case SubjectKind::NODE:
            return SubjectRef::for_node(
                NodeId{StrongId<thermal_governor::NodeIdTag>{4}},
                NodeGeneration{StrongId<thermal_governor::NodeGenerationTag>{2}});
        case SubjectKind::RACK:
            return SubjectRef::for_rack(
                RackId{StrongId<thermal_governor::RackIdTag>{7}},
                RackGeneration{StrongId<thermal_governor::RackGenerationTag>{1}});
        case SubjectKind::COOLING_ZONE:
            return SubjectRef::for_cooling_zone(
                CoolingZoneId{StrongId<thermal_governor::CoolingZoneIdTag>{9}});
        case SubjectKind::CHASSIS:
            return SubjectRef::for_chassis(ChassisId{StrongId<thermal_governor::ChassisIdTag>{2}});
        case SubjectKind::THERMAL_DOMAIN:
            return SubjectRef::for_domain(
                ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{kDomain}},
                ThermalDomainGeneration{StrongId<thermal_governor::ThermalDomainGenerationTag>{6}});
    }
    return SubjectRef{};
}

/// The same kind, with the identity its kind requires deliberately missing.
[[nodiscard]] SubjectRef incomplete_subject(SubjectKind kind) {
    SubjectRef subject;
    subject.kind = kind;
    switch (kind) {
        case SubjectKind::DEVICE:
            subject.device = DeviceId{StrongId<thermal_governor::DeviceIdTag>{kDevice}};
            break;
        case SubjectKind::NODE:
            subject.node = NodeId{StrongId<thermal_governor::NodeIdTag>{4}};
            break;
        case SubjectKind::RACK:
            subject.rack = RackId{StrongId<thermal_governor::RackIdTag>{7}};
            break;
        case SubjectKind::COOLING_ZONE:
            break;
        case SubjectKind::CHASSIS:
            break;
        case SubjectKind::THERMAL_DOMAIN:
            subject.domain = ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{kDomain}};
            break;
    }
    return subject;
}

[[nodiscard]] ThermalEvidence valid_evidence() {
    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<thermal_governor::EvidenceIdTag>{7}};
    evidence.subject = complete_subject(SubjectKind::DEVICE);
    evidence.domain = ThermalDomainId{StrongId<thermal_governor::ThermalDomainIdTag>{kDomain}};
    evidence.domain_generation =
        ThermalDomainGeneration{StrongId<thermal_governor::ThermalDomainGenerationTag>{1}};
    evidence.temperature = DegreesCelsius{72.5};
    evidence.source = MeasurementSource::NVML_GPU_TEMPERATURE;
    evidence.provenance = Provenance::REAL;
    evidence.measurement_sequence = 2;
    evidence.telemetry_generation =
        TelemetryGeneration{StrongId<thermal_governor::TelemetryGenerationTag>{9}};
    evidence.worker = WorkerId{StrongId<thermal_governor::WorkerIdTag>{3}};
    evidence.worker_boot = WorkerBootId{StrongId<thermal_governor::WorkerBootIdTag>{4}};
    evidence.coordinator_epoch =
        CoordinatorEpoch{StrongId<thermal_governor::CoordinatorEpochTag>{5}};
    evidence.capability_generation =
        CapabilityGeneration{StrongId<thermal_governor::CapabilityGenerationTag>{1}};
    evidence.topology_generation =
        TopologyGeneration{StrongId<thermal_governor::TopologyGenerationTag>{1}};
    return evidence;
}

}  // namespace

TG_CASE(evidence, accepts_a_complete_record) {
    const ThermalEvidence evidence = valid_evidence();
    TG_STATUS_OK(validate_evidence_structure(evidence));
    TG_CHECK(evidence.temperature.is_valid());
    TG_CHECK(evidence.subject.key() == valid_evidence().subject.key());
    TG_CHECK(!evidence.subject.key().empty());
}

TG_CASE(evidence, accepts_every_subject_kind) {
    const SubjectKind kinds[] = {SubjectKind::DEVICE,       SubjectKind::NODE,
                                 SubjectKind::RACK,         SubjectKind::COOLING_ZONE,
                                 SubjectKind::CHASSIS,      SubjectKind::THERMAL_DOMAIN};
    for (const SubjectKind kind : kinds) {
        ThermalEvidence evidence = valid_evidence();
        evidence.subject = complete_subject(kind);
        TG_CHECK_EQ(evidence.subject.kind, kind);
        TG_STATUS_OK(validate_evidence_structure(evidence));
        TG_CHECK(!evidence.subject.key().empty());
    }
}

TG_CASE(evidence, rejects_invalid_temperature) {
    ThermalEvidence evidence = valid_evidence();
    evidence.temperature = DegreesCelsius{-300.0};
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::TEMPERATURE_INVALID);

    evidence = valid_evidence();
    evidence.temperature = DegreesCelsius{2000.0};
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::TEMPERATURE_INVALID);

    evidence = valid_evidence();
    evidence.temperature_limit = DegreesCelsius{-400.0};
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::TEMPERATURE_INVALID);

    evidence = valid_evidence();
    evidence.shutdown_limit = DegreesCelsius{4000.0};
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::TEMPERATURE_INVALID);

    evidence = valid_evidence();
    evidence.temperature_limit = DegreesCelsius{95.0};
    evidence.shutdown_limit = DegreesCelsius{105.0};
    TG_STATUS_OK(validate_evidence_structure(evidence));
}

TG_CASE(evidence, rejects_incomplete_subject_identity) {
    const SubjectKind kinds[] = {SubjectKind::DEVICE,       SubjectKind::NODE,
                                 SubjectKind::RACK,         SubjectKind::COOLING_ZONE,
                                 SubjectKind::CHASSIS,      SubjectKind::THERMAL_DOMAIN};
    for (const SubjectKind kind : kinds) {
        ThermalEvidence evidence = valid_evidence();
        evidence.subject = incomplete_subject(kind);
        TG_CHECK_EQ(evidence.subject.kind, kind);
        TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence),
                             ThermalErrorCode::INVALID_ARGUMENT);
    }

    TG_PHASE("a default constructed subject is never complete");
    ThermalEvidence evidence = valid_evidence();
    evidence.subject = SubjectRef{};
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);
}

TG_CASE(evidence, rejects_zero_generations_and_epoch) {
    ThermalEvidence evidence = valid_evidence();
    evidence.telemetry_generation = TelemetryGeneration{};
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);

    evidence = valid_evidence();
    evidence.coordinator_epoch = CoordinatorEpoch{};
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);
}

TG_CASE(evidence, rejects_half_supplied_worker_identity) {
    ThermalEvidence evidence = valid_evidence();
    evidence.worker_boot = WorkerBootId{};
    TG_CHECK(evidence.worker.is_valid());
    TG_CHECK(!evidence.worker_boot.is_valid());
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);

    evidence = valid_evidence();
    evidence.worker = WorkerId{};
    TG_CHECK(!evidence.worker.is_valid());
    TG_CHECK(evidence.worker_boot.is_valid());
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);

    TG_PHASE("both absent is acceptable");
    evidence = valid_evidence();
    evidence.worker = WorkerId{};
    evidence.worker_boot = WorkerBootId{};
    TG_STATUS_OK(validate_evidence_structure(evidence));
}

TG_CASE(evidence, rejects_confidence_outside_unit_interval) {
    ThermalEvidence evidence = valid_evidence();
    evidence.confidence = 1.25;
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);

    evidence = valid_evidence();
    evidence.confidence = -0.25;
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);

    evidence = valid_evidence();
    evidence.confidence = std::numeric_limits<double>::quiet_NaN();
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);

    TG_PHASE("the closed interval bounds are accepted");
    evidence = valid_evidence();
    evidence.confidence = 0.0;
    TG_STATUS_OK(validate_evidence_structure(evidence));
    evidence.confidence = 1.0;
    TG_STATUS_OK(validate_evidence_structure(evidence));
    evidence.confidence = 0.5;
    TG_STATUS_OK(validate_evidence_structure(evidence));
    evidence.confidence = std::nullopt;
    TG_STATUS_OK(validate_evidence_structure(evidence));
}

TG_CASE(evidence, rejects_provenance_overstating_the_source) {
    TG_PHASE("a real sensor source cannot be synthetic");
    ThermalEvidence evidence = valid_evidence();
    evidence.source = MeasurementSource::NVML_GPU_TEMPERATURE;
    evidence.provenance = Provenance::SYNTHETIC;
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);

    evidence.provenance = Provenance::UNKNOWN;
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);

    evidence.provenance = Provenance::UNSUPPORTED;
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);

    TG_PHASE("a synthetic model cannot be real");
    evidence = valid_evidence();
    evidence.source = MeasurementSource::SYNTHETIC_MODEL;
    evidence.provenance = Provenance::REAL;
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);

    TG_PHASE("honest pairings are accepted");
    evidence.provenance = Provenance::SYNTHETIC;
    TG_STATUS_OK(validate_evidence_structure(evidence));

    evidence = valid_evidence();
    evidence.source = MeasurementSource::NVML_GPU_TEMPERATURE;
    evidence.provenance = Provenance::REAL;
    TG_STATUS_OK(validate_evidence_structure(evidence));

    evidence = valid_evidence();
    evidence.source = MeasurementSource::PLATFORM_SENSOR;
    evidence.provenance = Provenance::UNSUPPORTED;
    TG_STATUS_OK(validate_evidence_structure(evidence));

    evidence = valid_evidence();
    evidence.source = MeasurementSource::UNKNOWN;
    evidence.provenance = Provenance::UNKNOWN;
    TG_STATUS_OK(validate_evidence_structure(evidence));
}

TG_CASE(evidence, rejects_synthetic_evidence_claiming_real_cooling) {
    CoolingEvidence cooling;
    cooling.provenance = Provenance::REAL;
    cooling.air_inlet = DegreesCelsius{21.0};

    ThermalEvidence evidence = valid_evidence();
    evidence.source = MeasurementSource::SYNTHETIC_MODEL;
    evidence.provenance = Provenance::SYNTHETIC;
    evidence.cooling = cooling;
    TG_CHECK(evidence.cooling->has_any());
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::INVALID_ARGUMENT);

    TG_PHASE("honest cooling provenance is accepted");
    evidence.cooling->provenance = Provenance::SYNTHETIC;
    TG_STATUS_OK(validate_evidence_structure(evidence));
}

TG_CASE(evidence, rejects_failed_integrity) {
    ThermalEvidence evidence = valid_evidence();
    evidence.integrity = IntegrityStatus::FAILED;
    TG_STATUS_ERROR_CODE(validate_evidence_structure(evidence), ThermalErrorCode::EVIDENCE_STALE);

    evidence.integrity = IntegrityStatus::DEGRADED;
    TG_STATUS_OK(validate_evidence_structure(evidence));
}

TG_CASE(evidence, compare_evidence_classifies_duplicates) {
    const ThermalEvidence held = valid_evidence();

    TG_PHASE("an identical replay is an idempotent duplicate");
    TG_CHECK(compare_evidence(held, held) == EvidenceComparison::IDENTICAL_DUPLICATE);
    ThermalEvidence replay = held;
    TG_CHECK(compare_evidence(held, replay) == EvidenceComparison::IDENTICAL_DUPLICATE);

    TG_PHASE("the same identity with divergent content conflicts");
    ThermalEvidence divergent = held;
    divergent.temperature = DegreesCelsius{85.0};
    TG_CHECK(compare_evidence(held, divergent) == EvidenceComparison::CONFLICTING_DUPLICATE);

    divergent = held;
    divergent.provenance = Provenance::SYNTHETIC;
    TG_CHECK(compare_evidence(held, divergent) == EvidenceComparison::CONFLICTING_DUPLICATE);

    TG_PHASE("a different identity claiming the same measurement position conflicts");
    ThermalEvidence other_identity = held;
    other_identity.evidence_id = EvidenceId{StrongId<thermal_governor::EvidenceIdTag>{8}};
    TG_CHECK(compare_evidence(held, other_identity) == EvidenceComparison::CONFLICTING_DUPLICATE);

    other_identity.temperature = DegreesCelsius{85.0};
    TG_CHECK(compare_evidence(held, other_identity) == EvidenceComparison::CONFLICTING_DUPLICATE);
}

TG_CASE(evidence, compare_evidence_reports_superseded_and_independent) {
    const ThermalEvidence held = valid_evidence();

    TG_PHASE("older measurements are superseded regardless of identity");
    ThermalEvidence older = held;
    older.measurement_sequence = 1;
    older.evidence_id = EvidenceId{StrongId<thermal_governor::EvidenceIdTag>{99}};
    older.temperature = DegreesCelsius{99.0};
    TG_CHECK(compare_evidence(held, older) == EvidenceComparison::SUPERSEDED);

    ThermalEvidence older_identical = held;
    older_identical.measurement_sequence = 1;
    TG_CHECK(compare_evidence(held, older_identical) == EvidenceComparison::SUPERSEDED);

    TG_PHASE("a strictly newer position is an independent observation");
    ThermalEvidence newer = held;
    newer.measurement_sequence = 3;
    newer.evidence_id = EvidenceId{StrongId<thermal_governor::EvidenceIdTag>{10}};
    newer.temperature = DegreesCelsius{91.5};
    TG_CHECK(compare_evidence(held, newer) == EvidenceComparison::INDEPENDENT);

    TG_CHECK(compare_evidence(newer, held) == EvidenceComparison::SUPERSEDED);
}

TG_CASE(evidence, canonical_digest_is_stable_and_temperature_sensitive) {
    const ThermalEvidence evidence = valid_evidence();
    const std::string digest = canonical_evidence_digest(evidence);

    TG_CHECK(!digest.empty());
    TG_CHECK_EQ(digest, canonical_evidence_digest(evidence));
    TG_CHECK_EQ(digest, canonical_evidence_digest(valid_evidence()));

    ThermalEvidence hotter = evidence;
    hotter.temperature = DegreesCelsius{99.25};
    const std::string hotter_digest = canonical_evidence_digest(hotter);
    TG_CHECK(hotter_digest != digest);
    TG_CHECK_EQ(hotter_digest, canonical_evidence_digest(hotter));
    TG_CHECK(hotter_digest.find("99.25") != std::string::npos);

    ThermalEvidence requantified = evidence;
    requantified.domain_generation =
        ThermalDomainGeneration{StrongId<thermal_governor::ThermalDomainGenerationTag>{2}};
    TG_CHECK(canonical_evidence_digest(requantified) != digest);

    ThermalEvidence relabelled = evidence;
    relabelled.evidence_id = EvidenceId{StrongId<thermal_governor::EvidenceIdTag>{8}};
    TG_CHECK(canonical_evidence_digest(relabelled) != digest);
}
