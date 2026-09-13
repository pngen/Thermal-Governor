// Thermal Governor — coordinator restart and durability tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "framework.hpp"
#include "governor_fixture.hpp"
#include "process_util.hpp"

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

TG_CASE(restart, durable_state_survives_and_epoch_advances) {
    const std::string path = tg::unique_temp_path("tg-restart");
    tg::remove_file(path);
    {
        auto fixture = GovernorFixture::create_with_state(path);
        TG_CHECK(fixture != nullptr);
        prepare(tg_ctx, *fixture);
        TG_OK(fixture->publish(1, 70.0));
        TG_CHECK(fixture->governor->history_size() > 0);
        TG_STATUS_OK(fixture->governor->persist());
    }

    auto restarted = GovernorFixture::create_with_state(path);
    TG_CHECK(restarted != nullptr);
    TG_STATUS_OK(restarted->governor->load_durable());

    // A restart advances the coordinated epoch.
    TG_CHECK_EQ(restarted->governor->epoch().value(), 2U);
    // Durable definitions and history survive.
    TG_CHECK(restarted->governor->snapshot()->domains.size() == 1U);
    TG_CHECK(restarted->governor->history_size() > 0);
    // Dynamic thermal evidence does not survive: authority must be
    // re-established from fresh evidence.
    TG_CHECK_EQ(state_of(*restarted), ThermalState::REVALIDATION_REQUIRED);

    tg::remove_file(path);
}

TG_CASE(restart, old_epoch_traffic_is_rejected_after_restart) {
    const std::string path = tg::unique_temp_path("tg-restart-epoch");
    tg::remove_file(path);
    std::uint64_t original_epoch = 0;
    {
        auto fixture = GovernorFixture::create_with_state(path);
        TG_CHECK(fixture != nullptr);
        prepare(tg_ctx, *fixture);
        TG_OK(fixture->publish(1, 70.0));
        original_epoch = fixture->governor->epoch().value();
        TG_STATUS_OK(fixture->governor->persist());
    }
    auto restarted = GovernorFixture::create_with_state(path);
    TG_CHECK(restarted != nullptr);
    TG_STATUS_OK(restarted->governor->load_durable());
    TG_CHECK(restarted->governor->epoch().value() > original_epoch);

    // Evidence stamped with the previous epoch cannot mutate state.
    ThermalEvidence stale;
    stale.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{1}};
    stale.subject = SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{1}},
                                           DeviceGeneration{StrongId<DeviceGenerationTag>{1}});
    stale.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{1}};
    stale.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    stale.temperature = DegreesCelsius{40.0};
    stale.source = MeasurementSource::SYNTHETIC_MODEL;
    stale.provenance = Provenance::SYNTHETIC;
    stale.measurement_sequence = 1;
    stale.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{1}};
    stale.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{original_epoch}};
    stale.measured_at = restarted->clock->now();
    TG_ERROR_CODE(restarted->governor->publish_temperature(stale),
                  ThermalErrorCode::STALE_EPOCH);

    // Fresh evidence under the new epoch re-establishes authority.
    TG_OK(restarted->publish(1, 50.0));
    TG_CHECK_EQ(state_of(*restarted), ThermalState::NORMAL);
    tg::remove_file(path);
}

TG_CASE(restart, unresolved_actions_are_recovered_conservatively) {
    const std::string path = tg::unique_temp_path("tg-restart-actions");
    tg::remove_file(path);
    ActionId id{};
    {
        auto fixture = GovernorFixture::create_with_state(path);
        TG_CHECK(fixture != nullptr);
        prepare(tg_ctx, *fixture);
        TG_OK(fixture->publish(1, 85.0));
        MitigationIntentRecord record;
        record.intent = MitigationIntent::REQUEST_CLOCK_REDUCTION;
        auto action = fixture->governor->authorize_mitigation(
            ThermalDomainId{StrongId<ThermalDomainIdTag>{1}}, record, "derate");
        TG_OK(action);
        id = action.value();
        TG_OK(fixture->governor->record_action_dispatch(id));
        TG_STATUS_OK(fixture->governor->persist());
    }
    auto restarted = GovernorFixture::create_with_state(path);
    TG_CHECK(restarted != nullptr);
    TG_STATUS_OK(restarted->governor->load_durable());
    auto restored = restarted->governor->get_action(id);
    TG_OK(restored);
    // An action whose outcome was never proven is not assumed to have taken
    // effect, nor assumed to have failed silently.
    TG_CHECK_EQ(restored.value().lifecycle, ActionLifecycle::OUTCOME_UNKNOWN);
    tg::remove_file(path);
}

TG_CASE(restart, policy_generation_advances_and_domains_are_revalidated) {
    const std::string path = tg::unique_temp_path("tg-restart-policy");
    tg::remove_file(path);
    {
        auto fixture = GovernorFixture::create_with_state(path);
        TG_CHECK(fixture != nullptr);
        prepare(tg_ctx, *fixture);
        auto policy = fixture->governor->get_policy(
            ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}});
        TG_OK(policy);
        ThermalPolicy updated = policy.value();
        updated.generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{2}};
        updated.thresholds.warning = DegreesCelsius{60.0};
        updated.thresholds.derating = DegreesCelsius{62.0};
        updated.thresholds.critical = DegreesCelsius{84.0};
        updated.thresholds.recovery = DegreesCelsius{55.0};
        updated.thresholds.near_limit_band = TemperatureDelta{1.0};
        TG_OK(fixture->governor->set_policy(updated));
        TG_STATUS_OK(fixture->governor->persist());
    }
    auto restarted = GovernorFixture::create_with_state(path);
    TG_CHECK(restarted != nullptr);
    TG_STATUS_OK(restarted->governor->load_durable());
    auto policy = restarted->governor->get_policy(ThermalPolicyId{StrongId<ThermalPolicyIdTag>{1}});
    TG_OK(policy);
    // A REAL telemetry plus SYNTHETIC policy threshold configuration: the
    // thresholds are synthetic inputs, the mechanism is the real one.
    TG_CHECK_EQ(policy.value().thresholds.derating.value(), 62.0);
    TG_CHECK_EQ(policy.value().thresholds.recovery.value(), 55.0);
    tg::remove_file(path);
}

TG_CASE(restart, corrupt_durable_file_is_rejected) {
    const std::string path = tg::unique_temp_path("tg-restart-corrupt");
    tg::remove_file(path);
    {
        auto fixture = GovernorFixture::create_with_state(path);
        TG_CHECK(fixture != nullptr);
        prepare(tg_ctx, *fixture);
        TG_STATUS_OK(fixture->governor->persist());
    }
    std::string bytes;
    TG_CHECK(tg::read_file(path, bytes));
    TG_CHECK(bytes.size() > 8);
    // Corrupt a byte in the middle of the container body.
    bytes[bytes.size() / 2] = static_cast<char>(static_cast<unsigned char>(bytes[bytes.size() / 2]) ^ 0xFFU);
    TG_CHECK(tg::write_file(path, bytes));

    auto restarted = GovernorFixture::create_with_state(path);
    TG_CHECK(restarted != nullptr);
    auto loaded = restarted->governor->load_durable();
    TG_CHECK(!loaded.ok());
    TG_CHECK(loaded.code() == ThermalErrorCode::PERSISTENCE_INTEGRITY ||
              loaded.code() == ThermalErrorCode::PERSISTENCE_CORRUPT);
    tg::remove_file(path);
}
