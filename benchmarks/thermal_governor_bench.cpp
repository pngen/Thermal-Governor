// Thermal Governor - micro-benchmarks.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every measurement covers completed operations and is taken with
// std::chrono::steady_clock. The governor used by the single-threaded
// benchmarks is driven by a monotonic ManualClock, so the measured cost is
// never confounded by wall-clock reads inside freshness evaluation. Each row
// prints the fixed iteration count, the mean nanoseconds per completed
// operation and the implied operations per second. Two decimal places are
// printed deliberately: no precision beyond that is claimed.

#include "thermal_governor/thermal_governor.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace thermal_governor;

using SteadyClock = std::chrono::steady_clock;

// --- Fixed iteration counts ------------------------------------------------
constexpr std::uint64_t kIngestIterations = 200000;
constexpr std::uint64_t kEvaluateIterations = 200000;
constexpr std::uint64_t kHeadroomIterations = 1000000;
constexpr std::uint64_t kDomainScaleIterations = 20000;
constexpr std::uint64_t kRecoveryIterations = 200000;
constexpr std::uint64_t kExplainIterations = 20000;
constexpr std::uint64_t kRenderIterations = 20000;
constexpr std::uint64_t kSnapshotIterations = 1000000;
constexpr std::uint64_t kPersistIterations = 2000;
constexpr std::uint64_t kLoadIterations = 500;
constexpr std::uint64_t kRoundTripIterations = 500;
constexpr std::uint32_t kReaderThreads = 4;
constexpr std::uint64_t kEvaluationsPerReader = 25000;

// A sink that keeps completed work observable to the optimiser. It is printed
// at the end of the run so that nothing measured can be discarded.
std::uint64_t g_sink = 0;
std::uint64_t g_failures = 0;

// --- Formatting ------------------------------------------------------------
std::string pad_right(const std::string& text, std::size_t width) {
    if (text.size() >= width) {
        return text;
    }
    return text + std::string(width - text.size(), ' ');
}

std::string pad_left(const std::string& text, std::size_t width) {
    if (text.size() >= width) {
        return text;
    }
    return std::string(width - text.size(), ' ') + text;
}

std::string fixed2(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

void print_header() {
    std::cout << pad_right("benchmark", 54) << pad_left("iterations", 12)
              << pad_left("ns/op", 14) << pad_left("ops/sec", 18) << '\n';
    std::cout << std::string(98, '-') << '\n';
}

void print_row(const std::string& name, std::uint64_t iterations, double total_ns) {
    const double per_operation =
        iterations == 0 ? 0.0 : total_ns / static_cast<double>(iterations);
    const double operations_per_second = per_operation > 0.0 ? 1.0e9 / per_operation : 0.0;
    std::cout << pad_right(name, 54) << pad_left(std::to_string(iterations), 12)
              << pad_left(fixed2(per_operation), 14) << pad_left(fixed2(operations_per_second), 18)
              << '\n';
}

void print_note(const std::string& text) { std::cout << "  " << text << '\n'; }

/// Time a fixed number of completed operations.
template <class Fn>
double time_ns(std::uint64_t iterations, Fn&& body) {
    const auto start = SteadyClock::now();
    for (std::uint64_t index = 0; index < iterations; ++index) {
        body();
    }
    const auto finish = SteadyClock::now();
    return std::chrono::duration<double, std::nano>(finish - start).count();
}

// --- Scenario helpers ------------------------------------------------------
struct Scenario {
    std::shared_ptr<ManualClock> clock;
    std::unique_ptr<ThermalGovernor> governor;
    DeviceId device{StrongId<DeviceIdTag>{1}};
    DeviceGeneration device_generation{StrongId<DeviceGenerationTag>{1}};
    ThermalDomainId domain{StrongId<ThermalDomainIdTag>{1}};
    ThermalDomainGeneration domain_generation{StrongId<ThermalDomainGenerationTag>{1}};
    std::uint64_t sequence = 0;
};

std::unique_ptr<ThermalGovernor> create_governor(std::shared_ptr<const IClock> clock,
                                                 const std::string& durable_path = std::string{}) {
    GovernorConfig config;
    config.clock = std::move(clock);
    config.durable_path = durable_path;
    auto created = ThermalGovernor::create(config);
    if (!created) {
        std::cerr << "fatal: governor creation failed: " << created.error().render() << '\n';
        return nullptr;
    }
    return std::move(created.value());
}

ThermalEvidence make_sample(const Scenario& scenario, double temperature, std::uint64_t sequence) {
    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{sequence}};
    evidence.subject = SubjectRef::for_device(scenario.device, scenario.device_generation);
    evidence.domain = scenario.domain;
    evidence.domain_generation = scenario.domain_generation;
    evidence.temperature = DegreesCelsius{temperature};
    evidence.source = MeasurementSource::SYNTHETIC_MODEL;
    evidence.provenance = Provenance::SYNTHETIC;
    evidence.measurement_sequence = sequence;
    evidence.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{sequence}};
    evidence.coordinator_epoch = scenario.governor->epoch();
    evidence.measured_at = scenario.clock->now();
    evidence.measured_wall_at = scenario.clock->wall_now();
    evidence.integrity = IntegrityStatus::OK;
    evidence.confidence = 0.95;
    return evidence;
}

