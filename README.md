# Thermal Governor

**Thermal Governor is a vendor-neutral C++20 runtime for governing thermal domains, temperature
headroom, derating, throttling constraints, recovery hysteresis, rack thermal envelopes, and
temperature-driven execution authority as a first-class control plane.**

Version 1.0.0. Apache License 2.0.

---

## The core question

> Given current thermal-domain state, temperature evidence, headroom, throttle state, rack or node
> thermal envelope, workload thermal characteristics, infrastructure generations, and policy: what
> execution capability remains thermally legal *now*, what derating or restriction is required, and
> under what fresh evidence may full capability return?

Thermal Governor answers that question as a runtime, not as a formula.

## What Thermal Governor owns

- thermal-domain identity and membership;
- device, node and rack thermal envelopes;
- synthetic rack thermal envelopes where physical evidence does not exist;
- cooling-zone identity where available;
- temperature evidence, evidence generations and provenance;
- thermal headroom, safety margins, uncertainty margins;
- warning, derating, critical and recovery thresholds;
- thermal-state transitions and thermal derating;
- throttling constraints;
- temperature-driven admission and execution authority;
- recovery hysteresis and recovery gating;
- thermal-domain coupling and thermal propagation;
- thermal-domain saturation and restriction authority;
- typed mitigation intents;
- post-action verification;
- stale-evidence and stale-generation rejection at every level;
- deterministic policy evaluation and deterministic explanations;
- durable thermal policies and configuration, and historical thermal-governance records.

## What Thermal Governor does not own

Thermal Governor is **not** a power runtime. It does not implement, and must not absorb:

- general power budgets, power reservations, power allocation or general power-cap governance;
- energy accounting or performance-per-watt optimisation;
- generic SLO management, cost optimisation, workload scheduling or workload placement;
- resource brokerage, generic accelerator-health diagnosis, generic contention resolution;
- generic failure semantics, generic recovery strategy or generic workload migration;
- generic topology discovery;
- arbitrary clock-management policy unrelated to thermal protection;
- facility HVAC control, BMS implementation, fan-controller firmware, PDU control or
  cooling-plant automation;
- GPU memory management, model lifecycle, storage or networking.

It may **consume evidence** from adjacent systems and it may **emit typed intents** to them. It does
not absorb them.

The defining distinction is:

> A device reports a temperature.

versus:

> Under this exact thermal-domain generation, policy generation, telemetry generation, device
> generation, worker incarnation, and coordinator epoch, this resource is authorised to execute only
> within this explicit thermal envelope, and current evidence proves that authority remains valid.

## Core principles

- **Temperature is evidence, not authority.** A reading below a vendor maximum does not make
  unrestricted execution legal; a reading above a warning threshold does not automatically imply
  shutdown. Thermal state is policy-evaluated.
- **UNKNOWN never silently becomes SAFE.** Neither does UNSUPPORTED become UNKNOWN.
- **Hard thermal constraints dominate ranking.** Workload importance, utilisation, cost and
  performance preference cannot rescue a failed hard thermal predicate.
- **Thermal authority is generation-bound.** No thermal decision survives relevant generation
  advancement without revalidation.
- **ACKNOWLEDGED is not EFFECTIVE.** A mitigation command that was accepted is not proven effective
  until fresh thermal evidence verifies the result.
- **Recovery is explicit.** A resource does not regain full capability because time passed, one
  cooler sample arrived, a worker restarted, a device reset, the coordinator restarted, or
  throttling disappeared temporarily.

## Thermal state model

| State | Meaning |
| --- | --- |
| `NORMAL` | Below the warning threshold. |
| `WARM` | At or above the warning threshold. |
| `NEAR_LIMIT` | Inside the configured band immediately below the derating threshold. |
| `DERATED` | At or above the derating threshold: proactively restricted before hardware throttling. |
| `THROTTLING` | Actual thermal throttle evidence observed. Distinct from `DERATED`. |
| `CRITICAL` | At or above the critical threshold: the strongest legal restriction applies. |
| `REVALIDATION_REQUIRED` | Evidence is stale, invalidated, or generation-bound to superseded state. |
| `UNKNOWN` | No trustworthy thermal knowledge. |
| `UNSUPPORTED` | The capability genuinely does not exist on this host. |

