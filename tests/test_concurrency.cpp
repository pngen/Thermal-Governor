// Thermal Governor — concurrency and deterministic race tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every case uses explicit synchronisation (std::barrier, atomics, condition
// variables). No case sleeps, and no case is given a timeout.

#include <atomic>
#include <barrier>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "framework.hpp"
#include "governor_fixture.hpp"
#include "process_util.hpp"
#include "thermal_governor/persistence.hpp"

namespace {

using namespace thermal_governor;
using tg::GovernorFixture;
using tg::GovernorFixture;

constexpr std::uint64_t kDeviceCount = 8;
constexpr int kPublishesPerDevice = 40;

/// A fixture with several independent accelerator domains.
[[nodiscard]] std::unique_ptr<GovernorFixture> make_multi_device_fixture() {
    auto fixture = GovernorFixture::create();
    if (fixture == nullptr) {
        return nullptr;
    }
    for (std::uint64_t i = 1; i <= kDeviceCount; ++i) {
        if (!fixture->add_device(i).ok()) {
            return nullptr;
        }
        if (!fixture->add_accelerator_domain(i, 1, i).ok()) {
            return nullptr;
        }
    }
    return fixture;
}

/// Build evidence for a device without touching shared fixture state.
[[nodiscard]] ThermalEvidence build_evidence(SteadyTimePoint now, CoordinatorEpoch epoch,
                                             std::uint64_t device, std::uint64_t sequence,
                                             double temperature) {
    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{sequence}};
    evidence.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{device}},
                                              DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    evidence.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{device}};
    evidence.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    evidence.temperature = DegreesCelsius{temperature};
    evidence.source = MeasurementSource::SYNTHETIC_MODEL;
    evidence.provenance = Provenance::SYNTHETIC;
    evidence.measurement_sequence = sequence;
    evidence.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{sequence}};
    evidence.coordinator_epoch = epoch;
    evidence.measured_at = now;
    evidence.measured_wall_at = WallTimePoint{};
    return evidence;
}

}  // namespace