bool register_primary(ThermalGovernor& governor, Scenario& scenario) {
    DeviceRegistration registration;
    registration.device = scenario.device;
    registration.generation = scenario.device_generation;
    registration.label = Label{"benchmark-device"};
    registration.provenance = Provenance::SYNTHETIC;
    registration.capabilities.set(ThermalCapability::TEMPERATURE,
                                  CapabilityState::SUPPORTED_SYNTHETIC);
    registration.capabilities.set(ThermalCapability::THROTTLE_REASONS,
                                  CapabilityState::SUPPORTED_SYNTHETIC);
    auto device_status = governor.register_device(registration);
    if (!device_status.ok()) {
        std::cerr << "fatal: register_device: " << device_status.error().render() << '\n';
        return false;
    }

    ThermalDomainDefinition definition;
    definition.id = scenario.domain;
    definition.generation = scenario.domain_generation;
    definition.type = ThermalDomainType::ACCELERATOR;
    definition.label = Label{"benchmark-domain"};
    definition.provenance = Provenance::SYNTHETIC;
    definition.evidence_source = "synthetic benchmark model";
    definition.members.push_back(
        DomainMember{SubjectRef::for_device(scenario.device, scenario.device_generation), 1.0});
    auto domain_status = governor.register_thermal_domain(definition);
    if (!domain_status.ok()) {
        std::cerr << "fatal: register_thermal_domain: " << domain_status.error().render() << '\n';
        return false;
    }
    return true;
}

bool prime_evidence(Scenario& scenario, double temperature) {
    ++scenario.sequence;
    auto receipt =
        scenario.governor->publish_temperature(make_sample(scenario, temperature, scenario.sequence));
    if (!receipt) {
        std::cerr << "fatal: publish_temperature: " << receipt.error().render() << '\n';
        return false;
    }
    return true;
}

// --- Benchmarks ------------------------------------------------------------

void bench_ingestion(Scenario& scenario) {
    const CoordinatorEpoch epoch = scenario.governor->epoch();
    const SteadyTimePoint now = scenario.clock->now();
    const WallTimePoint wall_now = scenario.clock->wall_now();
    std::uint64_t rejected = 0;
    const double total = time_ns(kIngestIterations, [&]() {
        ++scenario.sequence;
        ThermalEvidence evidence;
        evidence.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{scenario.sequence}};
        evidence.subject = SubjectRef::for_device(scenario.device, scenario.device_generation);
        evidence.domain = scenario.domain;
        evidence.domain_generation = scenario.domain_generation;
        evidence.temperature = DegreesCelsius{60.0};
        evidence.source = MeasurementSource::SYNTHETIC_MODEL;
        evidence.provenance = Provenance::SYNTHETIC;
        evidence.measurement_sequence = scenario.sequence;
        evidence.telemetry_generation =
            TelemetryGeneration{StrongId<TelemetryGenerationTag>{scenario.sequence}};
        evidence.coordinator_epoch = epoch;
        evidence.measured_at = now;
        evidence.measured_wall_at = wall_now;
        evidence.integrity = IntegrityStatus::OK;
        auto receipt = scenario.governor->publish_temperature(evidence);
        if (!receipt) {
            ++rejected;
            return;
        }
        g_sink += receipt.value().telemetry_generation.value();
    });
    print_row("temperature evidence ingestion (publish_temperature)", kIngestIterations, total);
    if (rejected != 0) {
        ++g_failures;
        print_note("rejected publications: " + std::to_string(rejected));
    }
}

