// Thermal Governor — coordinator process.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "session.hpp"
#include "thermal_governor/thermal_governor.hpp"
#include "thermal_governor/wire.hpp"

namespace thermal_governor::tools {
namespace {

using wire::CommandRequest;
using wire::CommandResponse;
using wire::StatusPayload;
using wire::WorkerRegistration;

struct Options {
    std::uint16_t port = 0;
    std::string state_path;
    std::uint64_t epoch = 1;
    std::string ready_file;
    std::size_t max_connections = 64;
};

[[nodiscard]] bool option_value(int argc, char** argv, int& index, std::string& out) {
    if (index + 1 >= argc) {
        return false;
    }
    out = argv[++index];
    return true;
}

// --- Argument parsing for structured command arguments --------------------

[[nodiscard]] bool parse_typed_member(const std::string& token, DomainMember& member) {
    const auto parts = split(token, ':');
    if (parts.size() == 3) {
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
        if (!parse_u64(parts[1], id) || !parse_u64(parts[2], generation)) {
            return false;
        }
        if (parts[0] == "d") {
            member.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{id}},
                                                    DeviceGeneration{StrongId<DeviceGenerationTag>{generation}});
            return true;
        }
        if (parts[0] == "n") {
            member.subject = SubjectRef::for_node(NodeId{StrongId<NodeIdTag>{id}},
                                                  NodeGeneration{StrongId<NodeGenerationTag>{generation}});
            return true;
        }
        if (parts[0] == "r") {
            member.subject = SubjectRef::for_rack(RackId{StrongId<RackIdTag>{id}},
                                                  RackGeneration{StrongId<RackGenerationTag>{generation}});
            return true;
        }
        return false;
    }
    if (parts.size() == 2 && parts[0] == "z") {
        std::uint64_t id = 0;
        if (!parse_u64(parts[1], id)) {
            return false;
        }
        member.subject = SubjectRef::for_cooling_zone(CoolingZoneId{StrongId<CoolingZoneIdTag>{id}});
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_domain_type(const std::string& text, ThermalDomainType& out) {
    static const std::pair<const char*, ThermalDomainType> kTypes[] = {
        {"ACCELERATOR", ThermalDomainType::ACCELERATOR},
        {"ACCELERATOR_PARTITION", ThermalDomainType::ACCELERATOR_PARTITION},
        {"NODE", ThermalDomainType::NODE},
        {"CHASSIS", ThermalDomainType::CHASSIS},
        {"RACK", ThermalDomainType::RACK},
        {"COOLING_ZONE", ThermalDomainType::COOLING_ZONE},
        {"COUPLED_RESOURCE_GROUP", ThermalDomainType::COUPLED_RESOURCE_GROUP},
    };
    for (const auto& [name, value] : kTypes) {
        if (text == name) {
            out = value;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool parse_aggregation(const std::string& text, AggregationRule& out) {
    static const std::pair<const char*, AggregationRule> kRules[] = {
        {"HOTTEST_MEMBER_GOVERNS", AggregationRule::HOTTEST_MEMBER_GOVERNS},
        {"CONFIGURED_WEIGHTED_RULE", AggregationRule::CONFIGURED_WEIGHTED_RULE},
        {"NODE_SENSOR_GOVERNS", AggregationRule::NODE_SENSOR_GOVERNS},
        {"SYNTHETIC_AGGREGATION", AggregationRule::SYNTHETIC_AGGREGATION},
    };
    for (const auto& [name, value] : kRules) {
        if (text == name) {
            out = value;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool parse_coupling_type(const std::string& text, CouplingType& out) {
    static const std::pair<const char*, CouplingType> kTypes[] = {
        {"UNKNOWN_COUPLING", CouplingType::UNKNOWN_COUPLING},
        {"SHARES_CHASSIS", CouplingType::SHARES_CHASSIS},
        {"SHARES_AIRFLOW_PATH", CouplingType::SHARES_AIRFLOW_PATH},
        {"SHARES_COOLING_ZONE", CouplingType::SHARES_COOLING_ZONE},
        {"SHARES_HEATSINK", CouplingType::SHARES_HEATSINK},
        {"THERMALLY_COUPLED", CouplingType::THERMALLY_COUPLED},
        {"SYNTHETIC_COUPLING", CouplingType::SYNTHETIC_COUPLING},
    };
    for (const auto& [name, value] : kTypes) {
        if (text == name) {
            out = value;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool parse_provenance(const std::string& text, Provenance& out) {
    if (text == "REAL") {
        out = Provenance::REAL;
        return true;
    }
    if (text == "SYNTHETIC") {
        out = Provenance::SYNTHETIC;
        return true;
    }
    if (text == "UNSUPPORTED") {
        out = Provenance::UNSUPPORTED;
        return true;
    }
    return false;
}

[[nodiscard]] bool parse_capability_state(const std::string& text, CapabilityState& out) {
    static const std::pair<const char*, CapabilityState> kStates[] = {
        {"UNKNOWN", CapabilityState::UNKNOWN},
        {"SUPPORTED_REAL", CapabilityState::SUPPORTED_REAL},
        {"SUPPORTED_READ_ONLY", CapabilityState::SUPPORTED_READ_ONLY},
        {"SUPPORTED_SYNTHETIC", CapabilityState::SUPPORTED_SYNTHETIC},
        {"UNSUPPORTED", CapabilityState::UNSUPPORTED},
    };
    for (const auto& [name, value] : kStates) {
        if (text == name) {
            out = value;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool parse_intent(const std::string& text, MitigationIntent& out) {
    for (std::uint32_t i = 0; i < kMitigationIntentCount; ++i) {
        const auto intent = static_cast<MitigationIntent>(i);
        if (text == to_string(intent)) {
            out = intent;
            return true;
        }
    }
    return false;
}

/// Coordinator-side command surface. Every mutation travels the same public
/// authority path a library caller would use.
class CoordinatorSession {
public:
    CoordinatorSession(ThermalGovernor& governor, std::string state_path)
        : governor_(governor), state_path_(std::move(state_path)) {}

    [[nodiscard]] CommandResponse handle(const CommandRequest& request);

private:
    [[nodiscard]] CommandResponse ok(std::string payload) {
        CommandResponse response;
        response.status.code = ThermalErrorCode::NONE;
        response.payload = std::move(payload);
        return response;
    }
    [[nodiscard]] CommandResponse fail(const Status& status) {
        CommandResponse response;
        response.status.code = status.code();
        response.status.detail = status.error().detail;
        return response;
    }
    template <class T>
    [[nodiscard]] CommandResponse fail(const Result<T>& result) {
        CommandResponse response;
        response.status.code = result.error().code;
        response.status.detail = result.error().detail;
        return response;
    }

    [[nodiscard]] Status persist_now();
    [[nodiscard]] CommandResponse handle_structured(const CommandRequest& request);

    ThermalGovernor& governor_;
    std::string state_path_;
};

Status CoordinatorSession::persist_now() {
    if (state_path_.empty()) {
        return Status::success();
    }
    return governor_.persist();
}

CommandResponse CoordinatorSession::handle(const CommandRequest& request) {
    const auto& args = request.arguments;

    if (request.verb == "status") {
        auto snapshot = governor_.snapshot();
        return ok(snapshot != nullptr ? snapshot->render_summary() : std::string("no snapshot"));
    }
    if (request.verb == "epoch") {
        return ok(std::to_string(governor_.epoch().value()));
    }
    if (request.verb == "domains") {
        auto snapshot = governor_.snapshot();
        std::string out;
        for (const auto& [key, domain] : snapshot->domains) {
            out += std::to_string(key);
            out += " ";
            out += to_string(domain.state);
            out += " ";
            out += to_string(domain.derating);
            out += "\n";
        }
        return ok(out);
    }
    if (request.verb == "devices") {
        auto snapshot = governor_.snapshot();
        std::string out;
        for (const auto& [key, device] : snapshot->devices) {
            out += std::to_string(key);
            out += " gen=";
            out += std::to_string(device.generation.value());
            out += " ";
            out += to_string(device.state);
            if (device.temperature.has_value()) {
                out += " ";
                out += device.temperature->render();
                out += "C";
            }
            out += "\n";
        }
        return ok(out);
    }
    if (request.verb == "snapshot-info") {
        return ok(governor_.snapshot()->render_summary());
    }
    if (request.verb == "actions") {
        std::string out;
        for (const auto& action : governor_.actions()) {
            out += render_action(action);
            out += "--\n";
        }
        return ok(out);
    }
    if (request.verb == "history") {
        std::string out;
        for (const auto& record : governor_.history()) {
            out += std::to_string(record.domain.value());
            out += " ";
            out += to_string(record.from);
            out += " -> ";
            out += to_string(record.to);
            out += " epoch=";
            out += std::to_string(record.coordinator_epoch.value());
            out += " seq=";
            out += std::to_string(record.sequence);
            out += "\n";
        }
        return ok(out);
    }
    if (request.verb == "persist") {
        auto status = persist_now();
        if (!status.ok()) {
            return fail(status);
        }
        return ok("persisted");
    }
    if (request.verb == "shutdown") {
        auto status = governor_.shutdown();
        if (!status.ok()) {
            return fail(status);
        }
        return ok("shutdown");
    }
    if (request.verb == "explain") {
        if (args.size() != 1) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT, "explain needs a domain id"));
        }
        std::uint64_t id = 0;
        if (!parse_u64(args[0], id)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT, "malformed domain id"));
        }
        auto explanation = governor_.explain(ThermalDomainId{StrongId<ThermalDomainIdTag>{id}});
        if (!explanation.has_value()) {
            return fail(explanation);
        }
        return ok(explanation.value().render());
    }
    return handle_structured(request);
}

CommandResponse CoordinatorSession::handle_structured(const CommandRequest& request) {
    const auto& args = request.arguments;

    if (request.verb == "publish") {
        std::vector<std::uint8_t> bytes;
        if (!from_hex(request.payload, bytes)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT, "payload is not hex"));
        }
        auto evidence = wire::decode_evidence(bytes.data(), bytes.size());
        if (!evidence.has_value()) {
            return fail(evidence);
        }
        auto receipt = governor_.publish_temperature(evidence.value());
        if (!receipt.has_value()) {
            return fail(receipt);
        }
        std::string out = "accepted=1 telemetry=";
        out += std::to_string(receipt.value().telemetry_generation.value());
        return ok(out);
    }
    if (request.verb == "register-device") {
        if (args.size() < 11) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "register-device needs 11 arguments"));
        }
        std::uint64_t values[6] = {};
        for (int i = 0; i < 6; ++i) {
            if (!parse_u64(args[static_cast<std::size_t>(i)], values[i])) {
                return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                            "malformed numeric device argument"));
            }
        }
        CapabilityState temperature = CapabilityState::UNKNOWN;
        CapabilityState throttle = CapabilityState::UNKNOWN;
        CapabilityState limit = CapabilityState::UNKNOWN;
        Provenance provenance = Provenance::UNKNOWN;
        if (!parse_capability_state(args[7], temperature) ||
            !parse_capability_state(args[8], throttle) ||
            !parse_capability_state(args[9], limit) || !parse_provenance(args[10], provenance)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "malformed device capability argument"));
        }
        DeviceRegistration registration;
        registration.device = DeviceId{StrongId<DeviceIdTag>{values[0]}};
        registration.generation = DeviceGeneration{StrongId<DeviceGenerationTag>{values[1]}};
        registration.node = NodeId{StrongId<NodeIdTag>{values[2]}};
        registration.node_generation = NodeGeneration{StrongId<NodeGenerationTag>{values[3]}};
        registration.rack = RackId{StrongId<RackIdTag>{values[4]}};
        registration.rack_generation = RackGeneration{StrongId<RackGenerationTag>{values[5]}};
        registration.label = Label{args[6]};
        registration.provenance = provenance;
        registration.capabilities.set(ThermalCapability::DEVICE_IDENTITY,
                                      CapabilityState::SUPPORTED_SYNTHETIC);
        registration.capabilities.set(ThermalCapability::TEMPERATURE, temperature);
        registration.capabilities.set(ThermalCapability::THROTTLE_REASONS, throttle);
        registration.capabilities.set(ThermalCapability::TEMPERATURE_LIMIT, limit);
        registration.capabilities.set(ThermalCapability::CURRENT_CLOCK,
                                      CapabilityState::SUPPORTED_SYNTHETIC);
        registration.capabilities.set(ThermalCapability::MAX_CLOCK,
                                      CapabilityState::SUPPORTED_SYNTHETIC);
        auto status = governor_.register_device(registration);
        if (!status.ok()) {
            return fail(status);
        }
        auto persisted = persist_now();
        if (!persisted.ok()) {
            return fail(persisted);
        }
        return ok("registered");
    }
    if (request.verb == "register-domain") {
        if (args.size() < 6) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "register-domain needs at least 6 arguments"));
        }
        std::uint64_t id = 0;
        std::uint64_t generation = 0;
        std::uint64_t policy_id = 1;
        ThermalDomainType type = ThermalDomainType::ACCELERATOR;
        Provenance provenance = Provenance::UNKNOWN;
        AggregationRule aggregation = AggregationRule::HOTTEST_MEMBER_GOVERNS;
        if (!parse_u64(args[0], id) || !parse_u64(args[1], generation) ||
            !parse_domain_type(args[2], type) || !parse_provenance(args[3], provenance) ||
            !parse_aggregation(args[4], aggregation) || !parse_u64(args[6], policy_id)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "malformed domain definition argument"));
        }
        ThermalDomainDefinition definition;
        definition.id = ThermalDomainId{StrongId<ThermalDomainIdTag>{id}};
        definition.generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{generation}};
        definition.type = type;
        definition.provenance = provenance;
        definition.aggregation = aggregation;
        definition.label = Label{args[5]};
        definition.policy = ThermalPolicyId{StrongId<ThermalPolicyIdTag>{policy_id}};
        definition.policy_generation = governor_.policy_generation();
        for (std::size_t i = 7; i < args.size(); ++i) {
            DomainMember member;
            if (!parse_typed_member(args[i], member)) {
                return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                            "malformed domain member token"));
            }
            definition.members.push_back(member);
        }
        auto status = governor_.register_thermal_domain(definition);
        if (!status.ok()) {
            return fail(status);
        }
        auto persisted = persist_now();
        if (!persisted.ok()) {
            return fail(persisted);
        }
        return ok("registered");
    }
    if (request.verb == "register-coupling") {
        if (args.size() < 6) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "register-coupling needs 6 arguments"));
        }
        std::uint64_t id = 0;
        std::uint64_t source = 0;
        std::uint64_t destination = 0;
        double weight = 1.0;
        CouplingType type = CouplingType::UNKNOWN_COUPLING;
        Provenance provenance = Provenance::UNKNOWN;
        if (!parse_u64(args[0], id) || !parse_u64(args[1], source) ||
            !parse_u64(args[2], destination) || !parse_coupling_type(args[3], type) ||
            !parse_provenance(args[4], provenance) || !parse_double(args[5], weight)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "malformed coupling argument"));
        }
        CouplingRelation relation;
        relation.id = CouplingId{StrongId<CouplingIdTag>{id}};
        relation.source = ThermalDomainId{StrongId<ThermalDomainIdTag>{source}};
        relation.destination = ThermalDomainId{StrongId<ThermalDomainIdTag>{destination}};
        relation.type = type;
        relation.provenance = provenance;
        relation.weight = weight;
        relation.generation = CouplingGeneration{StrongId<CouplingGenerationTag>{id}};
        relation.evidence_source = "operator-configured coupling";
        auto status = governor_.register_coupling(relation);
        if (!status.ok()) {
            return fail(status);
        }
        auto persisted = persist_now();
        if (!persisted.ok()) {
            return fail(persisted);
        }
        return ok("registered");
    }
    if (request.verb == "set-thresholds") {
        if (args.size() < 12) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "set-thresholds needs 12 arguments: eight values, a "
                                        "policy generation, a maximum evidence age in "
                                        "milliseconds, a required consecutive sample count and a "
                                        "minimum evidence span in milliseconds"));
        }
        double values[8] = {};
        for (int i = 0; i < 8; ++i) {
            if (!parse_double(args[static_cast<std::size_t>(i)], values[i])) {
                return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                            "malformed threshold argument"));
            }
        }
        std::uint64_t generation = 0;
        std::uint64_t max_age_ms = 0;
        std::uint64_t required_samples = 0;
        std::uint64_t min_span_ms = 0;
        if (!parse_u64(args[8], generation) || !parse_u64(args[9], max_age_ms) ||
            !parse_u64(args[10], required_samples) || !parse_u64(args[11], min_span_ms)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "malformed policy generation or freshness argument"));
        }
        auto current = governor_.get_policy(ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}});
        if (!current.has_value()) {
            return fail(current);
        }
        ThermalPolicy policy = current.value();
        policy.generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{generation}};
        policy.thresholds.warning = DegreesCelsius{values[0]};
        policy.thresholds.derating = DegreesCelsius{values[1]};
        policy.thresholds.critical = DegreesCelsius{values[2]};
        policy.thresholds.recovery = DegreesCelsius{values[3]};
        policy.thresholds.near_limit_band = TemperatureDelta{values[4]};
        policy.margins.policy_safety_margin = TemperatureDelta{values[5]};
        policy.margins.uncertainty_margin = TemperatureDelta{values[6]};
        policy.margins.recovery_margin = TemperatureDelta{values[7]};
        policy.freshness.max_age = Milliseconds{static_cast<std::int64_t>(max_age_ms)};
        policy.freshness.required_consecutive_samples =
            static_cast<std::uint32_t>(required_samples);
        policy.freshness.min_evidence_span = Milliseconds{static_cast<std::int64_t>(min_span_ms)};
        auto applied = governor_.set_policy(policy);
        if (!applied.has_value()) {
            return fail(applied);
        }
        auto persisted = persist_now();
        if (!persisted.ok()) {
            return fail(persisted);
        }
        return ok("policy-generation=" + std::to_string(applied.value().value()));
    }
    if (request.verb == "bump-domain-generation") {
        if (args.size() != 1) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "bump-domain-generation needs a domain id"));
        }
        std::uint64_t id = 0;
        if (!parse_u64(args[0], id)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT, "malformed domain id"));
        }
        auto snapshot = governor_.snapshot();
        const auto* domain = snapshot->find_domain(ThermalDomainId{StrongId<ThermalDomainIdTag>{id}});
        if (domain == nullptr) {
            return fail(Status::failure(ThermalErrorCode::UNKNOWN_DOMAIN,
                                        "thermal domain is not registered"));
        }
        ThermalDomainDefinition definition = domain->definition;
        definition.generation = ThermalDomainGeneration{
            StrongId<ThermalDomainGenerationTag>{definition.generation.value() + 1}};
        auto status = governor_.update_thermal_domain(definition);
        if (!status.ok()) {
            return fail(status);
        }
        auto persisted = persist_now();
        if (!persisted.ok()) {
            return fail(persisted);
        }
        return ok("domain-generation=" + std::to_string(definition.generation.value()));
    }
    if (request.verb == "bump-device-generation") {
        if (args.size() != 1) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "bump-device-generation needs a device id"));
        }
        std::uint64_t id = 0;
        if (!parse_u64(args[0], id)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT, "malformed device id"));
        }
        auto snapshot = governor_.snapshot();
        const auto* device = snapshot->find_device(DeviceId{StrongId<DeviceIdTag>{id}});
        if (device == nullptr) {
            return fail(Status::failure(ThermalErrorCode::UNKNOWN_DEVICE,
                                        "device is not registered"));
        }
        DeviceRegistration registration;
        registration.device = device->device;
        registration.generation = DeviceGeneration{
            StrongId<DeviceGenerationTag>{device->generation.value() + 1}};
        registration.node = device->node;
        registration.node_generation = device->evidence.has_value()
                                           ? device->evidence->subject.node_generation
                                           : NodeGeneration{StrongId<NodeGenerationTag>{1}};
        registration.rack = device->rack;
        registration.rack_generation = RackGeneration{StrongId<RackGenerationTag>{1}};
        registration.label = device->label;
        registration.provenance = device->provenance;
        registration.capabilities = device->capabilities;
        auto status = governor_.register_device(registration);
        if (!status.ok()) {
            return fail(status);
        }
        auto persisted = persist_now();
        if (!persisted.ok()) {
            return fail(persisted);
        }
        return ok("device-generation=" + std::to_string(registration.generation.value()));
    }
    if (request.verb == "worker-death") {
        if (args.size() < 2) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "worker-death needs a worker id and boot id"));
        }
        std::uint64_t worker = 0;
        std::uint64_t boot = 0;
        if (!parse_u64(args[0], worker) || !parse_u64(args[1], boot)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "malformed worker identity"));
        }
        auto status = governor_.note_worker_death(
            WorkerId{StrongId<WorkerIdTag>{worker}}, WorkerBootId{StrongId<WorkerBootIdTag>{boot}},
            "worker process terminated");
        if (!status.ok()) {
            return fail(status);
        }
        auto persisted = persist_now();
        if (!persisted.ok()) {
            return fail(persisted);
        }
        return ok("retired");
    }
    if (request.verb == "authorize-mitigation" || request.verb == "authorize-derating") {
        if (args.size() < 2) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "authorize needs a domain id and an intent"));
        }
        std::uint64_t id = 0;
        MitigationIntent intent = MitigationIntent::NO_ACTION;
        if (!parse_u64(args[0], id) || !parse_intent(args[1], intent)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "malformed authorize argument"));
        }
        MitigationIntentRecord record;
        record.intent = intent;
        const ThermalDomainId domain{StrongId<ThermalDomainIdTag>{id}};
        record.domain = domain;
        if (intent == MitigationIntent::REQUEST_CONCURRENCY_REDUCTION) {
            record.concurrency_ceiling = Percent{50.0};
        }
        auto action = request.verb == "authorize-derating"
                          ? governor_.authorize_derating(domain, record, "operator request")
                          : governor_.authorize_mitigation(domain, record, "operator request");
        if (!action.has_value()) {
            return fail(action);
        }
        auto persisted = persist_now();
        if (!persisted.ok()) {
            return fail(persisted);
        }
        return ok("action=" + std::to_string(action.value().value()));
    }
    if (request.verb == "dispatch" || request.verb == "cancel") {
        if (args.size() != 1) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "action verb needs an action id"));
        }
        std::uint64_t id = 0;
        if (!parse_u64(args[0], id)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT, "malformed action id"));
        }
        const ActionId action{StrongId<ActionIdTag>{id}};
        if (request.verb == "dispatch") {
            auto lifecycle = governor_.record_action_dispatch(action);
            if (!lifecycle.has_value()) {
                return fail(lifecycle);
            }
            return ok(std::string(to_string(lifecycle.value())));
        }
        auto cancelled = governor_.cancel_action(action, "operator cancel");
        if (!cancelled.has_value()) {
            return fail(cancelled);
        }
        return ok(std::string(to_string(cancelled.value())));
    }
    if (request.verb == "verify") {
        if (args.size() != 1) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "verify needs an action id"));
        }
        std::uint64_t id = 0;
        if (!parse_u64(args[0], id)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT, "malformed action id"));
        }
        std::vector<std::uint8_t> bytes;
        if (!from_hex(request.payload, bytes)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT, "payload is not hex"));
        }
        auto evidence = wire::decode_evidence(bytes.data(), bytes.size());
        if (!evidence.has_value()) {
            return fail(evidence);
        }
        auto verification = governor_.verify_action(ActionId{StrongId<ActionIdTag>{id}},
                                                    evidence.value());
        if (!verification.has_value()) {
            return fail(verification);
        }
        auto persisted = persist_now();
        if (!persisted.ok()) {
            return fail(persisted);
        }
        return ok(render_verification(verification.value()));
    }
    if (request.verb == "evaluate" || request.verb == "domain" || request.verb == "headroom" ||
        request.verb == "throttle" || request.verb == "recovery" ||
        request.verb == "temperature" || request.verb == "device") {
        if (args.size() != 1) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "query needs exactly one identifier"));
        }
        std::uint64_t id = 0;
        if (!parse_u64(args[0], id)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT, "malformed identifier"));
        }
        const ThermalDomainId domain{StrongId<ThermalDomainIdTag>{id}};
        if (request.verb == "device") {
            auto snapshot = governor_.snapshot();
            const auto* device = snapshot->find_device(DeviceId{StrongId<DeviceIdTag>{id}});
            if (device == nullptr) {
                return fail(Status::failure(ThermalErrorCode::UNKNOWN_DEVICE,
                                            "device is not registered"));
            }
            std::string out;
            out += "DeviceId: " + std::to_string(id) + "\n";
            out += "DeviceGeneration: " + std::to_string(device->generation.value()) + "\n";
            out += "ThermalState: ";
            out += to_string(device->state);
            out += "\n";
            out += "Provenance: ";
            out += to_string(device->provenance);
            out += "\n";
            if (device->temperature.has_value()) {
                out += "Temperature: " + device->temperature->render() + "\n";
            } else {
                out += "Temperature: <unknown>\n";
            }
            out += device->capabilities.render();
            out += "\n";
            return ok(out);
        }
        if (request.verb == "temperature") {
            auto snapshot = governor_.snapshot();
            const auto* domain_snapshot = snapshot->find_domain(domain);
            if (domain_snapshot == nullptr || !domain_snapshot->evidence.has_value()) {
                return fail(Status::failure(ThermalErrorCode::EVIDENCE_UNKNOWN,
                                            "no current temperature evidence"));
            }
            return ok(domain_snapshot->evidence->temperature.render() + " provenance=" +
                      std::string(to_string(domain_snapshot->evidence->provenance)));
        }
        if (request.verb == "headroom") {
            auto headroom = governor_.query_headroom(domain);
            if (!headroom.has_value()) {
                return fail(headroom);
            }
            return ok(render_headroom(headroom.value()));
        }
        if (request.verb == "throttle") {
            auto envelope = governor_.query_envelope(domain);
            if (!envelope.has_value()) {
                return fail(envelope);
            }
            return ok(std::string(to_string(envelope.value().throttle_class)));
        }
        if (request.verb == "recovery") {
            auto recovery = governor_.evaluate_recovery(domain);
            if (!recovery.has_value()) {
                return fail(recovery);
            }
            return ok(build_recovery_explanation(recovery.value(), ThermalPolicy{}).render());
        }
        if (request.verb == "domain") {
            auto envelope = governor_.query_envelope(domain);
            if (!envelope.has_value()) {
                return fail(envelope);
            }
            return ok(render_envelope(envelope.value()));
        }
        auto evaluation = governor_.evaluate_domain(domain);
        if (!evaluation.has_value()) {
            return fail(evaluation);
        }
        std::string out;
        out += "State: ";
        out += to_string(evaluation.value().state);
        out += "\nDecision: ";
        out += to_string(evaluation.value().decision);
        out += "\nDerating: ";
        out += to_string(evaluation.value().derating);
        out += "\nTemperature: ";
        out += evaluation.value().headroom.current_temperature.render();
        out += "\nHeadroom: ";
        out += std::to_string(evaluation.value().headroom.effective_headroom.value());
        out += "\nThrottle: ";
        out += to_string(evaluation.value().throttle_class);
        out += "\nRecoveryAllowed: ";
        out += evaluation.value().recovery.allowed ? "true" : "false";
        out += "\nProvenance: ";
        out += to_string(evaluation.value().provenance);
        out += "\nReasons: ";
        out += evaluation.value().reasons.render();
        out += "\n";
        return ok(out);
    }
    if (request.verb == "policy") {
        auto policy = governor_.get_policy(ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}});
        if (!policy.has_value()) {
            return fail(policy);
        }
        return ok(render_policy(policy.value()));
    }
    if (request.verb == "explain-admission") {
        if (args.size() < 3) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "explain-admission needs a domain id, a workload id and a profile"));
        }
        std::uint64_t domain_id = 0;
        std::uint64_t workload_id = 0;
        if (!parse_u64(args[0], domain_id) || !parse_u64(args[1], workload_id)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "malformed admission identifier"));
        }
        ThermalAdmissionRequest admission;
        admission.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{domain_id}};
        admission.workload = WorkloadId{StrongId<WorkloadIdTag>{workload_id}};
        for (std::uint32_t i = 0; i < 6; ++i) {
            const auto profile = static_cast<WorkloadThermalProfile>(i);
            if (args[2] == to_string(profile)) {
                admission.profile = profile;
            }
        }
        auto explanation = governor_.explain_admission(admission);
        if (!explanation.has_value()) {
            return fail(explanation);
        }
        return ok(explanation.value().render());
    }
    if (request.verb == "admission") {
        if (args.size() < 3) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "admission needs a domain id, a workload id and a profile"));
        }
        std::uint64_t domain_id = 0;
        std::uint64_t workload_id = 0;
        if (!parse_u64(args[0], domain_id) || !parse_u64(args[1], workload_id)) {
            return fail(Status::failure(ThermalErrorCode::INVALID_ARGUMENT,
                                        "malformed admission identifier"));
        }
        ThermalAdmissionRequest admission;
        admission.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{domain_id}};
        admission.workload = WorkloadId{StrongId<WorkloadIdTag>{workload_id}};
        for (std::uint32_t i = 0; i < 6; ++i) {
            const auto profile = static_cast<WorkloadThermalProfile>(i);
            if (args[2] == to_string(profile)) {
                admission.profile = profile;
            }
        }
        auto result = governor_.evaluate_admission(admission);
        if (!result.has_value()) {
            return fail(result);
        }
        return ok(render_admission(result.value()));
    }

    return fail(Status::failure(ThermalErrorCode::NOT_IMPLEMENTED,
                                "unrecognised coordinator verb: " + request.verb));
}

}  // namespace
}  // namespace thermal_governor::tools

