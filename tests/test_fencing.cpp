// Thermal Governor — worker incarnation fencing tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "framework.hpp"
#include "governor_fixture.hpp"

namespace {

using namespace thermal_governor;
using tg::GovernorFixture;
using tg::GovernorFixture;

void prepare(tg::Context& tg_ctx, GovernorFixture& fixture) {
    TG_STATUS_OK(fixture.add_device(1));
    TG_STATUS_OK(fixture.add_accelerator_domain(1, 1, 1));
}

[[nodiscard]] ThermalState state_of(GovernorFixture& fixture) {
    auto evaluation = fixture.governor->evaluate_domain(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}});
    if (!evaluation.has_value()) {
        return ThermalState::UNSUPPORTED;
    }
    return evaluation.value().state;
}

}  // namespace

TG_CASE(fencing, worker_death_invalidates_live_evidence) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_STATUS_OK(fixture->governor->register_worker(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}},
        Label{"worker-A"}));
    TG_OK(fixture->publish(1, 60.0, 1, 1, 1, WorkerId{StrongId<WorkerIdTag>{1}},
                           WorkerBootId{StrongId<WorkerBootIdTag>{1}}));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::NORMAL);

    TG_STATUS_OK(fixture->governor->note_worker_death(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}},
        "process terminated"));
    // The evidence was current a moment ago; it is not current now.
    TG_CHECK_EQ(state_of(*fixture), ThermalState::REVALIDATION_REQUIRED);
}

TG_CASE(fencing, delayed_old_boot_telemetry_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_STATUS_OK(fixture->governor->register_worker(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}},
        Label{"worker-A"}));
    TG_OK(fixture->publish(1, 60.0, 1, 1, 1, WorkerId{StrongId<WorkerIdTag>{1}},
                           WorkerBootId{StrongId<WorkerBootIdTag>{1}}));
    TG_STATUS_OK(fixture->governor->note_worker_death(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}}, "killed"));

    // Telemetry that was in flight when the worker died must never mutate
    // current state.
    auto delayed = fixture->publish(1, 40.0, 1, 1, 1, WorkerId{StrongId<WorkerIdTag>{1}},
                                    WorkerBootId{StrongId<WorkerBootIdTag>{1}});
    TG_CHECK(!delayed.has_value());
    TG_CHECK_EQ(delayed.error().code, ThermalErrorCode::STALE_WORKER);
}

TG_CASE(fencing, reincarnation_does_not_inherit_authority) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_STATUS_OK(fixture->governor->register_worker(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}},
        Label{"worker-A"}));
    TG_OK(fixture->publish(1, 60.0, 1, 1, 1, WorkerId{StrongId<WorkerIdTag>{1}},
                           WorkerBootId{StrongId<WorkerBootIdTag>{1}}));
    TG_STATUS_OK(fixture->governor->note_worker_death(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}}, "killed"));

    TG_STATUS_OK(fixture->governor->register_worker(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{2}},
        Label{"worker-A-prime"}));
    // A fresh incarnation starts from revalidation, not from the previous
    // incarnation's conclusion.
    TG_CHECK_EQ(state_of(*fixture), ThermalState::REVALIDATION_REQUIRED);

    // Evidence from the old boot is still refused.
    auto old_boot = fixture->publish(1, 60.0, 1, 1, 1, WorkerId{StrongId<WorkerIdTag>{1}},
                                     WorkerBootId{StrongId<WorkerBootIdTag>{1}});
    TG_CHECK(!old_boot.has_value());
    TG_CHECK_EQ(old_boot.error().code, ThermalErrorCode::STALE_WORKER);

    // Evidence from the new boot is accepted and re-establishes authority.
    TG_OK(fixture->publish(1, 55.0, 1, 1, 1, WorkerId{StrongId<WorkerIdTag>{1}},
                           WorkerBootId{StrongId<WorkerBootIdTag>{2}}));
    TG_CHECK_EQ(state_of(*fixture), ThermalState::NORMAL);
}