void bench_evaluate(Scenario& scenario) {
    std::uint64_t errors = 0;
    const double total = time_ns(kEvaluateIterations, [&]() {
        auto evaluation = scenario.governor->evaluate_domain(scenario.domain);
        if (!evaluation) {
            ++errors;
            return;
        }
        g_sink += static_cast<std::uint64_t>(evaluation.value().state);
    });
    print_row("policy evaluation (evaluate_domain)", kEvaluateIterations, total);
    if (errors != 0) {
        ++g_failures;
        print_note("failed evaluations: " + std::to_string(errors));
    }
}

void bench_headroom() {
    HeadroomInput input;
    input.current_temperature = DegreesCelsius{74.0};
    input.policy_critical = DegreesCelsius{88.0};
    input.vendor_limit = DegreesCelsius{84.0};
    input.vendor_limit_provenance = Provenance::REAL;
    input.policy_safety_margin = TemperatureDelta{2.0};
    input.uncertainty_margin = TemperatureDelta{0.5};
    input.recovery_threshold = DegreesCelsius{72.0};
    input.recovery_margin = TemperatureDelta{1.0};
    const double total = time_ns(kHeadroomIterations, [&]() {
        const HeadroomBreakdown breakdown = compute_headroom(input);
        g_sink += static_cast<std::uint64_t>(
            static_cast<std::int64_t>(breakdown.effective_headroom.value() * 100.0));
    });
    print_row("headroom computation (compute_headroom)", kHeadroomIterations, total);
}

void bench_recovery_assessment() {
    const ThermalPolicy policy = ThermalPolicy::make_default();
    RecoveryHistory history(64);
    const SteadyTimePoint base{std::chrono::seconds{5000}};
    for (std::uint32_t index = 0; index < 64; ++index) {
        RecoverySample sample;
        sample.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{index + 1}};
        sample.temperature = DegreesCelsius{64.0};
        sample.at = base + Milliseconds{static_cast<std::int64_t>(index) * 500};
        sample.worker = WorkerId{StrongId<WorkerIdTag>{1}};
        sample.worker_boot = WorkerBootId{StrongId<WorkerBootIdTag>{1}};
        sample.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{1}};
        sample.device_generation = DeviceGeneration{StrongId<DeviceGenerationTag>{1}};
        sample.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
        sample.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{index + 1}};
        sample.throttle_class = ThrottleClass::NO_THROTTLE_OBSERVED;
        sample.provenance = Provenance::SYNTHETIC;
        sample.admissible = true;
        history.push(sample);
    }

    HeadroomInput headroom_input;
    headroom_input.current_temperature = DegreesCelsius{64.0};
    headroom_input.policy_critical = policy.thresholds.critical;
    headroom_input.policy_safety_margin = policy.margins.policy_safety_margin;
    headroom_input.uncertainty_margin = policy.margins.uncertainty_margin;
    headroom_input.recovery_threshold = policy.thresholds.recovery;
    headroom_input.recovery_margin = policy.margins.recovery_margin;

    RecoveryInput input;
    input.policy = &policy;
    input.history = &history;
    input.recovery_threshold = policy.thresholds.recovery;
    input.recovery_margin = policy.margins.recovery_margin;
    input.headroom = compute_headroom(headroom_input);
    input.evidence_provenance = Provenance::SYNTHETIC;
    input.current_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{1}};
    input.current_boot = WorkerBootId{StrongId<WorkerBootIdTag>{1}};
    input.current_device_generation = DeviceGeneration{StrongId<DeviceGenerationTag>{1}};
    input.current_domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    input.explicit_authorization = false;
    input.coupled_domains_recovered = true;
    input.coupled_domains_checked = true;
    input.distinct_witnesses = 1;

    std::uint64_t not_allowed = 0;
    const double total = time_ns(kRecoveryIterations, [&]() {
        const RecoveryAssessment assessment = evaluate_recovery(input);
        if (!assessment.allowed) {
            ++not_allowed;
            return;
        }
        g_sink += assessment.qualifying_samples;
    });
    print_row("recovery evaluation (evaluate_recovery, 64 samples)", kRecoveryIterations, total);
    if (not_allowed != 0) {
        ++g_failures;
        print_note("assessments that did not permit recovery: " + std::to_string(not_allowed));
    }
    print_note("recovery benchmark uses a fixed 64-sample admissible history on a manual clock");
}

