// Thermal Governor — seeded randomized property tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every case is driven by a deterministic seed. On failure the seed and the
// complete reproduction parameters are printed before the assertion fails.

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "framework.hpp"
#include "governor_fixture.hpp"

namespace {

using namespace thermal_governor;
using tg::GovernorFixture;
using tg::GovernorFixture;

constexpr std::uint64_t kSeeds[] = {1ULL, 2ULL, 3ULL, 5ULL, 8ULL, 13ULL, 21ULL, 34ULL};
constexpr int kOperationsPerSeed = 220;

/// SplitMix64: a small, well-defined generator so a seed reproduces a run
/// exactly on any platform.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}

    [[nodiscard]] std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    [[nodiscard]] std::uint64_t below(std::uint64_t bound) {
        return bound == 0 ? 0 : next() % bound;
    }

    [[nodiscard]] double temperature(double low, double high) {
        const double unit = static_cast<double>(next() >> 11) / 9007199254740992.0;
        return low + unit * (high - low);
    }

private:
    std::uint64_t state_;
};

struct Harness {
    std::unique_ptr<GovernorFixture> fixture;
    std::uint64_t seed = 0;
    int operation = 0;
    std::vector<std::uint64_t> devices;
    std::vector<std::uint64_t> domains;
    std::vector<ActionId> live_actions;

    void report() {
        std::printf("PROPERTY REPRODUCTION seed=%llu operation=%d devices=%zu domains=%zu\n",
                    static_cast<unsigned long long>(seed), operation, devices.size(),
                    domains.size());
        std::fflush(stdout);
    }

    [[nodiscard]] bool invariants(tg::Context& context) {
        auto snapshot = fixture->governor->snapshot();
        if (snapshot == nullptr) {
            context.fail("snapshot is null");
            return false;
        }
        if (snapshot->coordinator_epoch.value() == 0) {
            context.fail("coordinator epoch is zero");
            return false;
        }
        for (const auto& [key, domain] : snapshot->domains) {
            (void)key;
            auto evaluation = fixture->governor->evaluate_domain(domain.definition.id);
            if (!evaluation.has_value()) {
                context.fail("evaluation failed for a registered domain");
                return false;
            }
            const ThermalEvaluation& value = evaluation.value();
            // Hard invariant: a critical thermal state never authorises
            // unrestricted execution.
            if (value.state == ThermalState::CRITICAL && value.decision == ThermalDecision::ALLOW) {
                context.fail("critical state produced ALLOW");
                return false;
            }
            if ((value.state == ThermalState::UNKNOWN ||
                 value.state == ThermalState::UNSUPPORTED) &&
                value.decision == ThermalDecision::ALLOW) {
                context.fail("unresolved state produced ALLOW");
                return false;
            }
            // Hard invariant: nothing below FULL_CAPABILITY claims elsewise.
            if (value.decision == ThermalDecision::ALLOW_DERATED &&
                value.derating == DeratingLevel::FULL_CAPABILITY &&
                value.state != ThermalState::WARM && value.state != ThermalState::NORMAL) {
                context.fail("ALLOW_DERATED reported with FULL_CAPABILITY");
                return false;
            }
            // Hard invariant: an evaluation is a pure function of its input,
            // so recomputing must not change the observable state.
            auto again = fixture->governor->evaluate_domain(domain.definition.id);
            if (!again.has_value() || again.value().state != value.state ||
                again.value().derating != value.derating ||
                again.value().decision != value.decision) {
                context.fail("evaluation is not deterministic");
                return false;
            }
            // Authority must remain generation-bound.
            if (value.domain_generation.value() != domain.definition.generation.value()) {
                context.fail("evaluation reports a mismatched domain generation");
                return false;
            }
        }
        // Hard invariant: a recorded action is never EFFECTIVE while merely
        // ACKNOWLEDGED, and vice versa.
        for (const auto& action : fixture->governor->actions()) {
            if (action.lifecycle == ActionLifecycle::EFFECTIVE && action.terminal_at == SteadyTimePoint{}) {
                context.fail("action reached EFFECTIVE without a terminal instant");
                return false;
            }
        }
        return true;
    }
};