`DERATED` and `THROTTLING` are never collapsed. `THROTTLING` requires real throttle
reason evidence; not all clock reduction is thermal throttling, and the runtime distinguishes
`THERMAL_THROTTLE_OBSERVED`, `NON_THERMAL_THROTTLE_OBSERVED`, `NO_THROTTLE_OBSERVED`,
`THROTTLE_UNKNOWN` and `THROTTLE_UNSUPPORTED`.

## Thermal-domain model

Thermal domains are first-class runtime objects. Domain scopes: `ACCELERATOR`,
`ACCELERATOR_PARTITION`, `NODE`, `CHASSIS`, `RACK`, `COOLING_ZONE`,
`COUPLED_RESOURCE_GROUP`.

Every domain carries a `ThermalDomainId`, a `ThermalDomainGeneration`, its type, its typed
members, an optional parent and children, its current state, current evidence, effective headroom,
derating state, governing policy, execution authority, coupling relationships, recovery
eligibility, evidence provenance, and an explicit `REAL` / `SYNTHETIC` /
`UNSUPPORTED` classification. Registration rejects an undefined provenance: a domain must
declare honestly what it is.

Aggregation from members to a node, rack or group is always performed by a declared rule, and the
rule that produced a result is exposed: `HOTTEST_MEMBER_GOVERNS`,
`CONFIGURED_WEIGHTED_RULE`, `NODE_SENSOR_GOVERNS`, or `SYNTHETIC_AGGREGATION`.

## Thermal evidence

Temperature is evidence, not authority. Every observation carries its evidence identity, typed
subject identity and subject generation, thermal-domain identity and generation, temperature,
measurement source, provenance, measurement sequence, telemetry generation, worker identity and
worker boot identity, coordinator epoch, measurement timestamps, capability and topology
generations, integrity status, and optional throttle, clock, fan, cooling, thermal-limit and
confidence evidence.

- Identical duplicate evidence is idempotent.
- Conflicting duplicate evidence is rejected with `DUPLICATE_CONFLICT`.
- Evidence older than what is already held is rejected with `STALE_TELEMETRY`.
- Telemetry sequencing is scoped to a worker incarnation, so a reincarnated worker legitimately
  restarts its own numbering without being mistaken for stale traffic.
- Freshness is computed from measurement time against the policy window, never inferred from
  arrival order.
- A record whose provenance overstates its measurement source is rejected outright: a synthetic
  measurement cannot be labelled `REAL`, and a synthetic policy configuration cannot promote
  telemetry.

## Thermal headroom

Usable headroom is never defined as *limit minus current temperature*.

    governing limit    = min(policy critical, REAL vendor temperature limit)
    raw headroom       = governing limit - current temperature
    effective headroom = raw headroom - policy safety margin - uncertainty margin

The recovery gate is handled separately as `recovery threshold + recovery margin`. Effective
headroom is deliberately **not** clamped: a negative value is an honest deficit and is exactly what
admission predicates test.

## Thermal envelope

The envelope of a governed resource is queryable and generation-bound. It exposes current state,
maximum legal temperature, the vendor limit where one genuinely exists, thresholds, the full
headroom breakdown, permitted concurrency, permitted execution class, permitted clock ceiling where
supported, permitted workload intensity, prohibited mitigation intents, required mitigation, and
structured reasons. It carries the coordinator epoch, policy generation, telemetry generation,
capability generation, topology generation and recovery generation that make it valid.

## Thermal policy

A policy declares warning, derating, critical and recovery thresholds, a near-limit band, safety,
uncertainty and recovery margins, evidence freshness and recovery-sample requirements, UNKNOWN and
unsupported-capability behaviour, coupling and co-derating rules, workload restrictions, admission
and concurrency restrictions, permitted mitigation intents, emergency restrictions, recovery
requirements and required post-action verification.

Hard constraints execute before ranking, and no ranking stage can make an illegal state legal.
Impossible policies are rejected deterministically: inverted thresholds, non-positive near-limit
bands that would place `NEAR_LIMIT` below `WARNING`, negative margins, non-monotonic
concurrency ceilings, duplicate workload profiles, undefined intent bits, and non-finite values.