TG_CASE(concurrency, concurrent_publish_evaluate_snapshot_and_policy) {
    TG_PHASE("CONCURRENCY_SETUP");
    auto fixture = make_multi_device_fixture();
    TG_CHECK(fixture != nullptr);
    auto* governor = fixture->governor.get();
    const SteadyTimePoint start = fixture->clock->now();
    const CoordinatorEpoch epoch = governor->epoch();

    constexpr int kPublishers = 4;
    constexpr int kEvaluators = 3;
    constexpr int kSnapshotReaders = 2;
    constexpr int kExplainers = 2;
    constexpr int kPolicyWriters = 1;
    constexpr int kThreads =
        kPublishers + kEvaluators + kSnapshotReaders + kExplainers + kPolicyWriters + 1;

    std::barrier start_gate(kThreads);
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> evaluations{0};
    std::atomic<std::uint64_t> snapshots{0};
    std::atomic<std::uint64_t> explanations{0};
    std::atomic<std::uint64_t> policy_updates{0};
    std::atomic<bool> failed{false};
    std::string failure;

    const auto note_failure = [&failed, &failure](const std::string& text) {
        bool expected = false;
        if (failed.compare_exchange_strong(expected, true)) {
            failure = text;
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(kThreads));

    for (int worker = 0; worker < kPublishers; ++worker) {
        threads.emplace_back([&, worker]() {
            start_gate.arrive_and_wait();
            for (int step = 0; step < kPublishesPerDevice; ++step) {
                const std::uint64_t device =
                    static_cast<std::uint64_t>(worker) + 1 +
                    static_cast<std::uint64_t>(step % 2) * static_cast<std::uint64_t>(kPublishers);
                if (device > kDeviceCount) {
                    continue;
                }
                const std::uint64_t sequence =
                    static_cast<std::uint64_t>(worker * 1000 + step + 1);
                const double temperature = 40.0 + static_cast<double>((worker * 7 + step) % 40);
                auto receipt = governor->publish_temperature(
                    build_evidence(start, epoch, device, sequence, temperature));
                if (receipt.has_value()) {
                    accepted.fetch_add(1);
                } else {
                    rejected.fetch_add(1);
                }
            }
        });
    }
    for (int worker = 0; worker < kEvaluators; ++worker) {
        threads.emplace_back([&, worker]() {
            start_gate.arrive_and_wait();
            for (int step = 0; step < 200; ++step) {
                const std::uint64_t device =
                    static_cast<std::uint64_t>((worker + step) % kDeviceCount) + 1;
                auto evaluation = governor->evaluate_domain(
                    ThermalDomainId{StrongId<ThermalDomainIdTag>{device}});
                if (!evaluation.has_value()) {
                    note_failure("evaluation failed during concurrency stress");
                    return;
                }
                if (evaluation.value().state == ThermalState::CRITICAL &&
                    evaluation.value().decision == ThermalDecision::ALLOW) {
                    note_failure("critical state produced ALLOW under concurrency");
                    return;
                }
                evaluations.fetch_add(1);
            }
        });
    }
    for (int worker = 0; worker < kSnapshotReaders; ++worker) {
        threads.emplace_back([&]() {
            start_gate.arrive_and_wait();
            for (int step = 0; step < 200; ++step) {
                auto snapshot = governor->snapshot();
                if (snapshot == nullptr) {
                    note_failure("snapshot was null under concurrency");
                    return;
                }
                if (snapshot->coordinator_epoch.value() == 0) {
                    note_failure("snapshot carried a zero epoch");
                    return;
                }
                snapshots.fetch_add(1);
            }
        });
    }
    for (int worker = 0; worker < kExplainers; ++worker) {
        threads.emplace_back([&, worker]() {
            start_gate.arrive_and_wait();
            for (int step = 0; step < 60; ++step) {
                const std::uint64_t device =
                    static_cast<std::uint64_t>((worker + step) % kDeviceCount) + 1;
                auto explanation = governor->explain(
                    ThermalDomainId{StrongId<ThermalDomainIdTag>{device}});
                if (!explanation.has_value()) {
                    note_failure("explanation failed during concurrency stress");
                    return;
                }
                if (explanation.value().render().empty()) {
                    note_failure("explanation rendered empty");
                    return;
                }
                explanations.fetch_add(1);
            }
        });
    }
    threads.emplace_back([&]() {
        start_gate.arrive_and_wait();
        for (int step = 0; step < 12; ++step) {
            auto policy = governor->get_policy(
                ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}});
            if (!policy.has_value()) {
                note_failure("policy lookup failed under concurrency");
                return;
            }
            ThermalPolicy updated = policy.value();
            updated.generation = ThermalPolicyGeneration{
                StrongId<ThermalPolicyGenerationTag>{policy.value().generation.value() + 1}};
            updated.thresholds.warning = DegreesCelsius{70.0 + static_cast<double>(step % 5)};
            updated.thresholds.derating = DegreesCelsius{80.0 + static_cast<double>(step % 5)};
            updated.thresholds.critical = DegreesCelsius{90.0 + static_cast<double>(step % 5)};
            updated.thresholds.recovery = DegreesCelsius{60.0};
            auto applied = governor->set_policy(updated);
            if (!applied.has_value()) {
                note_failure("concurrent policy update was rejected");
                return;
            }
            policy_updates.fetch_add(1);
        }
    });
    // Persistence runs concurrently with everything else.
    const std::string path = tg::unique_temp_path("tg-concurrency");
    tg::remove_file(path);
    // A second governor owns the durable file. It outlives every thread, so
    // the persistence thread can never observe a dangling runtime.
    GovernorConfig store_config;
    store_config.coordinator = CoordinatorId{StrongId<CoordinatorIdTag>{2}};
    store_config.clock = fixture->clock;
    store_config.durable_path = path;
    auto store_owner = ThermalGovernor::create(std::move(store_config));
    TG_CHECK(store_owner.has_value());
    auto* store = store_owner.value().get();
    threads.emplace_back([&, store]() {
        start_gate.arrive_and_wait();
        for (int step = 0; step < 40; ++step) {
            auto status = store->persist();
            if (!status.ok()) {
                note_failure("persistence failed under concurrency: " +
                             status.error().render());
                return;
            }
        }
    });

    TG_PHASE("CONCURRENCY_RUN");
    for (auto& thread : threads) {
        thread.join();
    }
    TG_PHASE("CONCURRENCY_DRAIN");
    tg::remove_file(path);

    TG_CHECK(!failed.load());
    if (failed.load()) {
        TG_FAIL(failure);
    }
    TG_CHECK(accepted.load() > 0);
    TG_CHECK_EQ(accepted.load() + rejected.load(),
                static_cast<std::uint64_t>(kPublishers * kPublishesPerDevice));
    TG_CHECK(evaluations.load() == static_cast<std::uint64_t>(kEvaluators * 200));
    TG_CHECK(snapshots.load() == static_cast<std::uint64_t>(kSnapshotReaders * 200));
    TG_CHECK(explanations.load() == static_cast<std::uint64_t>(kExplainers * 60));
    TG_CHECK(policy_updates.load() == 12U);

    // After the storm the runtime must still be exactly consistent.
    auto snapshot = governor->snapshot();
    TG_CHECK(snapshot != nullptr);
    TG_CHECK_EQ(snapshot->coordinator_epoch.value(), epoch.value());
    TG_CHECK_EQ(snapshot->domains.size(), kDeviceCount);
}