void bench_explain(Scenario& scenario) {
    std::uint64_t errors = 0;
    const double total = time_ns(kExplainIterations, [&]() {
        auto explanation = scenario.governor->explain(scenario.domain);
        if (!explanation) {
            ++errors;
            return;
        }
        g_sink += explanation.value().sections().size();
    });
    print_row("deterministic explanation (explain)", kExplainIterations, total);
    if (errors != 0) {
        ++g_failures;
        print_note("failed explanations: " + std::to_string(errors));
    }

    auto explanation = scenario.governor->explain(scenario.domain);
    if (!explanation) {
        ++g_failures;
        print_note("no explanation available for the render benchmark");
        return;
    }
    std::uint64_t bytes = 0;
    const double render_total = time_ns(kRenderIterations, [&]() {
        const std::string rendered = explanation.value().render();
        bytes = rendered.size();
        g_sink += bytes;
    });
    print_row("explanation render (Explanation::render)", kRenderIterations, render_total);
    print_note("rendered explanation bytes: " + std::to_string(bytes));
}

void bench_snapshot(Scenario& scenario) {
    std::uint64_t null_snapshots = 0;
    const double total = time_ns(kSnapshotIterations, [&]() {
        const std::shared_ptr<const GovernanceSnapshot> snapshot = scenario.governor->snapshot();
        if (snapshot == nullptr) {
            ++null_snapshots;
            return;
        }
        g_sink += static_cast<std::uint64_t>(snapshot->domains.size());
    });
    print_row("snapshot acquisition (snapshot)", kSnapshotIterations, total);
    if (null_snapshots != 0) {
        ++g_failures;
        print_note("null snapshots: " + std::to_string(null_snapshots));
    }
    print_note("snapshot() returns the published copy-on-write view; creation happens on mutation");
}

std::filesystem::path bench_directory() {
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    if (error) {
        return std::filesystem::current_path();
    }
    return directory;
}

