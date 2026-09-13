// Thermal Governor — evidence validation and comparison.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/evidence.hpp"

#include "format.hpp"

namespace thermal_governor {
namespace {

/// Provenance implied by a measurement source. A record that claims a
/// stronger provenance than its source can support is rejected outright.
[[nodiscard]] bool provenance_matches_source(MeasurementSource source, Provenance provenance) {
    switch (source) {
        case MeasurementSource::NVML_GPU_TEMPERATURE:
        case MeasurementSource::NVML_TEMPERATURE_THRESHOLD:
        case MeasurementSource::CUDA_DEVICE_ATTRIBUTE:
            return provenance == Provenance::REAL;
        case MeasurementSource::SYNTHETIC_MODEL:
            return provenance == Provenance::SYNTHETIC;
        case MeasurementSource::PLATFORM_SENSOR:
        case MeasurementSource::NODE_SENSOR:
        case MeasurementSource::RACK_SENSOR:
        case MeasurementSource::COOLING_ZONE_SENSOR:
            return provenance == Provenance::REAL || provenance == Provenance::SYNTHETIC ||
                   provenance == Provenance::UNSUPPORTED;
        case MeasurementSource::OPERATOR_INJECTED:
            return provenance == Provenance::SYNTHETIC || provenance == Provenance::REAL;
        case MeasurementSource::UNKNOWN:
            return true;
    }
    return false;
}

[[nodiscard]] bool subject_identity_present(const SubjectRef& subject) {
    switch (subject.kind) {
        case SubjectKind::DEVICE:
            return subject.device.is_valid() && subject.device_generation.is_valid();
        case SubjectKind::NODE:
            return subject.node.is_valid() && subject.node_generation.is_valid();
        case SubjectKind::RACK:
            return subject.rack.is_valid() && subject.rack_generation.is_valid();
        case SubjectKind::COOLING_ZONE:
            return subject.cooling_zone.is_valid();
        case SubjectKind::CHASSIS:
            return subject.chassis.is_valid();
        case SubjectKind::THERMAL_DOMAIN:
            return subject.domain.is_valid() && subject.domain_generation.is_valid();
    }
    return false;
}

}  // namespace

std::string SubjectRef::key() const {
    std::string out(to_string(kind));
    out += ":";
    switch (kind) {
        case SubjectKind::DEVICE:
            out += detail::unsigned_decimal(device.value());
            out += ":";
            out += detail::unsigned_decimal(device_generation.value());
            break;
        case SubjectKind::NODE:
            out += detail::unsigned_decimal(node.value());
            out += ":";
            out += detail::unsigned_decimal(node_generation.value());
            break;
        case SubjectKind::RACK:
            out += detail::unsigned_decimal(rack.value());
            out += ":";
            out += detail::unsigned_decimal(rack_generation.value());
            break;
        case SubjectKind::COOLING_ZONE:
            out += detail::unsigned_decimal(cooling_zone.value());
            break;
        case SubjectKind::CHASSIS:
            out += detail::unsigned_decimal(chassis.value());
            break;
        case SubjectKind::THERMAL_DOMAIN:
            out += detail::unsigned_decimal(domain.value());
            out += ":";
            out += detail::unsigned_decimal(domain_generation.value());
            break;
    }
    return out;
}

Status validate_evidence_structure(const ThermalEvidence& evidence) {
    if (!evidence.temperature.is_valid()) {
        return Status::failure(ThermalErrorCode::TEMPERATURE_INVALID,
                               "temperature outside the representable range");
    }
    if (!subject_identity_present(evidence.subject)) {
        return Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                               "evidence subject identity is incomplete for its kind");
    }
    if (!evidence.telemetry_generation.is_valid()) {
        return Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                               "telemetry generation must be non-zero");
    }
    if (!evidence.coordinator_epoch.is_valid()) {
        return Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                               "coordinator epoch must be non-zero");
    }
    if (evidence.worker.is_valid() != evidence.worker_boot.is_valid()) {
        return Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                               "worker identity and worker boot identity must be supplied together");
    }
    if (evidence.confidence.has_value()) {
        const double confidence = *evidence.confidence;
        if (!(confidence >= 0.0 && confidence <= 1.0)) {
            return Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                   "confidence must lie in [0, 1]");
        }
    }
    if (evidence.temperature_limit.has_value() && !evidence.temperature_limit->is_valid()) {
        return Status::failure(ThermalErrorCode::TEMPERATURE_INVALID,
                               "temperature limit outside the representable range");
    }
    if (evidence.shutdown_limit.has_value() && !evidence.shutdown_limit->is_valid()) {
        return Status::failure(ThermalErrorCode::TEMPERATURE_INVALID,
                               "shutdown limit outside the representable range");
    }
    if (!provenance_matches_source(evidence.source, evidence.provenance)) {
        return Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                               "evidence provenance overstates its measurement source");
    }
    if (evidence.cooling.has_value() &&
        evidence.cooling->provenance == Provenance::REAL &&
        evidence.source == MeasurementSource::SYNTHETIC_MODEL) {
        return Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                               "synthetic evidence cannot carry real cooling provenance");
    }
    if (evidence.integrity == IntegrityStatus::FAILED) {
        return Status::failure(ThermalErrorCode::EVIDENCE_STALE,
                               "evidence integrity check failed");
    }
    return Status::success();
}