TG_CASE(concurrency, deterministic_race_on_identical_evidence) {
    TG_PHASE("RACE_SETUP");
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    TG_STATUS_OK(fixture->add_device(1));
    TG_STATUS_OK(fixture->add_accelerator_domain(1, 1, 1));
    auto* governor = fixture->governor.get();
    const ThermalEvidence evidence =
        build_evidence(fixture->clock->now(), governor->epoch(), 1, 1, 55.0);

    constexpr int kRacers = 6;
    std::barrier gate(kRacers);
    std::atomic<int> accepted{0};
    std::atomic<int> idempotent{0};
    std::atomic<int> conflicted{0};
    std::atomic<int> other{0};

    std::vector<std::thread> threads;
    threads.reserve(kRacers);
    for (int i = 0; i < kRacers; ++i) {
        threads.emplace_back([&]() {
            gate.arrive_and_wait();
            auto receipt = governor->publish_temperature(evidence);
            if (!receipt.has_value()) {
                if (receipt.error().code == ThermalErrorCode::DUPLICATE_CONFLICT) {
                    conflicted.fetch_add(1);
                } else {
                    other.fetch_add(1);
                }
                return;
            }
            if (receipt.value().accepted) {
                accepted.fetch_add(1);
            } else {
                idempotent.fetch_add(1);
            }
        });
    }
    TG_PHASE("RACE_RUN");
    for (auto& thread : threads) {
        thread.join();
    }
    TG_PHASE("RACE_CHECK");
    // Exactly one racer may install the observation; every other racer sees
    // either an idempotent duplicate or an explicit conflict.
    TG_CHECK_EQ(accepted.load(), 1);
    TG_CHECK_EQ(idempotent.load() + conflicted.load() + other.load(), kRacers - 1);
    TG_CHECK_EQ(other.load(), 0);
}

