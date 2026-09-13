// Thermal Governor — thermal domain registry and aggregation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "thermal_governor/domain.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace thermal_governor {
namespace {

[[nodiscard]] Status fail(ThermalErrorCode code, std::string detail) {
    return Status::failure(code, std::move(detail));
}

}  // namespace

Status DomainRegistry::add(const ThermalDomainDefinition& definition) {
    if (!definition.id.is_valid()) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT, "thermal domain id must be non-zero");
    }
    if (!definition.generation.is_valid()) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT,
                    "thermal domain generation must be non-zero");
    }
    if (definition.provenance == Provenance::UNKNOWN) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT,
                    "thermal domain provenance must be declared explicitly");
    }
    if (definition.provenance == Provenance::UNSUPPORTED) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT,
                    "an UNSUPPORTED thermal domain cannot be registered as a governed domain");
    }
    if (definitions_.size() >= limits_.max_domains) {
        return fail(ThermalErrorCode::RESOURCE_EXHAUSTED, "thermal domain limit reached");
    }
    if (definition.members.size() > limits_.max_members_per_domain) {
        return fail(ThermalErrorCode::RESOURCE_EXHAUSTED, "thermal domain member limit reached");
    }
    if (definition.children.size() > limits_.max_children_per_domain) {
        return fail(ThermalErrorCode::RESOURCE_EXHAUSTED, "thermal domain child limit reached");
    }
    if (definitions_.find(definition.id.value()) != definitions_.end()) {
        return fail(ThermalErrorCode::DUPLICATE_CONFLICT, "thermal domain already registered");
    }
    if (definition.parent.has_value() &&
        definitions_.find(definition.parent->value()) == definitions_.end()) {
        return fail(ThermalErrorCode::UNKNOWN_DOMAIN, "thermal domain parent is not registered");
    }
    for (const auto& child : definition.children) {
        if (definitions_.find(child.value()) == definitions_.end()) {
            return fail(ThermalErrorCode::UNKNOWN_DOMAIN, "thermal domain child is not registered");
        }
    }
    if (definition.aggregation == AggregationRule::CONFIGURED_WEIGHTED_RULE) {
        for (const auto& member : definition.members) {
            if (!(member.weight > 0.0) || !std::isfinite(member.weight)) {
                return fail(ThermalErrorCode::INVALID_ARGUMENT,
                            "weighted aggregation requires strictly positive finite weights");
            }
        }
    }
    if (definition.aggregation == AggregationRule::NODE_SENSOR_GOVERNS) {
        const bool has_node_member =
            std::any_of(definition.members.begin(), definition.members.end(),
                        [](const DomainMember& m) { return m.subject.kind == SubjectKind::NODE; });
        if (!has_node_member) {
            return fail(ThermalErrorCode::INVALID_ARGUMENT,
                        "NODE_SENSOR_GOVERNS requires at least one node member");
        }
    }

    definitions_.emplace(definition.id.value(), definition);
    rebuild_membership();
    bump_topology_generation();
    return Status::success();
}

Status DomainRegistry::update(const ThermalDomainDefinition& definition) {
    const auto it = definitions_.find(definition.id.value());
    if (it == definitions_.end()) {
        return fail(ThermalErrorCode::UNKNOWN_DOMAIN, "thermal domain is not registered");
    }
    if (definition.generation.value() <= it->second.generation.value()) {
        return fail(ThermalErrorCode::STALE_DOMAIN_GENERATION,
                    "thermal domain update must advance the domain generation");
    }
    if (definition.provenance == Provenance::UNKNOWN ||
        definition.provenance == Provenance::UNSUPPORTED) {
        return fail(ThermalErrorCode::INVALID_ARGUMENT,
                    "thermal domain provenance must be REAL or SYNTHETIC");
    }
    if (definition.members.size() > limits_.max_members_per_domain) {
        return fail(ThermalErrorCode::RESOURCE_EXHAUSTED, "thermal domain member limit reached");
    }
    if (definition.children.size() > limits_.max_children_per_domain) {
        return fail(ThermalErrorCode::RESOURCE_EXHAUSTED, "thermal domain child limit reached");
    }
    if (definition.parent.has_value() && definition.parent->value() == definition.id.value()) {
        return fail(ThermalErrorCode::POLICY_CYCLE, "a thermal domain cannot parent itself");
    }
    if (definition.parent.has_value() &&
        definitions_.find(definition.parent->value()) == definitions_.end()) {
        return fail(ThermalErrorCode::UNKNOWN_DOMAIN, "thermal domain parent is not registered");
    }
    for (const auto& child : definition.children) {
        if (child.value() == definition.id.value()) {
            return fail(ThermalErrorCode::POLICY_CYCLE, "a thermal domain cannot be its own child");
        }
        if (definitions_.find(child.value()) == definitions_.end()) {
            return fail(ThermalErrorCode::UNKNOWN_DOMAIN, "thermal domain child is not registered");
        }
    }

    it->second = definition;
    rebuild_membership();
    bump_topology_generation();
    return Status::success();
}