Decisions are `ALLOW`, `ALLOW_DERATED`, `DEFER`, `DENY`,
`REVALIDATION_REQUIRED`, `UNKNOWN`, `UNSUPPORTED`, each with structured reason
codes.

## Derating

Derating is first-class: `FULL_CAPABILITY`, `DERATED_CLOCK`, `DERATED_ADMISSION`,
`DERATED_CONCURRENCY`, `DERATED_WORKLOAD_CLASS`, `DERATED_COMBINED`,
`NO_SAFE_EXECUTION`, `REVALIDATION_REQUIRED`, plus `UNKNOWN` and
`UNSUPPORTED`. A derating decision binds the affected domain and resources, the triggering
evidence, the previous and new envelopes, all relevant generations, action authority, recovery
conditions and a deterministic explanation.

**Derating cannot expand authority.** A derated resource is never reported as
`FULL_CAPABILITY`.

## Throttling constraints

Thermal Governor owns constraints caused by thermal state and exposes clock ceilings, concurrency
ceilings, admission ceilings, workload-class restrictions, execution denial and containment
requests. It does not build generic clock-management policy, and it consumes real throttle reasons
when a backend genuinely reports them.

## Thermal admission

`evaluate_admission` answers whether new work is thermally admissible and returns a typed
thermal constraint: `ADMIT`, `ADMIT_DERATED`, `DEFER`, `DENY`,
`REVALIDATION_REQUIRED`, `UNKNOWN`, `UNSUPPORTED`. This is thermal eligibility
only. It is not a scheduler, and it does not implement generic admission control.

## Hysteresis and recovery

Hysteresis is mandatory and prevents an 80 to 79 to 80 to 79 oscillation from flapping authority.
Leaving a restrictive state requires positive fresh evidence, never merely the absence of a
violation.

Recovery may require, per policy: temperature strictly below `recovery threshold + recovery
margin`; a required number of consecutive admissible samples; a minimum evidence span; no thermal
throttle observed; positive effective headroom; coupled domains recovered; the current device
generation, the current worker boot and the current coordinator epoch; and explicit operator
authorisation.

The qualifying run stops at the first witness from a superseded incarnation or generation, so stale
witnesses can never accumulate toward recovery. Duration-dependent policy uses an injectable clock;
no policy path sleeps.

## Domain coupling and propagation

Coupling relationships are explicit, typed (`SHARES_CHASSIS`, `SHARES_AIRFLOW_PATH`,
`SHARES_COOLING_ZONE`, `SHARES_HEATSINK`, `THERMALLY_COUPLED`,
`SYNTHETIC_COUPLING`, `UNKNOWN_COUPLING`), generated, provenance-classified, and
weighted. Real physical coupling is never inferred from proximity.

An edge `source -> destination` means the source heats the destination, so a domain is
pressured by the domains that point **at** it; the runtime traverses the graph in that direction.
Propagation is classified `NONE`, `LOCAL`, `DOMAIN`, `COUPLED_DOMAIN` or
`UNKNOWN`, and `UNKNOWN` never becomes `NONE`. Traversal is cycle-tolerant and
depth-bounded, and the smallest legal restriction set is computed: a hot domain co-derates the
domains policy says are coupled to it and leaves unrelated domains untouched.

## Rack and node thermal envelopes

Rack-level envelopes are in scope with explicit provenance. When real rack telemetry exists the
envelope is `REAL`; when rack behaviour is modelled from synthetic domain evidence it is
`SYNTHETIC`; when rack thermal evidence cannot be represented meaningfully it is
`UNSUPPORTED`. Physical cooling telemetry is never fabricated. Node envelopes aggregate or
constrain member accelerators using a declared rule, and the rule that produced the result is always
exposed.

## Authority model

Every thermal mitigation action is generation-bound to the coordinator epoch, action identity and
generation, thermal-domain identity and generation, device identity and generation where relevant,
policy generation, telemetry generation, worker identity and boot identity where relevant, topology
generation and capability generation.

Immediately before dispatch, the action is revalidated against the generations the runtime actually
holds, read from current evidence rather than from the plan. A stale plan does not execute. Stale
coordinator epochs, worker boots, device generations, domain generations, policy generations,
telemetry generations, action generations and topology generations are rejected, and a stale action
result cannot mutate current state.