std::string canonical_evidence_digest(const ThermalEvidence& evidence) {
    std::string out;
    out += detail::unsigned_decimal(evidence.evidence_id.value());
    out += "|";
    out += evidence.subject.key();
    out += "|";
    out += detail::unsigned_decimal(evidence.domain.value());
    out += "|";
    out += detail::unsigned_decimal(evidence.domain_generation.value());
    out += "|";
    out += detail::number(evidence.temperature.value());
    out += "|";
    out += to_string(evidence.source);
    out += "|";
    out += to_string(evidence.provenance);
    out += "|";
    out += detail::unsigned_decimal(evidence.measurement_sequence);
    out += "|";
    out += detail::unsigned_decimal(evidence.telemetry_generation.value());
    out += "|";
    out += detail::unsigned_decimal(evidence.worker.value());
    out += "|";
    out += detail::unsigned_decimal(evidence.worker_boot.value());
    out += "|";
    out += detail::unsigned_decimal(evidence.coordinator_epoch.value());
    out += "|";
    out += detail::unsigned_decimal(evidence.capability_generation.value());
    out += "|";
    out += detail::unsigned_decimal(evidence.topology_generation.value());
    out += "|";
    out += to_string(evidence.integrity);
    if (evidence.throttle.has_value()) {
        out += "|T:";
        out += detail::unsigned_decimal(evidence.throttle->raw_reasons);
        out += ":";
        out += to_string(evidence.throttle->classification);
    }
    if (evidence.temperature_limit.has_value()) {
        out += "|L:";
        out += detail::number(evidence.temperature_limit->value());
    }
    if (evidence.shutdown_limit.has_value()) {
        out += "|S:";
        out += detail::number(evidence.shutdown_limit->value());
    }
    if (evidence.current_clock.has_value()) {
        out += "|C:";
        out += detail::unsigned_decimal(evidence.current_clock->value());
    }
    if (evidence.max_clock.has_value()) {
        out += "|M:";
        out += detail::unsigned_decimal(evidence.max_clock->value());
    }
    if (evidence.fan.has_value() && evidence.fan->has_any()) {
        out += "|F:";
        if (evidence.fan->speed_percent.has_value()) {
            out += detail::number(*evidence.fan->speed_percent);
        }
        out += ":";
        if (evidence.fan->speed_rpm.has_value()) {
            out += detail::unsigned_decimal(*evidence.fan->speed_rpm);
        }
    }
    if (evidence.cooling.has_value() && evidence.cooling->has_any()) {
        const auto& cooling = *evidence.cooling;
        out += "|K:";
        if (cooling.air_inlet.has_value()) {
            out += detail::number(cooling.air_inlet->value());
        }
        out += ":";
        if (cooling.liquid_inlet.has_value()) {
            out += detail::number(cooling.liquid_inlet->value());
        }
        out += ":";
        if (cooling.coolant_flow_litres_per_minute.has_value()) {
            out += detail::number(*cooling.coolant_flow_litres_per_minute);
        }
    }
    if (evidence.confidence.has_value()) {
        out += "|P:";
        out += detail::number(*evidence.confidence);
    }
    return out;
}

EvidenceComparison compare_evidence(const ThermalEvidence& held,
                                    const ThermalEvidence& incoming) {
    if (incoming.measurement_sequence < held.measurement_sequence) {
        return EvidenceComparison::SUPERSEDED;
    }

    const std::string incoming_digest = canonical_evidence_digest(incoming);

    if (incoming.evidence_id == held.evidence_id) {
        return incoming_digest == canonical_evidence_digest(held)
                   ? EvidenceComparison::IDENTICAL_DUPLICATE
                   : EvidenceComparison::CONFLICTING_DUPLICATE;
    }

    if (incoming.measurement_sequence == held.measurement_sequence) {
        // The same measurement position claimed by two different evidence
        // identities. Identical content is an idempotent duplicate;
        // divergent content is an explicit conflict.
        return incoming_digest == canonical_evidence_digest(held)
                   ? EvidenceComparison::IDENTICAL_DUPLICATE
                   : EvidenceComparison::CONFLICTING_DUPLICATE;
    }

    return EvidenceComparison::INDEPENDENT;
}

}  // namespace thermal_governor