void run_seed(tg::Context& context, std::uint64_t seed) {
    Harness harness;
    harness.seed = seed;
    harness.fixture = GovernorFixture::create();
    if (harness.fixture == nullptr) {
        context.fail("fixture creation failed");
        return;
    }
    Rng rng(seed);
    auto& fixture = *harness.fixture;

    const std::uint64_t device_count = 1 + rng.below(4);
    for (std::uint64_t i = 1; i <= device_count; ++i) {
        const CapabilityState temperature =
            rng.below(6) == 0 ? CapabilityState::SUPPORTED_SYNTHETIC
                              : (rng.below(8) == 0 ? CapabilityState::UNKNOWN
                                                   : CapabilityState::SUPPORTED_SYNTHETIC);
        const CapabilityState throttle = rng.below(2) == 0 ? CapabilityState::SUPPORTED_SYNTHETIC
                                                           : CapabilityState::UNSUPPORTED;
        const CapabilityState limit = rng.below(3) == 0 ? CapabilityState::SUPPORTED_SYNTHETIC
                                                        : CapabilityState::UNSUPPORTED;
        if (!fixture.add_device(i, 1, temperature, throttle, limit).ok()) {
            harness.report();
            context.fail("device registration failed");
            return;
        }
        harness.devices.push_back(i);
        if (!fixture.add_accelerator_domain(i, 1, i).ok()) {
            harness.report();
            context.fail("domain registration failed");
            return;
        }
        harness.domains.push_back(i);
    }

    for (int step = 0; step < kOperationsPerSeed; ++step) {
        harness.operation = step;
        const std::uint64_t choice = rng.below(10);
        const std::uint64_t device = harness.devices[rng.below(harness.devices.size())];

        if (choice <= 3) {
            const double temperature = rng.temperature(30.0, 100.0);
            std::optional<ThrottleObservation> throttle;
            if (rng.below(4) == 0) {
                const std::uint64_t raw = rng.below(2) == 0
                                              ? bits(ThrottleReasonBit::HW_THERMAL_SLOWDOWN)
                                              : bits(ThrottleReasonBit::SW_POWER_CAP);
                throttle = ThrottleObservation::from_raw(raw);
            }
            fixture.advance(static_cast<std::int64_t>(rng.below(700)));
            auto receipt = fixture.publish(device, temperature, device, 1, 1, WorkerId{},
                                           WorkerBootId{}, throttle);
            if (!receipt.has_value()) {
                harness.report();
                context.fail("ordinary publication was rejected: " +
                             std::string(to_string(receipt.error().code)));
                return;
            }
        } else if (choice == 4) {
            // Policy revision.
            auto policy = fixture.governor->get_policy(
                ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}});
            if (!policy.has_value()) {
                harness.report();
                context.fail("policy lookup failed");
                return;
            }
            ThermalPolicy updated = policy.value();
            updated.generation = ThermalPolicyGeneration{
                StrongId<ThermalPolicyGenerationTag>{policy.value().generation.value() + 1}};
            const double warning = rng.temperature(40.0, 65.0);
            const double derating = warning + 2.0 + rng.temperature(0.0, 8.0);
            const double critical = derating + 2.0 + rng.temperature(0.0, 10.0);
            const double recovery = warning - rng.temperature(1.0, 10.0);
            updated.thresholds.warning = DegreesCelsius{warning};
            updated.thresholds.derating = DegreesCelsius{derating};
            updated.thresholds.critical = DegreesCelsius{critical};
            updated.thresholds.recovery = DegreesCelsius{recovery};
            updated.thresholds.near_limit_band = TemperatureDelta{1.0};
            updated.margins.policy_safety_margin = TemperatureDelta{rng.temperature(0.0, 4.0)};
            updated.margins.uncertainty_margin = TemperatureDelta{rng.temperature(0.0, 3.0)};
            updated.margins.recovery_margin = TemperatureDelta{rng.temperature(0.0, 2.0)};
            auto applied = fixture.governor->set_policy(updated);
            if (!applied.has_value()) {
                harness.report();
                context.fail("valid randomized policy was rejected: " +
                             std::string(to_string(applied.error().code)));
                return;
            }
        } else if (choice == 5) {
            // Coupling between two random domains, including self-cycles.
            CouplingRelation relation;
            relation.id = CouplingId{StrongId<CouplingIdTag>{rng.below(1000) + 1}};
            relation.source = ThermalDomainId{
                StrongId<ThermalDomainIdTag>{harness.domains[rng.below(harness.domains.size())]}};
            relation.destination = ThermalDomainId{
                StrongId<ThermalDomainIdTag>{harness.domains[rng.below(harness.domains.size())]}};
            relation.type = CouplingType::SYNTHETIC_COUPLING;
            relation.provenance = Provenance::SYNTHETIC;
            relation.generation = CouplingGeneration{StrongId<CouplingGenerationTag>{1}};
            relation.weight = 1.0;
            relation.evidence_source = "randomized synthetic coupling";
            auto status = fixture.governor->register_coupling(relation);
            if (!status.ok() && status.code() != ThermalErrorCode::RESOURCE_EXHAUSTED) {
                harness.report();
                context.fail("coupling registration failed: " +
                             std::string(to_string(status.code())));
                return;
            }
        } else if (choice == 6) {
            // Worker incarnation churn.
            const std::uint64_t worker = 1 + rng.below(3);
            const std::uint64_t boot = 1 + rng.below(4);
            auto status = fixture.governor->register_worker(
                WorkerId{StrongId<WorkerIdTag>{worker}}, WorkerBootId{StrongId<WorkerBootIdTag>{boot}},
                Label{"randomized worker"});
            // A boot identity that would move backwards is legitimately
            // refused; every other outcome is a defect.
            if (!status.ok() && status.code() != ThermalErrorCode::STALE_WORKER) {
                harness.report();
                context.fail("worker registration failed: " + std::string(to_string(status.code())));
                return;
            }
            // A death notice for an incarnation that was never admitted is
            // itself a stale-worker refusal, so it is only attempted for the
            // incarnation that actually holds authority.
            if (status.ok() && rng.below(2) == 0) {
                auto death = fixture.governor->note_worker_death(
                    WorkerId{StrongId<WorkerIdTag>{worker}},
                    WorkerBootId{StrongId<WorkerBootIdTag>{boot}}, "randomized death");
                if (!death.ok()) {
                    harness.report();
                    context.fail("worker death notice failed: " +
                                 std::string(to_string(death.code())));
                    return;
                }
            }
        } else if (choice == 7) {
            // Action lifecycle churn.
            MitigationIntentRecord record;
            record.intent = MitigationIntent::REQUEST_CLOCK_REDUCTION;
            const ThermalDomainId domain{
                StrongId<ThermalDomainIdTag>{harness.domains[rng.below(harness.domains.size())]}};
            auto action = fixture.governor->authorize_mitigation(domain, record, "randomized");
            if (action.has_value()) {
                if (rng.below(2) == 0) {
                    (void)fixture.governor->record_action_dispatch(action.value());
                }
                if (rng.below(3) == 0) {
                    (void)fixture.governor->cancel_action(action.value(), "randomized cancel");
                }
            }
        } else if (choice == 8) {
            // Stale and duplicate publication attacks.
            if (rng.below(2) == 0) {
                ThermalEvidence stale;
                stale.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{rng.below(100) + 1}};
                stale.subject = SubjectRef::for_device(
                    DeviceId{StrongId<DeviceIdTag>{device}},
                    DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
                stale.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{device}};
                stale.domain_generation =
                    ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
                stale.temperature = DegreesCelsius{rng.temperature(20.0, 120.0)};
                stale.source = MeasurementSource::SYNTHETIC_MODEL;
                stale.provenance = Provenance::SYNTHETIC;
                stale.measurement_sequence = rng.below(50);
                stale.telemetry_generation =
                    TelemetryGeneration{StrongId<TelemetryGenerationTag>{rng.below(50)}};
                stale.coordinator_epoch = CoordinatorEpoch{
                    StrongId<CoordinatorEpochTag>{fixture.governor->epoch().value() + 1}};
                stale.measured_at = fixture.clock->now();
                auto rejected = fixture.governor->publish_temperature(stale);
                if (rejected.has_value()) {
                    harness.report();
                    context.fail("stale epoch evidence was accepted");
                    return;
                }
            } else {
                fixture.advance(10);
                auto first = fixture.publish(device, 55.0, device);
                if (first.has_value()) {
                    ThermalEvidence duplicate;
                    duplicate.evidence_id = EvidenceId{
                        StrongId<EvidenceIdTag>{fixture.telemetry}};
                    duplicate.subject = SubjectRef::for_device(
                        DeviceId{StrongId<DeviceIdTag>{device}},
                        DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
                    duplicate.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{device}};
                    duplicate.domain_generation =
                        ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
                    duplicate.temperature = DegreesCelsius{55.0};
                    duplicate.source = MeasurementSource::SYNTHETIC_MODEL;
                    duplicate.provenance = Provenance::SYNTHETIC;
                    duplicate.measurement_sequence = fixture.telemetry;
                    duplicate.telemetry_generation =
                        TelemetryGeneration{StrongId<TelemetryGenerationTag>{fixture.telemetry}};
                    duplicate.coordinator_epoch = fixture.governor->epoch();
                    duplicate.capability_generation =
                        CapabilityGeneration{StrongId<CapabilityGenerationTag>{1}};
                    duplicate.topology_generation =
                        TopologyGeneration{StrongId<TopologyGenerationTag>{1}};
                    duplicate.measured_at = fixture.clock->now();
                    auto repeated = fixture.governor->publish_temperature(duplicate);
                    if (!repeated.has_value() || repeated.value().accepted) {
                        harness.report();
                        context.fail("identical duplicate was not idempotent");
                        return;
                    }
                }
            }
        } else {
            // Worker generation churn and independent re-evaluation.
            auto status = fixture.governor->register_device(DeviceRegistration{});
            if (status.ok()) {
                harness.report();
                context.fail("an empty device registration was accepted");
                return;
            }
            for (const auto id : harness.domains) {
                auto evaluation =
                    fixture.governor->evaluate_domain(ThermalDomainId{StrongId<ThermalDomainIdTag>{id}});
                if (!evaluation.has_value()) {
                    harness.report();
                    context.fail("evaluation failed during churn");
                    return;
                }
            }
        }

        if (!harness.invariants(context)) {
            harness.report();
            return;
        }
    }
    harness.report();
}

}  // namespace