Status DomainRegistry::remove(ThermalDomainId id) {
    const auto it = definitions_.find(id.value());
    if (it == definitions_.end()) {
        return fail(ThermalErrorCode::UNKNOWN_DOMAIN, "thermal domain is not registered");
    }
    for (const auto& [key, definition] : definitions_) {
        if (key == id.value()) {
            continue;
        }
        if (definition.parent.has_value() && definition.parent->value() == id.value()) {
            return fail(ThermalErrorCode::DUPLICATE_CONFLICT,
                        "thermal domain is still referenced as a parent");
        }
        if (std::find_if(definition.children.begin(), definition.children.end(),
                         [id](ThermalDomainId child) { return child == id; }) !=
            definition.children.end()) {
            return fail(ThermalErrorCode::DUPLICATE_CONFLICT,
                        "thermal domain is still referenced as a child");
        }
    }
    definitions_.erase(it);
    rebuild_membership();
    bump_topology_generation();
    return Status::success();
}

const ThermalDomainDefinition* DomainRegistry::find(ThermalDomainId id) const noexcept {
    const auto it = definitions_.find(id.value());
    return it == definitions_.end() ? nullptr : &it->second;
}

ThermalDomainDefinition* DomainRegistry::find_mutable(ThermalDomainId id) noexcept {
    const auto it = definitions_.find(id.value());
    return it == definitions_.end() ? nullptr : &it->second;
}