TG_CASE(fencing, boot_identity_cannot_move_backwards) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_STATUS_OK(fixture->governor->register_worker(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{5}},
        Label{"worker-A"}));
    TG_STATUS_ERROR_CODE(
        fixture->governor->register_worker(WorkerId{StrongId<WorkerIdTag>{1}},
                                           WorkerBootId{StrongId<WorkerBootIdTag>{4}},
                                           Label{"stale"}),
        ThermalErrorCode::STALE_WORKER);
}

TG_CASE(fencing, in_flight_action_is_classified_honestly) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_STATUS_OK(fixture->governor->register_worker(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}},
        Label{"worker-A"}));
    TG_OK(fixture->publish(1, 85.0, 1, 1, 1, WorkerId{StrongId<WorkerIdTag>{1}},
                           WorkerBootId{StrongId<WorkerBootIdTag>{1}}));

    MitigationIntentRecord record;
    record.intent = MitigationIntent::REQUEST_CLOCK_REDUCTION;
    auto action = fixture->governor->authorize_mitigation(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "derate");
    TG_OK(action);
    const auto authorised = fixture->governor->get_action(action.value()).value();
    TG_CHECK_EQ(authorised.authority.worker_boot.value(), 1U);
    TG_OK(fixture->governor->record_action_dispatch(action.value()));

    TG_STATUS_OK(fixture->governor->note_worker_death(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}}, "killed"));
    // The outcome was never proven, so it is not reported as success.
    TG_CHECK_EQ(fixture->governor->get_action(action.value()).value().lifecycle,
                ActionLifecycle::OUTCOME_UNKNOWN);
    TG_ERROR_CODE(fixture->governor->record_action_dispatch(action.value()),
                  ThermalErrorCode::ACTION_SUPERSEDED);
}

TG_CASE(fencing, old_boot_action_completion_rejected) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_STATUS_OK(fixture->governor->register_worker(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}},
        Label{"worker-A"}));
    TG_OK(fixture->publish(1, 85.0, 1, 1, 1, WorkerId{StrongId<WorkerIdTag>{1}},
                           WorkerBootId{StrongId<WorkerBootIdTag>{1}}));
    MitigationIntentRecord record;
    record.intent = MitigationIntent::REQUEST_CLOCK_REDUCTION;
    auto action = fixture->governor->authorize_mitigation(
        ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "derate");
    TG_OK(action);
    TG_OK(fixture->governor->record_action_dispatch(action.value()));
    const auto dispatched = fixture->governor->get_action(action.value()).value();

    ActionAck ack;
    ack.action_id = action.value();
    ack.action_generation = dispatched.generation;
    ack.coordinator_epoch = fixture->governor->epoch();
    ack.worker_boot = WorkerBootId{StrongId<WorkerBootIdTag>{9}};
    ack.device_generation = dispatched.authority.device_generation;
    ack.domain_generation = dispatched.authority.domain_generation;
    ack.accepted = true;
    TG_ERROR_CODE(fixture->governor->record_action_ack(ack), ThermalErrorCode::STALE_WORKER);
}

TG_CASE(fencing, historical_records_survive_worker_death) {
    auto fixture = GovernorFixture::create();
    TG_CHECK(fixture != nullptr);
    prepare(tg_ctx, *fixture);
    TG_STATUS_OK(fixture->governor->register_worker(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}},
        Label{"worker-A"}));
    TG_OK(fixture->publish(1, 90.0, 1, 1, 1, WorkerId{StrongId<WorkerIdTag>{1}},
                           WorkerBootId{StrongId<WorkerBootIdTag>{1}}));
    const std::size_t before = fixture->governor->history_size();
    TG_CHECK(before > 0);
    TG_STATUS_OK(fixture->governor->note_worker_death(
        WorkerId{StrongId<WorkerIdTag>{1}}, WorkerBootId{StrongId<WorkerBootIdTag>{1}}, "killed"));
    TG_CHECK(fixture->governor->history_size() >= before);
}