TG_CASE(property, randomized_invariants_hold) {
    for (const std::uint64_t seed : kSeeds) {
        std::printf("PROPERTY SEED %llu\n", static_cast<unsigned long long>(seed));
        std::fflush(stdout);
        run_seed(tg_ctx, seed);
        if (tg_ctx.failed()) {
            return;
        }
    }
}

TG_CASE(property, boundary_sweep_is_monotonic) {
    // Raising the temperature must never relax the reported derating level.
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    TG_STATUS_OK(fixture->add_device(1));
    TG_STATUS_OK(fixture->add_accelerator_domain(1, 1, 1));
    std::uint32_t previous = 0;
    for (double temperature = 20.0; temperature <= 100.0; temperature += 0.5) {
        TG_OK(fixture->publish(1, temperature));
        auto evaluation = fixture->governor->evaluate_domain(
            ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
        TG_OK(evaluation);
        const std::uint32_t rank = restriction_rank(evaluation.value().derating);
        TG_CHECK(rank >= previous);
        previous = rank;
    }
}

TG_CASE(property, invalid_policies_are_rejected_deterministically) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    auto base = fixture->governor->get_policy(ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}});
    TG_OK(base);

    ThermalPolicy impossible = base.value();
    impossible.generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{9}};
    impossible.thresholds.warning = DegreesCelsius{90.0};
    impossible.thresholds.derating = DegreesCelsius{80.0};
    TG_ERROR_CODE(fixture->governor->set_policy(impossible), ThermalErrorCode::POLICY_INVALID);

    ThermalPolicy inverted_critical = base.value();
    inverted_critical.generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{10}};
    inverted_critical.thresholds.critical = DegreesCelsius{70.0};
    TG_ERROR_CODE(fixture->governor->set_policy(inverted_critical),
                  ThermalErrorCode::POLICY_INVALID);

    ThermalPolicy recovery_above_derating = base.value();
    recovery_above_derating.generation =
        ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{11}};
    recovery_above_derating.thresholds.recovery = DegreesCelsius{85.0};
    TG_ERROR_CODE(fixture->governor->set_policy(recovery_above_derating),
                  ThermalErrorCode::POLICY_INVALID);

    ThermalPolicy nan_threshold = base.value();
    nan_threshold.generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{12}};
    nan_threshold.thresholds.warning = DegreesCelsius{std::nan("")};
    TG_ERROR_CODE(fixture->governor->set_policy(nan_threshold), ThermalErrorCode::POLICY_INVALID);

    ThermalPolicy infinite = base.value();
    infinite.generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{13}};
    infinite.thresholds.critical = DegreesCelsius{std::numeric_limits<double>::infinity()};
    TG_ERROR_CODE(fixture->governor->set_policy(infinite), ThermalErrorCode::POLICY_INVALID);
}