TG_CASE(concurrency, concurrent_telemetry_and_persistence_round_trip) {
    TG_PHASE("DURABILITY_SETUP");
    const std::string path = tg::unique_temp_path("tg-durability");
    tg::remove_file(path);
    auto fixture = GovernorFixture::create_with_state(path);
    TG_CHECK(fixture != nullptr);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        TG_STATUS_OK(fixture->add_device(i));
        TG_STATUS_OK(fixture->add_accelerator_domain(i, 1, i));
    }
    auto* governor = fixture->governor.get();
    const SteadyTimePoint start = fixture->clock->now();
    const CoordinatorEpoch epoch = governor->epoch();

    constexpr int kThreads = 4;
    std::barrier gate(kThreads);
    std::atomic<bool> failed{false};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&, i]() {
            gate.arrive_and_wait();
            for (int step = 0; step < 50; ++step) {
                const std::uint64_t device = static_cast<std::uint64_t>(i) + 1;
                (void)governor->publish_temperature(build_evidence(
                    start, epoch, device, static_cast<std::uint64_t>(i * 1000 + step + 1),
                    50.0 + static_cast<double>(step % 20)));
            }
        });
    }
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&]() {
            gate.arrive_and_wait();
            for (int step = 0; step < 50; ++step) {
                if (!governor->persist().ok()) {
                    failed.store(true);
                    return;
                }
            }
        });
    }
    TG_PHASE("DURABILITY_RUN");
    for (auto& thread : threads) {
        thread.join();
    }
    TG_PHASE("DURABILITY_VERIFY");
    TG_CHECK(!failed.load());

    auto snapshot = governor->snapshot();
    TG_CHECK(snapshot != nullptr);
    TG_CHECK_EQ(snapshot->coordinator_epoch.value(), epoch.value());

    // The durable container must decode after concurrent writes.
    auto restarted = GovernorFixture::create_with_state(path);
    TG_CHECK(restarted != nullptr);
    TG_STATUS_OK(restarted->governor->load_durable());
    TG_CHECK(restarted->governor->snapshot()->domains.size() == 4U);
    tg::remove_file(path);
}

TG_CASE(concurrency, concurrent_action_completion_and_verification) {
    TG_PHASE("ACTIONS_SETUP");
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    TG_STATUS_OK(fixture->add_device(1));
    TG_STATUS_OK(fixture->add_accelerator_domain(1, 1, 1));
    auto* governor = fixture->governor.get();
    TG_OK(fixture->publish(1, 85.0));

    std::vector<ActionId> actions;
    for (int i = 0; i < 8; ++i) {
        MitigationIntentRecord record;
        record.intent = MitigationIntent::REQUEST_CLOCK_REDUCTION;
        auto action = governor->authorize_mitigation(
            ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "concurrency");
        if (!action.has_value()) {
            continue;
        }
        actions.push_back(action.value());
        (void)governor->record_action_dispatch(action.value());
    }
    TG_CHECK(actions.size() == 8U);

    constexpr int kThreads = 8;
    std::barrier gate(kThreads);
    std::atomic<int> dispatched_ok{0};
    std::atomic<int> superseded{0};
    std::atomic<int> other{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t i = 0; i < actions.size(); ++i) {
        threads.emplace_back([&, i]() {
            gate.arrive_and_wait();
            const auto action = governor->get_action(actions[i]);
            if (!action.has_value()) {
                other.fetch_add(1);
                return;
            }
            ActionAck ack;
            ack.action_id = actions[i];
            ack.action_generation = action.value().generation;
            ack.coordinator_epoch = governor->epoch();
            ack.worker_boot = action.value().authority.worker_boot;
            ack.device_generation = action.value().authority.device_generation;
            ack.domain_generation = action.value().authority.domain_generation;
            ack.accepted = true;
            auto lifecycle = governor->record_action_ack(ack);
            if (lifecycle.has_value()) {
                dispatched_ok.fetch_add(1);
            } else if (lifecycle.error().code == ThermalErrorCode::ACTION_SUPERSEDED) {
                superseded.fetch_add(1);
            } else {
                other.fetch_add(1);
            }
        });
    }
    TG_PHASE("ACTIONS_RUN");
    for (auto& thread : threads) {
        thread.join();
    }
    TG_PHASE("ACTIONS_VERIFY");
    TG_CHECK_EQ(dispatched_ok.load(), static_cast<int>(actions.size()));
    TG_CHECK_EQ(other.load(), 0);
    for (const auto id : actions) {
        TG_CHECK_EQ(governor->get_action(id).value().lifecycle, ActionLifecycle::ACKNOWLEDGED);
    }
}