void remove_file(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

void bench_persistence() {
    const std::filesystem::path file = bench_directory() / "thermal_governor_bench_durable.bin";
    remove_file(file);
    remove_file(file.string() + ".tmp");

    // The persistence scenario is the only governor in this suite configured
    // with a durable path; persist() rejects a runtime without one.
    Scenario source;
    source.clock = std::make_shared<ManualClock>(SteadyTimePoint{std::chrono::seconds{20000}});
    source.governor = create_governor(source.clock, file.string());
    if (source.governor == nullptr) {
        ++g_failures;
        return;
    }
    if (!register_primary(*source.governor, source)) {
        ++g_failures;
        return;
    }
    if (!prime_evidence(source, 62.0)) {
        ++g_failures;
        return;
    }

    std::uint64_t persist_errors = 0;
    const double persist_total = time_ns(kPersistIterations, [&]() {
        auto status = source.governor->persist();
        if (!status.ok()) {
            ++persist_errors;
        }
    });
    if (persist_errors != 0) {
        ++g_failures;
        print_note("persist failures: " + std::to_string(persist_errors));
    }
    print_row("persistence encode + atomic replace (persist)", kPersistIterations, persist_total);

    std::uint64_t load_errors = 0;
    std::uint64_t restored_domains = 0;
    double load_total = 0.0;
    for (std::uint64_t index = 0; index < kLoadIterations; ++index) {
        std::unique_ptr<ThermalGovernor> fresh = create_governor(source.clock, file.string());
        if (fresh == nullptr) {
            ++load_errors;
            continue;
        }
        const auto start = SteadyClock::now();
        auto status = fresh->load_durable();
        const auto finish = SteadyClock::now();
        load_total += std::chrono::duration<double, std::nano>(finish - start).count();
        if (!status.ok()) {
            ++load_errors;
            continue;
        }
        const std::shared_ptr<const GovernanceSnapshot> snapshot = fresh->snapshot();
        restored_domains = snapshot != nullptr ? snapshot->domains.size() : 0;
        if (restored_domains == 0) {
            ++load_errors;
            continue;
        }
        g_sink += fresh->topology_generation().value();
    }
    if (load_errors != 0) {
        ++g_failures;
        print_note("load_durable failures: " + std::to_string(load_errors));
    }
    print_row("durable restore (load_durable into a fresh governor)", kLoadIterations, load_total);
    print_note("domains restored by a fresh governor per load: " + std::to_string(restored_domains));

    std::uint64_t round_trip_errors = 0;
    const double round_trip_total = time_ns(kRoundTripIterations, [&]() {
        auto persisted = source.governor->persist();
        if (!persisted.ok()) {
            ++round_trip_errors;
            return;
        }
        std::unique_ptr<ThermalGovernor> fresh = create_governor(source.clock, file.string());
        if (fresh == nullptr) {
            ++round_trip_errors;
            return;
        }
        auto loaded = fresh->load_durable();
        if (!loaded.ok()) {
            ++round_trip_errors;
            return;
        }
        const std::shared_ptr<const GovernanceSnapshot> snapshot = fresh->snapshot();
        if (snapshot == nullptr || snapshot->domains.empty()) {
            ++round_trip_errors;
            return;
        }
        g_sink += fresh->topology_generation().value();
    });
    if (round_trip_errors != 0) {
        ++g_failures;
        print_note("round trip failures: " + std::to_string(round_trip_errors));
    }
    print_row("persistence round trip (persist + fresh governor + load_durable)",
              kRoundTripIterations, round_trip_total);
    print_note("durable file: " + file.string());

    remove_file(file);
    remove_file(file.string() + ".tmp");
}

bool bench_domain_scale(std::size_t domain_count) {
    const std::filesystem::path file =
        bench_directory() / ("thermal_governor_bench_scale_" + std::to_string(domain_count) + ".bin");
    remove_file(file);
    remove_file(file.string() + ".tmp");

    // The domain population is installed through the durable restore path so
    // that the (unmeasured) registration cost of a large population does not
    // dominate the benchmark run. The measured operation is unchanged: one
    // evaluate_domain call in a governor holding domain_count domains.
    DurableState state;
    state.format_version = kPersistenceFormatVersion;
    state.coordinator = CoordinatorId{StrongId<CoordinatorIdTag>{1}};
    state.coordinator_epoch = CoordinatorEpoch{StrongId<CoordinatorEpochTag>{1}};
    state.policy_generation = ThermalPolicyGeneration{StrongId<ThermalPolicyGenerationTag>{1}};
    state.topology_generation = TopologyGeneration{StrongId<TopologyGenerationTag>{1}};
    state.policies.push_back(ThermalPolicy::make_default());
    state.domains.reserve(domain_count);
    for (std::size_t index = 0; index < domain_count; ++index) {
        ThermalDomainDefinition definition;
        definition.id =
            ThermalDomainId{StrongId<ThermalDomainIdTag>{static_cast<std::uint64_t>(index) + 1}};
        definition.generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
        definition.type = ThermalDomainType::ACCELERATOR;
        definition.label = Label{"scale-domain"};
        definition.provenance = Provenance::SYNTHETIC;
        if (index == 0) {
            definition.members.push_back(DomainMember{
                SubjectRef::for_device(DeviceId{StrongId<DeviceIdTag>{1}},
                                       DeviceGeneration{StrongId<DeviceGenerationTag>{1}}),
                1.0});
        }
        state.domains.push_back(std::move(definition));
    }

    DurableStore store(file.string());
    auto saved = store.save(state);
    if (!saved.ok()) {
        std::cerr << "fatal: durable save failed: " << saved.error().render() << '\n';
        remove_file(file);
        remove_file(file.string() + ".tmp");
        return false;
    }

    auto clock = std::make_shared<ManualClock>(SteadyTimePoint{std::chrono::seconds{30000}});
    std::unique_ptr<ThermalGovernor> governor = create_governor(clock, file.string());
    if (governor == nullptr) {
        remove_file(file);
        return false;
    }
    auto loaded = governor->load_durable();
    if (!loaded.ok()) {
        std::cerr << "fatal: durable load failed: " << loaded.error().render() << '\n';
        remove_file(file);
        remove_file(file.string() + ".tmp");
        return false;
    }

    ThermalDomainId probe_domain{StrongId<ThermalDomainIdTag>{1}};
    DeviceId probe_device{StrongId<DeviceIdTag>{1}};
    DeviceGeneration probe_generation{StrongId<DeviceGenerationTag>{1}};

    DeviceRegistration registration;
    registration.device = probe_device;
    registration.generation = probe_generation;
    registration.label = Label{"scale-probe-device"};
    registration.provenance = Provenance::SYNTHETIC;
    registration.capabilities.set(ThermalCapability::TEMPERATURE,
                                  CapabilityState::SUPPORTED_SYNTHETIC);
    auto device_status = governor->register_device(registration);
    if (!device_status.ok()) {
        std::cerr << "fatal: register_device: " << device_status.error().render() << '\n';
        remove_file(file);
        remove_file(file.string() + ".tmp");
        return false;
    }

    ThermalEvidence evidence;
    evidence.evidence_id = EvidenceId{StrongId<EvidenceIdTag>{1}};
    evidence.subject = SubjectRef::for_device(probe_device, probe_generation);
    evidence.domain = probe_domain;
    evidence.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    evidence.temperature = DegreesCelsius{70.0};
    evidence.source = MeasurementSource::SYNTHETIC_MODEL;
    evidence.provenance = Provenance::SYNTHETIC;
    evidence.measurement_sequence = 1;
    evidence.telemetry_generation = TelemetryGeneration{StrongId<TelemetryGenerationTag>{1}};
    evidence.coordinator_epoch = governor->epoch();
    evidence.measured_at = clock->now();
    evidence.measured_wall_at = clock->wall_now();
    evidence.integrity = IntegrityStatus::OK;
    auto published = governor->publish_temperature(evidence);
    if (!published) {
        std::cerr << "fatal: publish_temperature: " << published.error().render() << '\n';
        remove_file(file);
        remove_file(file.string() + ".tmp");
        return false;
    }

    std::uint64_t errors = 0;
    const double total = time_ns(kDomainScaleIterations, [&]() {
        auto evaluation = governor->evaluate_domain(probe_domain);
        if (!evaluation) {
            ++errors;
            return;
        }
        g_sink += static_cast<std::uint64_t>(evaluation.value().state);
    });
    print_row("evaluate_domain with " + std::to_string(domain_count) + " registered domains",
              kDomainScaleIterations, total);
    if (errors != 0) {
        ++g_failures;
        print_note("failed evaluations: " + std::to_string(errors));
    }
    print_note("per-domain cost above is the cost of one completed evaluation of the probe domain");

    auto shutdown = governor->shutdown();
    if (!shutdown.ok()) {
        ++g_failures;
    }
    remove_file(file);
    remove_file(file.string() + ".tmp");
    return errors == 0;
}

void bench_concurrency(Scenario& scenario) {
    Scenario writer_scenario;
    writer_scenario.clock = scenario.clock;
    writer_scenario.governor = create_governor(scenario.clock);
    if (writer_scenario.governor == nullptr) {
        ++g_failures;
        return;
    }
    writer_scenario.device = DeviceId{StrongId<DeviceIdTag>{2}};
    writer_scenario.device_generation = DeviceGeneration{StrongId<DeviceGenerationTag>{1}};
    writer_scenario.domain = ThermalDomainId{StrongId<ThermalDomainIdTag>{2}};
    writer_scenario.domain_generation = ThermalDomainGeneration{StrongId<ThermalDomainGenerationTag>{1}};
    if (!register_primary(*writer_scenario.governor, writer_scenario)) {
        ++g_failures;
        return;
    }
    if (!prime_evidence(writer_scenario, 62.0)) {
        ++g_failures;
        return;
    }

    ThermalGovernor& readers_governor = *scenario.governor;
    const ThermalDomainId read_domain = scenario.domain;

    std::atomic<std::uint64_t> completed{0};
    std::atomic<std::uint64_t> completed_states{0};
    std::atomic<std::uint64_t> reader_errors{0};
    std::atomic<bool> stop_writer{false};
    std::atomic<std::uint64_t> published{0};

    const auto reader_body = [&]() {
        std::uint64_t local = 0;
        std::uint64_t local_errors = 0;
        for (std::uint64_t index = 0; index < kEvaluationsPerReader; ++index) {
            auto evaluation = readers_governor.evaluate_domain(read_domain);
            if (!evaluation) {
                ++local_errors;
                continue;
            }
            local += static_cast<std::uint64_t>(evaluation.value().state);
        }
        completed.fetch_add(kEvaluationsPerReader - local_errors, std::memory_order_relaxed);
        reader_errors.fetch_add(local_errors, std::memory_order_relaxed);
        completed_states.fetch_add(local, std::memory_order_relaxed);
    };

    const auto writer_body = [&]() {
        std::uint64_t local = 0;
        while (!stop_writer.load(std::memory_order_relaxed)) {
            ++writer_scenario.sequence;
            auto receipt = writer_scenario.governor->publish_temperature(
                make_sample(writer_scenario, 62.0, writer_scenario.sequence));
            if (receipt) {
                ++local;
            }
        }
        published.store(local, std::memory_order_relaxed);
    };

    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);
    const auto start = SteadyClock::now();
    std::thread writer(writer_body);
    for (std::uint32_t index = 0; index < kReaderThreads; ++index) {
        readers.emplace_back(reader_body);
    }
    for (auto& reader : readers) {
        reader.join();
    }
    stop_writer.store(true);
    writer.join();
    const auto finish = SteadyClock::now();

    const double total_ns = std::chrono::duration<double, std::nano>(finish - start).count();
    const std::uint64_t completed_evaluations = completed.load();
    g_sink += completed_states.load();
    print_row("concurrent evaluate_domain (" + std::to_string(kReaderThreads) +
                  " readers + 1 writer)",
              completed_evaluations, total_ns);
    print_note("completed evaluations: " + std::to_string(completed_evaluations) +
               ", publications accepted: " + std::to_string(published.load()) +
               ", reader errors: " + std::to_string(reader_errors.load()));
    if (reader_errors.load() != 0) {
        ++g_failures;
    }
    if (!writer_scenario.governor->shutdown().ok()) {
        ++g_failures;
    }
}

}  // namespace