int main(int argc, char** argv) {
    using namespace thermal_governor;
    using namespace thermal_governor::tools;

    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        std::string value;
        if (argument == "--port") {
            if (!option_value(argc, argv, i, value)) {
                return 2;
            }
            std::uint64_t port = 0;
            if (!parse_u64(value, port) || port > 65535) {
                return 2;
            }
            options.port = static_cast<std::uint16_t>(port);
        } else if (argument == "--state") {
            if (!option_value(argc, argv, i, value)) {
                return 2;
            }
            options.state_path = value;
        } else if (argument == "--epoch") {
            if (!option_value(argc, argv, i, value)) {
                return 2;
            }
            if (!parse_u64(value, options.epoch)) {
                return 2;
            }
        } else if (argument == "--ready-file") {
            if (!option_value(argc, argv, i, value)) {
                return 2;
            }
            options.ready_file = value;
        } else {
            std::fprintf(stderr, "unrecognised argument: %s\n", argument.c_str());
            return 2;
        }
    }

    GovernorConfig config;
    config.coordinator = CoordinatorId{StrongId<CoordinatorIdTag>{1}};
    config.initial_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{options.epoch}};
    config.durable_path = options.state_path;

    auto created = ThermalGovernor::create(config);
    if (!created.has_value()) {
        std::fprintf(stderr, "coordinator creation failed: %s\n",
                     created.error().render().c_str());
        return 1;
    }
    std::unique_ptr<ThermalGovernor> governor = std::move(created.value());

    if (!options.state_path.empty()) {
        auto loaded = governor->load_durable();
        if (!loaded.ok()) {
            std::fprintf(stderr, "durable load failed: %s\n", loaded.error().render().c_str());
            return 1;
        }
    }

    auto listener = TcpListener::bind_loopback(options.port);
    if (!listener.has_value()) {
        std::fprintf(stderr, "listen failed: %s\n", listener.error().render().c_str());
        return 1;
    }
    TcpListener server = std::move(listener.value());

    std::printf("COORDINATOR LISTENING port=%u epoch=%llu\n", static_cast<unsigned>(server.bound_port()),
                static_cast<unsigned long long>(governor->epoch().value()));
    std::fflush(stdout);

    if (!options.ready_file.empty() &&
        !write_ready_file(options.ready_file, std::to_string(server.bound_port()))) {
        std::fprintf(stderr, "cannot publish the coordinator ready file\n");
        server.close();
        return 1;
    }

    std::atomic<bool> stopping{false};
    std::mutex threads_mutex;
    std::vector<std::thread> threads;

    // Live session sockets. Shutdown must be able to unblock a session that
    // is idle in a receive, otherwise joining the session threads would wait
    // forever on a client that has nothing more to say.
    std::mutex sessions_mutex;
    std::set<TcpSocket*> sessions;

    const auto interrupt_sessions = [&sessions, &sessions_mutex]() {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        for (TcpSocket* session : sessions) {
            session->shutdown_both();
        }
    };

    for (;;) {
        auto accepted = server.accept_one(&stopping);
        if (!accepted.has_value()) {
            break;
        }
        TcpSocket socket = std::move(accepted.value());
        {
            std::lock_guard<std::mutex> lock(threads_mutex);
            if (threads.size() >= options.max_connections) {
                std::fprintf(stderr, "connection limit reached; refusing a session\n");
                continue;
            }
            threads.emplace_back([socket = std::move(socket), &governor, &stopping,
                                  &sessions, &sessions_mutex,
                                  state_path = options.state_path]() mutable {
                CoordinatorSession session(*governor, state_path);
                // The registry entry is released on every exit path,
                // including every early return, so a retired session can
                // never leave a dangling pointer behind.
                struct SessionGuard {
                    std::mutex* mutex;
                    std::set<TcpSocket*>* registry;
                    TcpSocket* socket;
                    ~SessionGuard() {
                        std::lock_guard<std::mutex> lock(*mutex);
                        registry->erase(socket);
                    }
                };
                {
                    std::lock_guard<std::mutex> lock(sessions_mutex);
                    sessions.insert(&socket);
                }
                const SessionGuard guard{&sessions_mutex, &sessions, &socket};
                // A session that begins after shutdown was requested must
                // not linger.
                if (stopping.load()) {
                    return;
                }
                for (;;) {
                    if (stopping.load()) {
                        return;
                    }
                    auto frame = socket.recv_frame();
                    if (!frame.has_value()) {
                        return;
                    }
                    if (frame.value().kind == MessageKind::HELLO) {
                        auto registration = wire::decode_worker_registration(
                            frame.value().payload.data(), frame.value().payload.size());
                        CommandResponse response;
                        if (!registration.has_value()) {
                            response.status.code = registration.error().code;
                            response.status.detail = registration.error().detail;
                        } else {
                            auto status = governor->register_worker(
                                registration.value().worker, registration.value().boot,
                                Label{registration.value().label});
                            if (status.ok()) {
                                for (const auto& device : registration.value().devices) {
                                    auto device_status = governor->register_device(device);
                                    if (!device_status.ok()) {
                                        status = device_status;
                                        break;
                                    }
                                }
                            }
                            response.status.code = status.ok() ? ThermalErrorCode::NONE
                                                               : status.code();
                            response.status.detail = status.error().detail;
                            response.payload = "epoch=" +
                                               std::to_string(governor->epoch().value());
                            if (status.ok() && !state_path.empty() &&
                                !governor->persist().ok()) {
                                std::fprintf(stderr, "durable persistence failed after registration\n");
                            }
                        }
                        auto encoded = wire::encode_response(response);
                        if (!send_bytes(socket, MessageKind::HELLO_ACK, 0, encoded).ok()) {
                            return;
                        }
                        (void)send_end(socket);
                        continue;
                    }
                    if (frame.value().kind == MessageKind::PUBLISH_EVIDENCE) {
                        auto evidence = wire::decode_evidence(frame.value().payload.data(),
                                                              frame.value().payload.size());
                        CommandResponse response;
                        wire::EvaluationSummary summary;
                        bool have_summary = false;
                        if (!evidence.has_value()) {
                            response.status.code = evidence.error().code;
                            response.status.detail = evidence.error().detail;
                        } else {
                            auto receipt = governor->publish_temperature(evidence.value());
                            if (!receipt.has_value()) {
                                response.status.code = receipt.error().code;
                                response.status.detail = receipt.error().detail;
                            } else {
                                response.status.code = ThermalErrorCode::NONE;
                                response.payload = "accepted=1";
                                std::vector<ThermalDomainId> affected =
                                    receipt.value().affected_domains;
                                if (!affected.empty()) {
                                    auto evaluation = governor->evaluate_domain(affected.front());
                                    if (evaluation.has_value()) {
                                        // Report the most restrictive affected domain.
                                        summary = wire::summarise(evaluation.value());
                                        have_summary = true;
                                        for (const auto domain : affected) {
                                            auto candidate = governor->evaluate_domain(domain);
                                            if (!candidate.has_value()) {
                                                continue;
                                            }
                                            if (severity(candidate.value().state) >
                                                severity(summary.state)) {
                                                summary = wire::summarise(candidate.value());
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        auto encoded = wire::encode_response(response);
                        if (!send_bytes(socket, MessageKind::PUBLISH_ACK, 0, encoded).ok()) {
                            return;
                        }
                        if (have_summary) {
                            auto summary_bytes = wire::encode_evaluation_summary(summary);
                            if (!send_bytes(socket, MessageKind::EVALUATE_RESPONSE, 0,
                                            summary_bytes)
                                     .ok()) {
                                return;
                            }
                        }
                        (void)send_end(socket);
                        continue;
                    }
                    if (frame.value().kind == MessageKind::INTENT_ACK ||
                        frame.value().kind == MessageKind::INTENT_RESULT) {
                        CommandResponse response;
                        Status status = Status::success();
                        if (frame.value().kind == MessageKind::INTENT_ACK) {
                            auto ack = wire::decode_action_ack(frame.value().payload.data(),
                                                               frame.value().payload.size());
                            if (!ack.has_value()) {
                                response.status.code = ack.error().code;
                                response.status.detail = ack.error().detail;
                            } else {
                                auto lifecycle = governor->record_action_ack(ack.value());
                                if (!lifecycle.has_value()) {
                                    status = Status(lifecycle.error());
                                } else {
                                    response.payload = to_string(lifecycle.value());
                                }
                            }
                        } else {
                            auto result = wire::decode_action_result(frame.value().payload.data(),
                                                                     frame.value().payload.size());
                            if (!result.has_value()) {
                                response.status.code = result.error().code;
                                response.status.detail = result.error().detail;
                            } else {
                                auto lifecycle = governor->record_action_result(result.value());
                                if (!lifecycle.has_value()) {
                                    status = Status(lifecycle.error());
                                } else {
                                    response.payload = to_string(lifecycle.value());
                                }
                            }
                        }
                        if (!status.ok()) {
                            response.status.code = status.code();
                            response.status.detail = status.error().detail;
                        }
                        auto encoded = wire::encode_response(response);
                        if (!send_bytes(socket, MessageKind::INTENT_RESULT, 0, encoded).ok()) {
                            return;
                        }
                        (void)send_end(socket);
                        continue;
                    }
                    if (frame.value().kind == MessageKind::HEARTBEAT) {
                        auto encoded = wire::encode_status(StatusPayload{});
                        if (!send_bytes(socket, MessageKind::HEARTBEAT, 0, encoded).ok()) {
                            return;
                        }
                        (void)send_end(socket);
                        continue;
                    }
                    if (frame.value().kind == MessageKind::QUERY_REQUEST) {
                        auto command = wire::decode_command(frame.value().payload.data(),
                                                            frame.value().payload.size());
                        CommandResponse response;
                        if (!command.has_value()) {
                            response.status.code = command.error().code;
                            response.status.detail = command.error().detail;
                        } else if (command.value().verb == "shutdown") {
                            response = session.handle(command.value());
                            auto encoded = wire::encode_response(response);
                            (void)send_bytes(socket, MessageKind::QUERY_RESPONSE, 0, encoded);
                            (void)send_end(socket);
                            // The accept loop owns the listener: this thread
                            // only raises the cooperative stop flag and then
                            // returns, so the joining thread is never this
                            // thread and no socket is closed underneath a
                            // concurrent select().
                            stopping.store(true);
                            socket.close();
                            return;
                        } else {
                            response = session.handle(command.value());
                        }
                        auto encoded = wire::encode_response(response);
                        if (!send_bytes(socket, MessageKind::QUERY_RESPONSE, 0, encoded).ok()) {
                            return;
                        }
                        (void)send_end(socket);
                        continue;
                    }
                    if (frame.value().kind == MessageKind::DISCONNECT) {
                        return;
                    }
                    // An unrecognised frame kind is a protocol error, not
                    // something to ignore silently.
                    auto encoded = wire::encode_status(StatusPayload{
                        ThermalErrorCode::PROTOCOL_UNSUPPORTED, "unexpected frame kind"});
                    if (!send_bytes(socket, MessageKind::ERROR_RESPONSE, 0, encoded).ok()) {
                        return;
                    }
                    (void)send_end(socket);
                }
            });
        }
    }

    stopping.store(true);
    interrupt_sessions();
    {
        std::lock_guard<std::mutex> lock(threads_mutex);
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads.clear();
    }
    server.close();
    std::printf("COORDINATOR STOPPED\n");
    std::fflush(stdout);
    return 0;
}