Action lifecycle: `PLANNED`, `AUTHORIZED`, `DISPATCHED`, `ACKNOWLEDGED`,
`VERIFYING`, `EFFECTIVE`, `PARTIALLY_EFFECTIVE`, `INEFFECTIVE`,
`WORSENED`, `FAILED`, `SUPERSEDED`, `CANCELLED`, `OUTCOME_UNKNOWN`. A
backend returning success proves nothing about thermal effect; verification decides.

## Post-action verification

Verification always consumes fresh evidence: an observation taken after the mitigation was
dispatched. It re-checks temperature, effective headroom, temperature direction, throttle state,
device generation, domain generation, telemetry generation, policy generation, worker boot and
coordinator epoch, and reports `THERMAL_STATE_IMPROVED`, `THERMAL_STATE_UNCHANGED`,
`THERMAL_STATE_WORSENED`, `DERATING_EFFECTIVE`, `DERATING_PARTIALLY_EFFECTIVE`,
`DERATING_INEFFECTIVE`, `SECONDARY_VIOLATION_CREATED`, `RECOVERY_ALLOWED`,
`RECOVERY_FORBIDDEN` or `OUTCOME_UNKNOWN`.

## Worker incarnation fencing

`WorkerId` and `WorkerBootId` are distinct. A worker restart creates a new boot identity.
When a worker dies, its live authority is revoked, delayed telemetry and delayed action completions
from the old boot are rejected, committed history is preserved, dynamic evidence becomes
`REVALIDATION_REQUIRED`, and fresh evidence from the new incarnation is required. A new
incarnation never inherits the previous one's authority, and boot identity can never move backwards.

## Coordinator restart

A coordinator restart advances `CoordinatorEpoch`. Durable policy, domain and coupling
configuration, device registration and history are recovered; live telemetry is not. Old-epoch
traffic is rejected, dynamic thermal evidence becomes `REVALIDATION_REQUIRED`, unresolved
actions are recovered conservatively as `OUTCOME_UNKNOWN`, and current thermal authority
returns only after workers reconnect and publish fresh evidence. Restoring device registration is
what lets the runtime say *revalidation required* rather than *unknown*: it knows what it could
measure, and it knows it holds no current measurement.

## Persistence

Durable state is versioned, bounded, integrity-checked, checked-arithmetic safe, and rejects
corruption, truncation, trailing garbage, unsupported versions, invalid enumerations and duplicate
identities where duplication is illegal. The container declares a magic number, a format version, a
record count, record lengths and checksums.

Writes are transactional: serialise, write a unique temporary sibling, flush, close, atomically
replace the authoritative file, and clean up. Windows replacement semantics use `ReplaceFileW`
with a `MoveFileExW` fallback. An intended-durable mutation is never acknowledged before the
flush crosses the durability boundary. Concurrent saves of the same destination are serialised and
each uses its own scratch file.

The checksums are CRC-32C accidental-corruption and truncation detectors. They are not
authenticators: there is no shared secret anywhere in the format, so an editor who recomputes an
inner checksum can always recompute the outer one. Thermal Governor makes no cryptographic claim.

## Transport

Distributed deployment uses real framed TCP between independent operating-system processes. Frames
carry a magic number, a protocol version, a message kind, a payload length, a header checksum and a
payload checksum, and the checksum coverage is exact: the header checksum covers the header fields
that precede it and never itself, and the payload checksum covers exactly the payload bytes. Partial
reads and writes are handled; one send is never assumed to equal one receive.

Rejected: bad magic, unsupported version, truncated header, truncated payload, oversized payload,
invalid checksum, malformed identity, invalid enumeration, impossible length, stale coordinator
epoch, stale worker boot, stale device generation, stale domain generation, stale policy generation,
stale telemetry generation, stale action generation and conflicting duplicates.

## Concurrency model

All mutation is serialised under an internal mutex that is never held across network waits,
callbacks, backend waits, persistence I/O or thread joins. Readers obtain an immutable
copy-on-write snapshot through a published shared pointer and never observe a partially mutated
generation set.