int main() {
    std::cout << "== Thermal Governor benchmark suite ==\n";
    std::cout << "clock: std::chrono::steady_clock; governor clock: monotonic ManualClock\n";
    std::cout << "measurement unit: completed operations, two decimal places, fixed iteration"
              << " counts\n\n";
    print_header();

    Scenario scenario;
    scenario.clock = std::make_shared<ManualClock>(SteadyTimePoint{std::chrono::seconds{1000}});
    scenario.governor = create_governor(scenario.clock);
    if (scenario.governor == nullptr) {
        return 1;
    }
    if (!register_primary(*scenario.governor, scenario)) {
        return 1;
    }
    if (!prime_evidence(scenario, 60.0)) {
        return 1;
    }

    bench_ingestion(scenario);
    bench_evaluate(scenario);
    bench_headroom();
    bench_recovery_assessment();
    bench_explain(scenario);
    bench_snapshot(scenario);

    for (const std::size_t domain_count : {std::size_t{100}, std::size_t{1000},
                                           std::size_t{10000}}) {
        if (!bench_domain_scale(domain_count)) {
            ++g_failures;
        }
    }

    bench_persistence();
    bench_concurrency(scenario);

    if (!scenario.governor->shutdown().ok()) {
        ++g_failures;
    }

    std::cout << '\n' << "verification sink: " << g_sink << '\n';
    std::cout << (g_failures == 0 ? "RESULT: SUCCESS\n" : "RESULT: FAILURE\n");
    return g_failures == 0 ? 0 : 1;
}