void DomainRegistry::rebuild_membership() {
    membership_.clear();
    for (const auto& [key, definition] : definitions_) {
        (void)key;
        for (const auto& member : definition.members) {
            membership_[member.subject.key()].push_back(definition.id);
        }
    }
    for (auto& [key, ids] : membership_) {
        (void)key;
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
}

std::vector<ThermalDomainId> DomainRegistry::domains_for_device(DeviceId device) const {
    std::vector<ThermalDomainId> out;
    for (const auto& [key, definition] : definitions_) {
        (void)key;
        for (const auto& member : definition.members) {
            if (member.subject.kind == SubjectKind::DEVICE && member.subject.device == device) {
                out.push_back(definition.id);
                break;
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<ThermalDomainId> DomainRegistry::domains_for_subject(const SubjectRef& subject) const {
    std::vector<ThermalDomainId> out;
    const std::string key = subject.key();
    const auto it = membership_.find(key);
    if (it != membership_.end()) {
        return it->second;
    }
    return out;
}

std::vector<ThermalDomainId> DomainRegistry::all_domain_ids() const {
    std::vector<ThermalDomainId> out;
    out.reserve(definitions_.size());
    for (const auto& [key, definition] : definitions_) {
        (void)definition;
        out.push_back(ThermalDomainId{key});
    }
    std::sort(out.begin(), out.end());
    return out;
}

Result<AggregateResult> compute_aggregate(const ThermalDomainDefinition& definition,
                                          const std::vector<const ThermalEvidence*>& member_evidence) {
    AggregateResult result;
    result.rule = definition.aggregation;
    result.provenance = definition.provenance;

    std::vector<const ThermalEvidence*> usable;
    usable.reserve(member_evidence.size());
    for (const auto* evidence : member_evidence) {
        if (evidence != nullptr) {
            usable.push_back(evidence);
        }
    }
    if (usable.empty()) {
        return ThermalError{ThermalErrorCode::EVIDENCE_UNKNOWN,
                            "no member thermal evidence is available for aggregation"};
    }
    result.contributing_members = usable.size();

    Provenance combined = Provenance::REAL;
    bool first = true;
    for (const auto* evidence : usable) {
        combined = first ? evidence->provenance : combine(combined, evidence->provenance);
        first = false;
    }

    switch (definition.aggregation) {
        case AggregationRule::HOTTEST_MEMBER_GOVERNS: {
            const ThermalEvidence* hottest = usable.front();
            for (const auto* evidence : usable) {
                if (evidence->temperature.value() > hottest->temperature.value()) {
                    hottest = evidence;
                }
            }
            result.temperature = hottest->temperature;
            result.governing_member = hottest->subject;
            result.provenance = combined;
            return result;
        }
        case AggregationRule::CONFIGURED_WEIGHTED_RULE: {
            double weighted_sum = 0.0;
            double weight_total = 0.0;
            const ThermalEvidence* heaviest = usable.front();
            double heaviest_weight = -1.0;
            for (const auto* evidence : usable) {
                double weight = 1.0;
                for (const auto& member : definition.members) {
                    if (member.subject == evidence->subject) {
                        weight = member.weight;
                        break;
                    }
                }
                weighted_sum += evidence->temperature.value() * weight;
                weight_total += weight;
                if (weight > heaviest_weight) {
                    heaviest_weight = weight;
                    heaviest = evidence;
                }
            }
            if (!(weight_total > 0.0)) {
                return ThermalError{ThermalErrorCode::POLICY_INVALID,
                                    "weighted aggregation produced a non-positive weight total"};
            }
            const double aggregate = weighted_sum / weight_total;
            auto parsed = DegreesCelsius::try_from(aggregate);
            if (!parsed.has_value()) {
                return ThermalError{ThermalErrorCode::TEMPERATURE_INVALID,
                                    "weighted aggregation produced an invalid temperature"};
            }
            result.temperature = *parsed;
            result.governing_member = heaviest->subject;
            // A weighted average is a model, never a measurement.
            result.provenance = definition.provenance == Provenance::REAL
                                    ? Provenance::SYNTHETIC
                                    : combine(combined, Provenance::SYNTHETIC);
            return result;
        }
        case AggregationRule::NODE_SENSOR_GOVERNS: {
            const ThermalEvidence* sensor = nullptr;
            for (const auto* evidence : usable) {
                if (evidence->subject.kind == SubjectKind::NODE) {
                    sensor = evidence;
                    break;
                }
            }
            if (sensor == nullptr) {
                return ThermalError{ThermalErrorCode::EVIDENCE_UNKNOWN,
                                    "node sensor evidence is unavailable for this domain"};
            }
            result.temperature = sensor->temperature;
            result.governing_member = sensor->subject;
            result.provenance = sensor->provenance;
            return result;
        }
        case AggregationRule::SYNTHETIC_AGGREGATION: {
            const ThermalEvidence* hottest = usable.front();
            for (const auto* evidence : usable) {
                if (evidence->temperature.value() > hottest->temperature.value()) {
                    hottest = evidence;
                }
            }
            const double offset = definition.synthetic_aggregate_offset.value_or(0.0);
            auto parsed = DegreesCelsius::try_from(hottest->temperature.value() + offset);
            if (!parsed.has_value()) {
                return ThermalError{ThermalErrorCode::TEMPERATURE_INVALID,
                                    "synthetic aggregation produced an invalid temperature"};
            }
            result.temperature = *parsed;
            result.governing_member = hottest->subject;
            // A modelled aggregate is always SYNTHETIC, whatever the members
            // claimed. Provenance is never silently upgraded.
            result.provenance = definition.provenance == Provenance::SYNTHETIC
                                    ? Provenance::SYNTHETIC
                                    : combine(combined, Provenance::SYNTHETIC);
            return result;
        }
    }
    return ThermalError{ThermalErrorCode::INTERNAL, "unhandled aggregation rule"};
}

}  // namespace thermal_governor