Telemetry publication, policy evaluation, snapshot reads, explanations, action completion,
verification, policy updates and persistence are all safe to run concurrently. A single observed
snapshot is internally consistent: the coordinator epoch cannot change beneath a reader, and history
is appended under the same lock that produced the state change.

The lock and re-entrancy audit is manual and explicit: no read lock is followed by a write
acquisition before releasing the read guard; no write path reacquires the same lock; no event is
emitted under a lock; no network wait, backend wait, thread join or persistence I/O happens under
the contested lock; no callback re-enters mutable state; and no inconsistent nested acquisition
order exists. A session registry lets shutdown unblock idle sessions before joining their threads,
rather than waiting on a client that has nothing more to say.

## Real hardware telemetry

The backend interface is narrow and vendor-neutral: `query_device_identity`,
`query_temperature`, `query_temperature_limit`, `query_shutdown_limit`,
`query_throttle_reasons`, `query_current_clock`, `query_max_clock`,
`query_fan_state`, `query_cooling_state`, `query_node_temperature`,
`query_rack_temperature`, and an optional enforcement path. No vendor SDK object ever crosses
the public boundary.

Each capability is resolved **independently**. One working telemetry source never implies that
another works, and the runtime reports each one as `SUPPORTED_REAL`,
`SUPPORTED_READ_ONLY`, `SUPPORTED_SYNTHETIC`, `UNSUPPORTED` or `UNKNOWN`.

### NVIDIA / NVML

`NvmlThermalBackend` loads NVML at run time from the platform library path. On the host used
for this release it reports driver 616.92 and NVML 13.616.92, one device (NVIDIA GeForce RTX 5090,
UUID `GPU-d1056bb6-...`), and this capability matrix:

| Capability | Result |
| --- | --- |
| `DEVICE_IDENTITY`, `TEMPERATURE`, `TEMPERATURE_LIMIT` | `SUPPORTED_REAL` |
| `TEMPERATURE_THRESHOLD_SHUTDOWN`, `TEMPERATURE_THRESHOLD_SLOWDOWN` | `SUPPORTED_REAL` |
| `THROTTLE_REASONS`, `CURRENT_CLOCK`, `MAX_CLOCK`, `FAN_TELEMETRY` | `SUPPORTED_REAL` |
| `COOLING_TELEMETRY`, `THERMAL_DOMAIN_METADATA` | `UNSUPPORTED` |
| `NODE_THERMAL_METADATA`, `RACK_THERMAL_METADATA` | `UNSUPPORTED` |
| `ACTUAL_THROTTLE_ENFORCEMENT`, `CLOCK_REDUCTION_ENFORCEMENT` | `UNSUPPORTED` |

NVML exposes no coolant, airflow, node or rack sensor, so those capabilities are reported
`UNSUPPORTED` rather than as zero cooling. Thermal Governor does not own clock enforcement
through NVML and says so rather than pretending to act.

No AMD, Intel or other vendor backend is implemented, and none is claimed.

### CUDA

With `THERMAL_GOVERNOR_ENABLE_CUDA=ON` the runtime contains a real, bounded CUDA completed-work
proof: `cudaMalloc`, host-to-device copy, a deterministic integer-mixing kernel, device
synchronisation, device-to-host copy, an exact CPU recomputation and `cudaFree`. On the host
used for this release it reports the RTX 5090 with 170 SMs, driver 13040, runtime 12090, 32
iterations over 262 144 elements, a device checksum equal to the host checksum, zero mismatches,
every device allocation released, and a small bounded runtime. The workload is deliberately sized to
produce meaningful telemetry without stressing hardware; no throttling claim is made unless
throttling is actually observed, and cooling is never disabled or defeated.

CUDA reports temperature through NVML, because CUDA itself exposes no temperature API.

## Provable, modelled, and absent

- **REAL**: Windows operating-system processes, TCP, durable persistence, process kill and crash,
  RTX 5090 temperature, NVML throttle state, actual clocks, real NVML limits, a real CUDA workload,
  exact CPU parity, device-memory release.
- **SYNTHETIC**: rack thermal envelopes without rack sensors, multiple thermal zones on one host,
  multi-GPU thermal topologies with one GPU, synthetic cooling-domain relationships, and artificial
  threshold policies used to exercise derating safely.
- **UNSUPPORTED**: physical rack cooling controllers, BMS integration, liquid-loop telemetry when
  absent, PDU control, facility HVAC, multi-GPU physical coupling without hardware, node and rack
  temperature sensors on a workstation, and any vendor SDK that is not present.

Provenance is never silently upgraded. A synthetic rack envelope stays synthetic; an absent sensor
stays absent.

## Deterministic explanations

Every evaluation produces a stable structured explanation with fixed section order: thermal domain,
temperature, headroom, throttle, thermal state, restrictions, rejected alternatives, recovery,
reasons and authority. Two evaluations over equal inputs render byte-identical text. The "Rejected"
section names each alternative that was considered and why it was refused.

## Public API

Registration (`register_device`, `register_thermal_domain`,
`update_thermal_domain`, `register_coupling`, `remove_coupling`,
`publish_capabilities`), policy (`set_policy`, `get_policy`), workers
(`register_worker`, `worker_heartbeat`, `note_worker_death`), evidence
(`publish_temperature`, `publish_throttle_evidence`, `publish_cooling_evidence`,
`publish_node_temperature`, `publish_rack_temperature`), evaluation
(`evaluate_domain`, `evaluate_device`, `evaluate_admission`,
`query_headroom`, `query_envelope`, `query_state`, `evaluate_recovery`),
recovery (`authorize_recovery`, `revoke_recovery`), actions (`authorize_derating`,
`authorize_mitigation`, `record_action_dispatch`, `record_action_ack`,
`record_action_result`, `verify_action`, `cancel_action`), reads (`snapshot`,
`explain`, `explain_admission`, `history`, `actions`,
`verifications`), and durability (`persist`, `load_durable`, `shutdown`).

Mutable internals are never exposed. Ordinary policy outcomes are values, not exceptions.

## Strongly typed identities

`CoordinatorId`, `CoordinatorEpoch`, `WorkerId`, `WorkerBootId`,
`DeviceId`, `DeviceGeneration`, `AcceleratorId`, `NodeId`,
`NodeGeneration`, `RackId`, `RackGeneration`, `CoolingZoneId`,
`ThermalDomainId`, `ThermalDomainGeneration`, `WorkloadId`, `ExecutionId`,
`ThermalPolicyId`, `ThermalPolicyGeneration`, `TelemetryGeneration`,
`EvidenceId`, `ActionId`, `ActionGeneration`, `VerificationGeneration`,
`CapabilityGeneration`, `HealthGeneration`, `TopologyGeneration`,
`RestrictionId`, `RestrictionGeneration` and `RecoveryGeneration` are distinct
compile-time-tagged types. A `ThermalDomainId` is not a `DeviceId`, a
`DeviceGeneration` is not a `TelemetryGeneration`, and a `WorkerId` is not a
`WorkloadId`. Accidental cross-domain mixing does not compile.

Temperatures and thresholds are `DegreesCelsius`, differences are `TemperatureDelta`,
ceilings are `Percent` and clocks are `MegaHertz`. There are no ambiguous bare numbers
for thermal quantities.

## Building

Requirements: CMake 3.21 or newer and a C++20 compiler. Windows x64 with MSVC 2022 (toolset 14.44)
is the validated platform; the POSIX code paths are present but are **UNSUPPORTED** and unvalidated.

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build

Options:

| Option | Default | Effect |
| --- | --- | --- |
| `THERMAL_GOVERNOR_BUILD_TESTS` | ON (top level) | Build the test suites. |
| `THERMAL_GOVERNOR_BUILD_EXAMPLES` | ON (top level) | Build the examples. |
| `THERMAL_GOVERNOR_BUILD_BENCHMARKS` | ON (top level) | Build the benchmark suite. |
| `THERMAL_GOVERNOR_BUILD_CLI` | ON (top level) | Build the CLI, the coordinator and the worker. |
| `THERMAL_GOVERNOR_ENABLE_NVML` | ON | Build the dynamically loaded NVML backend. |
| `THERMAL_GOVERNOR_ENABLE_CUDA` | OFF | Build the real CUDA completed-work proof. |
| `THERMAL_GOVERNOR_ENABLE_ASAN` | OFF | Build with MSVC AddressSanitizer. |
| `THERMAL_GOVERNOR_WARNINGS_AS_ERRORS` | ON | Treat first-party warnings as errors. |

The core library never requires CUDA or NVML: with both disabled it builds and runs with no GPU
stack present at all. First-party code compiles cleanly under `/W4 /WX /permissive-`.

## Installing and consuming

    cmake --install build --prefix <prefix>

    find_package(ThermalGovernor CONFIG REQUIRED)
    target_link_libraries(app PRIVATE ThermalGovernor::ThermalGovernor)

The install exports the library, the public headers, the exported target set,
`ThermalGovernorConfig.cmake` and `ThermalGovernorConfigVersion.cmake`. A downstream
project that uses only the installed artifacts is provided in `consumer/`; it is not part of
this build and is configured separately against an install prefix to prove that no header is
resolved from the source or build tree. A CUDA-enabled install declares the CUDA runtime dependency
through the package config; the default install declares none.

## CLI, coordinator and worker

`thermal_governor_coordinator` hosts a governor and serves the framed protocol.
`thermal_governor_worker` registers a device, publishes evidence and executes mitigation
intents through a backend. `thermal_governor_cli` is a thin client: it connects to a running
coordinator, so every mutation travels exactly the same authority path a library caller would use.
There are no fake controls.

Read commands: `status`, `devices`, `device <id>`, `domains`,
`domain <id>`, `temperature <id>`, `headroom <id>`, `throttle <id>`,
`recovery <id>`, `policy`, `evaluate <id>`, `explain <id>`,
`admission <domain> <workload> <profile>`, `actions`, `history`,
`snapshot-info`.

Mutation commands: `set-thresholds`, `register-device`, `register-domain`,
`register-coupling`, `authorize-derating`, `authorize-mitigation`,
`dispatch`, `cancel`, `bump-domain-generation`, `bump-device-generation`,
`worker-death`, `persist`, `shutdown`.

## Examples

`examples/` contains runnable programs covering basic temperature evaluation, thermal
headroom, derating and hysteresis, recovery gating, thermal-domain coupling, a synthetic rack
thermal envelope, stale telemetry rejection, deterministic explanation, real NVML telemetry, and a
real CUDA completed-work proof. Each compiles against the public API only and exits non-zero on
failure.

## Tests

Every suite supports stable case identifiers, exact one-case execution (`--case
<suite>::<case>`), listing (`--list`), filtering (`--filter <prefix>`) and immediate
unbuffered progress: `BEGIN`, `PHASE`, `PASS`, `FAIL`. There are no test
timeouts, no watchdogs, no forced termination and no sleeps; concurrency and multiprocess cases
synchronise explicitly, and a hang is treated as a defect to diagnose rather than to paper over.

The suites are `quantity`, `identity`, `policy`, `headroom`,
`evidence`, `coupling`, `protocol`, `persistence`, `transport`,
`explanation`, `state_machine`, `governance`, `actions`, `recovery`,
`fencing`, `restart`, `property`, `adversarial`, `concurrency`,
`multiprocess`, `nvml` and `cuda`: 229 cases covering unit behaviour, integration,
end-to-end flows, the state machine, property and seeded-randomised invariants, adversarial
corruption and stale-authority attacks, multi-threaded stress with deterministic races, real
multiprocess worker death and reincarnation, real coordinator crash and restart, real NVML telemetry
and a real CUDA workload.

`tests/asan_self_test.cpp` deliberately triggers a sanitizer report to prove that
instrumentation is genuinely active; it is never registered as a passing test.

## Benchmarks

`benchmarks/thermal_governor_bench.cpp` measures completed operations with
`std::chrono::steady_clock` while the governor runs on a monotonic manual clock, so
measurements are not confounded by wall time. It reports temperature ingestion, policy evaluation,
headroom computation, recovery evaluation, deterministic explanation, snapshot acquisition,
evaluation at 100 / 1 000 / 10 000 registered domains, persistence and durable restore, and
concurrent read/evaluate throughput. Precision is not manufactured: iteration counts are printed and
results are given to two decimals.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
