# Migration Safety Runtime Integration Remediation

## Original Blocker

The runtime created a migration safety manager with only a feedback callback. It supplied an intentionally incomplete request and registered no Phase 7 execution, validation, or rollback callback.

## Changes

- Under `AWAVMA_RUNTIME_TESTING`, page-recovery fixtures supply an active attempt ID independently from
  the immutable page-checkpoint attempt ID. Attempt mismatch and incomplete-checkpoint rejections use the
  existing Safety Manager `finish()` path, producing one Phase 8 event without invoking rollback.

### Page Recovery Closure

The runtime recovery suite now covers complete, partial, failed, verification-failed, identity-changed,
attempt-mismatch, and incomplete-checkpoint outcomes. Complete/partial/failed/query/identity cases invoke
the registered `runtime_rollback_migration()` and the real provider; cross-attempt and incomplete inputs
are safely finalized by the testing seam before callback execution. Every case records exactly one terminal
feedback call and one matching Phase 8 row with its detailed page result. The normal binary excludes all
testing seam and adapter symbols. Production page execution remains blocked by lack of authoritative page
addresses, and thread affinity recovery remains unaffected.

- `src/awavma_runtime.c` now initializes the existing `Migration_Init()` module with runtime-root-specific result, history, log, and state paths.
- The runtime registers `runtime_execute_migration`, a thin production wrapper around `Migration_Execute()`.
- The runtime registers `runtime_validate_migration`, which performs attempt-bound structural identity and requested-affinity verification. It does not derive benefit, degradation, or application health.
- The runtime registers `runtime_rollback_migration`, which restores a captured binary CPU-affinity checkpoint for thread migrations. Page-placement rollback remains unavailable.
- `MigrationSafetyManager` now permits a valid `MOVE_THREAD` request when source and destination NUMA nodes are equal. The distinct-node constraint remains for memory migration.

## Safety Boundary

`migration_execution_enabled` remains false by default. The runtime still has no trustworthy candidate metadata provider for a requested action, destination CPU/node, verified page region, or comparable after-observation. It therefore constructs no executable Phase 7 candidate in normal operation and cannot invoke the registered executor on the single-node host.

The registered callbacks are production callbacks, not test mocks. They do not fabricate placement, a second NUMA node, successful validation, or rollback capability.

## Tests

| Command | Result |
|---|---|
| `make awavma-runtime` | PASS |
| `make test-awavma-runtime` | PASS |
| `make test-migration-safety-manager` | PASS |

## Environment-Limited

- Real cross-node thread/page migration: one NUMA node only.
- Memory migration and rollback: no runtime source of verified page addresses, ownership, or original placement.
- Real post-migration benefit validation: no comparable Phase 3 before/after locality or throughput observation.

## Remaining Risk And Status

The blocker is **PARTIALLY RESOLVED**. The production manager now owns real Phase 7 execution/validation/rollback callback registrations, but the required real metadata provider and recoverable checkpoint are not implemented. Enabling execution would still be unsafe, so it remains disabled.

## Runtime Integration Remediation - Metadata, Checkpoint, Validation, Rollback

### Audit

| Area | Existing and reusable data | Boundary |
|---|---|---|
| Identity | Application discovery/runtime records retain PID and `start_time_ticks`; `/proc/<pid>/stat` supports fresh revalidation. | Supported. |
| Application lifecycle | Application manager has discovered/active/inactive/terminated state and `job_in_progress`. | Runtime coordinator does not retain an attempt-specific migration state. |
| CPU placement | Linux `sched_getaffinity()` and `/proc/<pid>/stat` processor field are available. | CPU-to-NUMA mapping is unavailable without libnuma and stays unknown. |
| Page placement | Phase 3 records aggregate `/proc/<pid>/numa_maps` summaries. Phase 7 can query caller-provided page addresses with `move_pages()`. | No tracked page-address-to-node mapping; aggregate evidence cannot support page rollback. |
| Decision | Phase 5 action, gain/cost, source/destination nodes; Phase 6 confidence and final decision exist. | Runtime adapter intentionally emits unavailable placement fields and no CPU-mask target. |
| Metrics | `/proc/<pid>/stat` exposes cumulative user+system CPU ticks; Phase 3 records CPU utilization, RSS, page faults, thread count, and aggregate NUMA summary. | No attempt-bound comparable Phase 3 window is retained by the runtime. |

Exact files changed: `include/runtime_migration_metadata.h`, `src/runtime_migration_metadata.c`, `src/awavma_runtime.c`, `tests/runtime_migration_metadata_test.c`, and `Makefile`.

### Implemented

`runtime_get_migration_metadata()` reads fresh live `/proc/<pid>/stat` identity, process CPU-time counter, last observed CPU, and `sched_getaffinity()` mask. Every observation has an explicit availability flag; unknown CPU and NUMA node are `-1` with their corresponding flag false. It does not claim page placement.

`RuntimeMigrationCheckpoint` stores `cpu_set_t` by value, with PID, start ticks, capture timestamp, and validity. `runtime_migration_restore_affinity()` revalidates identity before `sched_setaffinity()`, reads back the result, and requires exact `CPU_EQUAL()` verification. Checkpoint release clears the owned value; no dynamic page storage is allocated.

The runtime acquires and releases a fresh metadata/checkpoint pair immediately before the existing safety-manager attempt. No target is supplied, so the manager still rejects the request before execution. This is intentional under the selected no-target-provider policy.

### Capability Matrix

| Capability | Production Provider | Real Data | Tested | Status |
|---|---|---|---|---|
| Process identity | Yes | `/proc` | Yes | SUPPORTED |
| CPU affinity metadata | Yes | `sched_getaffinity()` | Yes | SUPPORTED |
| NUMA topology | No runtime provider | None | No | UNSUPPORTED |
| Page placement | No | Aggregate monitor data only | Yes (unavailable) | UNSUPPORTED |
| Before metrics | Partial | CPU-time counter | No attempt window | PARTIAL |
| After metrics | Partial | Fresh CPU-time counter | No attempt window | PARTIAL |
| CPU-affinity checkpoint | Yes | Binary `cpu_set_t` | Yes | SUPPORTED |
| Page checkpoint | No | No page mapping | Yes (unavailable) | UNSUPPORTED |
| Migration executor | Yes | Existing Phase 7 | Existing tests | SUPPORTED |
| Placement verification | Thread only | `sched_getaffinity()` | Existing tests | PARTIAL |
| Benefit validation | No | No comparable attempt window | No | UNSUPPORTED |
| CPU-affinity rollback | Yes | Linux affinity API | Dedicated child | SUPPORTED |
| Page rollback | No | No page mapping | No | UNSUPPORTED |
| Phase 8 feedback | Yes | Terminal feedback | Existing tests | SUPPORTED |

### New Test Evidence

`make test-runtime-migration-metadata` passed:

- `MD01_LIVE_PID`
- `MD02_IDENTITY_AND_AFFINITY`
- `C01_BINARY_CHECKPOINT`
- `C04_REAL_CPU_AFFINITY_ROLLBACK`
- `C05_IDENTITY_MISMATCH`
- `C03_CHECKPOINT_RELEASE`
- `MD04_PROCESS_GONE`

The rollback test uses only a spawned child. It checkpoints an exact allowed CPU set, changes that child to a valid CPU subset, invokes the production rollback function, and verifies the original binary mask is restored. The identity-mismatch test confirms rollback refuses to modify the process.

Additional regression commands passed: `make awavma-runtime`, `make test-awavma-runtime`, `make test-migration-safety-manager`, `make test-migration`, `make test-validation`, `make test-feedback`, `make test-continuous-monitor`, and `make profile-awavma-runtime`.

### Status

The software integration remains **PARTIALLY RESOLVED**. CPU-affinity metadata and rollback are real and tested; production execution remains disabled and target metadata remains deliberately unavailable. Page rollback and meaningful benefit validation cannot be honestly implemented from the current tracked runtime data. Cross-NUMA migration remains environment-limited on the single-node host.

### Utility-Evidence Benefit Contract

Phase 5 uses normalized policy utilities, not physical gain/cost estimates. The runtime explicitly labels this as `UTILITY_POLICY_EVIDENCE`. Phase 6 records `ROI_NOT_APPLICABLE_TO_UTILITY_MODEL` for that evidence model while retaining confidence and safety gates. The benefit classifier validates stored final utilities, directed decision margin, epsilon, versions, action status, identity/generation/migration-ID/attempt binding, structural target evidence, and suppression state without recomputing action ranking.

Canonical physical predicted gain and migration cost remain unavailable. No `[0,100]` conversion was introduced. Complete utility evidence reaches `BENEFIT_POLICY_UNCALIBRATED` until a real production calibration exists; test-only calibrated fixtures still stop at `EXECUTION_DISABLED`. The overall integration remains **PARTIALLY RESOLVED**.

## Migration Target Provider Remediation

### Decision Audit

Phase 5 emits an action (`MOVE_THREAD`, `MOVE_MEMORY`, `NO_MIGRATION`, or insufficient), workload classification, normalized decision factors, raw/final utility scores, biases, epsilon, versions, and a dimensionless `decision_margin`. It does not emit source/destination-node columns, canonical predicted gain, or canonical estimated migration cost. Its persisted `DecisionData` has source/destination node fields, but the runtime Phase 5-to-Phase 6 adapter currently writes unavailable placement data and does not retain a destination CPU mask. Thus Phase 5 decides migration desirability and action type, not an authoritative executable destination.

`Migration_Execute()` requires a non-empty CPU mask or destination CPU for thread migration and a verified target memory node plus page addresses for memory migration. Existing topology support can read live sysfs node CPU lists; the runtime has no CPU-pressure, memory-pressure, page-placement, or target-selection policy input.

### Provider Contract

Added `include/migration_target_provider.h` and `src/migration_target_provider.c`.

- `MigrationTargetInput` binds PID, start ticks, attempt ID, action, source metadata, cross-node semantics, and explicitly authoritative CPU-mask/node candidates.
- `MigrationTarget` copies PID, start ticks, attempt ID, action, availability flags, source provenance, and accepted target fields.
- `MigrationTargetResult` distinguishes available, unavailable, no alternate target, unsupported action, invalid source, identity, topology, policy, invalid, stale, and internal outcomes.
- Provenance is explicit. Production runtime uses `MIGRATION_TARGET_SOURCE_NONE`; `MIGRATION_TARGET_SOURCE_TEST` is only usable by isolated unit topology fixtures.

The provider reads `/sys/devices/system/node/node*/cpulist` for the production topology. A thread mask must be non-empty and wholly online. A memory target must be an existing node and, when cross-node movement is required, differ from the known source. Numeric zero is never a target unless its corresponding availability flag is true.

### Runtime And Feedback Integration

`MigrationSafetyConfig.target_fn` is registered to the production `runtime_get_migration_target()` callback. The safety manager invokes it after identity/cooldown checks and before its pre-execution gate. It rejects non-available results, binds accepted metadata to the generated attempt ID, and centralizes terminal feedback.

New Phase 8 terminal outcomes distinguish target unavailable, no alternate target, invalid, stale, identity mismatch, and topology unavailable. No target-provider result can invoke the executor directly.

### Tests

`make test-migration-target-provider` passed:

- TP01 no target returns `TARGET_UNAVAILABLE`.
- TP02 real semantics for a single-node topology return `NO_ALTERNATE_TARGET` for a cross-node memory request.
- TP03 invalid node, TP04 empty mask, and TP05 offline CPU return `TARGET_INVALID`.
- TP09 constructs a valid target from a clearly test-only synthetic two-node topology.
- TP10 proves a zero-initialized target is not node zero.
- TP12 proves memory migration requires an explicit memory-node target.

Also passed: `make awavma-runtime`, `make test-awavma-runtime`, `make test-migration-safety-manager`, `make test-feedback`, and `make profile-awavma-runtime`.

### Target Capability Matrix

| Capability | Status |
|---|---|
| Target Provider API | SUPPORTED |
| Production provider registered | SUPPORTED |
| Target availability flags | SUPPORTED |
| Target identity and attempt binding | SUPPORTED |
| CPU-mask validation | SUPPORTED |
| NUMA-node validation | SUPPORTED |
| Single-node cross-node behavior | SUPPORTED |
| Phase 8 target feedback | PARTIAL |
| Real target-selection policy | NOT IMPLEMENTED |
| Real cross-NUMA execution | ENVIRONMENT-LIMITED |

## Production Runtime Target Provider + Phase 8 Persistence Validation

### Production Call Path

`awavma_runtime_init()` creates the safety manager and registers `runtime_get_migration_target`, `runtime_execute_migration`, `runtime_rollback_migration`, and `record_terminal_feedback`. An approved decision enters the runtime migration boundary, which collects fresh `runtime_get_migration_metadata()` data and a binary affinity checkpoint before calling `migration_safety_manager_attempt()`. The manager calls the registered target provider and every terminal result flows through `finish()`, the registered `record_terminal_feedback()`, and `ProcessFeedback()` persistence at `apps/<app_id>/history/migration_feedback.csv` under the runtime root.

The only relevant target-provider early return is in the manager after `target_fn` returns a non-available result. It calls `finish()` before returning; it does not call the executor or direct persistence path.

### Test-Only Entry

`AWAVMA_RUNTIME_TESTING` exposes `awavma_runtime_test_submit_approved_migration()`. It is compiled only into the production-runtime test binary and supplies a TEST APPROVED DECISION INPUT at the existing runtime migration boundary. It does not replace production callbacks or select a destination. The runtime still owns metadata, checkpoint, target callback, finalization, and Phase 8 persistence.

`tests/runtime_migration_target_production_test.c` uses a dedicated child and an isolated `/tmp/awavma-target-production-XXXXXX` runtime root. It observes the runtime-generated attempt ID and counts matching persistent feedback rows.

| Case | Runtime Provider Calls | Executor Calls | Rollback Calls | `record_terminal_feedback` Calls | Persistent Rows | Result |
|---|---:|---:|---:|---:|---:|---|
| RTPI01 `NO_ALTERNATE_TARGET` | 1 | 0 | 0 | 1 | 1 | PASS |
| RTPI02 `TARGET_UNAVAILABLE` | 1 | 0 | 0 | 1 | 1 | PASS |
| RTPI03 `TARGET_INVALID` | 1 | 0 | 0 | 1 | 1 | PASS |

Each persisted row was checked for the dedicated child PID, actual `start_time_ticks`, runtime-generated attempt ID, and terminal outcome. The feedback artifact is parsed rather than inferred from callback counts alone.

Commands passed: `make test-runtime-migration-target-production`, `make test-runtime-migration-target-integration`, `make test-migration-target-provider`, `make awavma-runtime`, `make test-awavma-runtime`, `make test-runtime-migration-metadata`, `make test-migration-safety-manager`, `make test-migration`, `make test-validation`, `make test-feedback`, `make test-continuous-monitor`, and `make profile-awavma-runtime`.

### Target Provider Status

- Target Provider Infrastructure: **SUPPORTED**
- Production Target Provider Registration: **SUPPORTED**
- Phase 8 Terminal Persistence Integration: **SUPPORTED**
- Target-Selection Policy: **NOT IMPLEMENTED**
- Real Cross-NUMA Validation: **ENVIRONMENT-LIMITED**

The overall runtime migration integration remains **PARTIALLY RESOLVED**. The next unresolved software task is attempt-bound comparable pre/post migration metrics for production benefit validation; page-address checkpoints and rollback also remain unavailable.

## Phase 5 Conservative Target Selection Policy

### Supported Thread Policy

The runtime now preserves one existing Phase 5/Phase 6-approved actionable intent at the migration
safety boundary. Target selection consumes that intent only; it never changes whether migration is
recommended, does not invoke an executor, and does not make a benefit claim.

For `MOVE_THREAD`, current Linux runtime identity, observed CPU, allowed affinity, online NUMA
topology, and safety history are the only inputs. A source node is accepted only when the observed
CPU maps to an online node, or when the entire allowed affinity mask belongs to one node. An affinity
mask spanning nodes without observed CPU evidence returns `TARGET_POLICY_SOURCE_AMBIGUOUS`.

Candidates are real, present, non-source NUMA nodes with an online CPU. Their CPU masks are exactly the intersection of
that node's online CPUs and the process's current allowed affinity/cpuset mask. No CPU pressure,
memory pressure, locality score, topology-distance score, or benefit evidence is fabricated. With
one valid candidate it is selected; with multiple valid candidates, the policy returns
`TARGET_POLICY_AMBIGUOUS` instead of choosing node ID order. Recent equivalent failed targets are
excluded before this decision. Active cooldown/quarantine remains a safety-manager early terminal
gate and is also represented by the policy result when the policy is directly evaluated.

`MOVE_MEMORY` returns `PAGE_RECOVERY_UNAVAILABLE`. No page target is emitted because page address
checkpointing and rollback are unavailable.

Every policy output is bound to PID, start-time ticks, and manager-generated attempt ID. The existing
target provider remains the final structural validation boundary for selected CPU masks/nodes. Phase
8 persists the policy result and evidence in `target_selection_reason`; no existing CSV column is
removed or reordered. `benefit_known` remains false.

### Validation

`make test-phase5-target-selection` passes TS01 through TS12, including real single-node no-alternate
behavior, synthetic two-node selection, synthetic three-node ambiguity, allowed-CPU intersection,
empty intersection, source ambiguity, history suppression, quarantine, identity mismatch, page
recovery blocking, and execution-disabled selection. Terminal policy cases assert exactly one feedback
call, one terminal persistence path, and zero executor calls. `make test-runtime-phase5-target-selection`
also passes `RTPI00_LIVE_POLICY_SINGLE_NODE_NO_ALTERNATE`, using the production runtime callback and
live host metadata with a test-approved upstream intent.

The current host exposes one online NUMA node, so a selected real cross-NUMA thread target cannot be
validated here. Synthetic results demonstrate policy logic only, not hardware benefit or performance.

### Status

| Capability | Status |
|---|---|
| Phase 5 target-selection infrastructure | SUPPORTED |
| Thread target-selection policy | SUPPORTED |
| Page target-selection policy | BLOCKED BY PAGE RECOVERY REQUIREMENTS |
| Target provider | SUPPORTED |
| Production structural validator | SUPPORTED |
| Production benefit classifier | NOT AUTHORIZED |
| Real cross-NUMA target execution | ENVIRONMENT-LIMITED |
| Overall runtime migration integration | PARTIALLY RESOLVED |

## Production Benefit Classifier

`src/benefit_classifier.c` adds a conservative pre-execution classifier. It reuses, but does not
recalculate, the existing Phase 5 selected action and Phase 6 confidence/ROI gate outputs. It has no
new action weights, no new utility formula, and no benchmark-derived threshold claim. The canonical
decision utility and epsilon remain in the existing decision engine.

The classifier binds PID, start-time ticks, manager-generated attempt ID, action, Phase 5/6 evidence,
and target metadata. It verifies the target provider result, current identity, online target mask,
allowed affinity, source/target relationship, cooldown, quarantine, and recent-history suppression.
It returns explicit `BENEFIT_SUPPORTED`, `BENEFIT_NOT_SUPPORTED`,
`INSUFFICIENT_BENEFIT_EVIDENCE`, `BENEFIT_POLICY_UNCALIBRATED`,
`STALE_OR_IDENTITY_MISMATCH`, `ACTION_NOT_ELIGIBLE`, `TARGET_NOT_VALIDATED`, or
`PAGE_RECOVERY_REQUIRED` outcomes. The safety manager records a non-supported result through its
existing exactly-once terminal path before the execution-disabled gate.

Runtime calibration state/provenance defaults to `CALIBRATION_UNAVAILABLE` and
`no_cross_numa_production_calibration`. The single-node host does not validate real cross-NUMA
benefit. Test-only synthetic calibration is guarded by `AWAVMA_RUNTIME_TESTING`; it cannot change
the production default. A classifier-supported synthetic candidate still terminates at
`migration_execution_enabled=false` with zero executor calls.

Phase 8 appends `benefit_classification` and `benefit_reason` to feedback persistence without
removing or reordering any existing CSV fields. `benefit_known` remains false because authorization
evidence is not measured post-migration benefit.

| Capability | Status |
|---|---|
| Production benefit classifier infrastructure | SUPPORTED |
| Thread benefit classification path | SUPPORTED |
| Uncalibrated benefit authorization | BLOCKED |
| Page benefit classification | BLOCKED BY PAGE RECOVERY REQUIREMENTS |
| Real cross-NUMA benefit calibration | ENVIRONMENT-LIMITED |
| Execution default | DISABLED |

## RV06 Idle Fixture Correction

The earlier `RV06` fixture was CPU-active despite its idle label and did not prove the no-progress branch. It now uses separate ready and gate pipes: the child reports ready, then remains blocked in a gate `read()` until validation and all assertions complete. The parent waits for readiness before collecting the BEFORE snapshot and releases the child only during cleanup.

`RV06_IDLE_CHILD_STRUCTURAL_NO_PROGRESS_CLAIM` now observed equal real Linux CPU-time ticks before and after the bounded window. It persisted `progress_known=true`, `progress_observed=false`, and `benefit_known=false`, with one feedback call, one terminal feedback row, and no rollback. No CPU-time advancement over one bounded interval is inconclusive: the process may legitimately be sleeping, idle, or waiting for I/O, so AWAVMA does not infer a stall.

| Case | Child | CPU Tick Delta | Progress Known | Progress Observed | Benefit Known | Structural Result |
|---|---|---:|---|---|---|---|
| RV02 | active | greater than zero | true | true | false | structurally healthy, benefit unknown |
| RV06 | blocked pipe read | zero | true | false | false | no-progress inconclusive |

## Attempt-Bound Structural Snapshot Validation

### Implemented Lifecycle

`MigrationSafetyManager` now generates the attempt ID, resolves the target, applies every pre-execution gate, and only then invokes the runtime `capture_before_fn`. A successful executor result then invokes `runtime_validate_migration()` with that same attempt ID, which captures AFTER evidence and compares it to the stored BEFORE snapshot. Target-unavailable and execution-disabled terminals do not invoke either snapshot collection path.

Snapshots retain only PID, start-time ticks, attempt ID, capture time, process/identity availability, and process affinity. The comparison verifies attempt/identity continuity and, when a CPU mask was requested, exact applied affinity. It deliberately does not inspect CPU-time progress, derive performance benefit, or assign a degradation classification.

`migration_execution_enabled` remains false by default. The production runtime still has no authoritative candidate-selection policy, so normal one-node paths terminate before execution and do not collect validation snapshots.

### Test Evidence

`make test-runtime-migration-validation` passed using a dedicated child process and the test-only upstream decision entry while retaining production manager callbacks:

| Case | Result |
|---|---|
| `RV01_EXECUTION_DEFAULT_DISABLED` | PASS |
| `RV02_REAL_CHILD_STRUCTURAL_BEFORE_AFTER` | PASS |
| `RV03_TARGET_PRETERMINAL_NO_SNAPSHOTS` | PASS |
| `RV04_EXECUTION_DISABLED_PRETERMINAL_NO_SNAPSHOTS` | PASS |

The real-child case changes only that child’s CPU affinity, verifies the attempt-bound structural BEFORE/AFTER path, then restores the child’s original affinity. It is not performance evidence. No page-placement, multi-NUMA, benefit, degradation, or application-health conclusion is supported by this integration.

## Structural Validation Completion

### Contract

`MigrationValidationSnapshot` now retains the availability and value of the live cumulative process CPU-time counter collected with each BEFORE and AFTER snapshot. The counter is retained as observation data only. Structural validation explicitly reports `progress_known=false`, `progress_observed=false`, and `benefit_known=false`; it does not infer progress from CPU time and does not add benefit, degradation, page, or target-selection policy.

`MigrationSafetyResult` carries structural-validation-known/succeeded, progress-known/observed, and benefit-known fields. `finish()` copies them to the Phase 8 terminal `FeedbackEvent`, and `feedback_log.c` persists the five fields in `migration_feedback.csv`. Terminal feedback remains non-learnable.

For a thread attempt, the runtime captures the affinity checkpoint immediately after the structural BEFORE snapshot. A structural affinity mismatch uses the production rollback callback, which calls `runtime_migration_restore_affinity()` and requires exact read-back verification. The checkpoint is released during terminal feedback finalization, including feedback-persistence failure paths.

### Runtime Test Evidence

`make test-runtime-migration-validation` passed with dedicated children and the existing test-only upstream decision entry. Production runtime callbacks remain installed; test-only modes only supply an authoritative test target or controlled failure condition.

| Case | Result |
|---|---|
| `RV01_EXECUTION_DEFAULT_DISABLED` | PASS |
| `RV02_ACTIVE_CHILD_CPU_SNAPSHOT_STRUCTURAL` | PASS |
| `RV03_PLACEMENT_MISMATCH_REAL_ROLLBACK` | PASS |
| `RV04_IDENTITY_MISMATCH_AFTER_EXEC` | PASS |
| `RV05_AFTER_CAPTURE_FAILURE` | PASS |
| `RV06_IDLE_CHILD_STRUCTURAL_NO_PROGRESS_CLAIM` | PASS |
| `RV07_TARGET_PRETERMINAL_NO_SNAPSHOTS` | PASS |
| `RV08_EXECUTION_DISABLED_PRETERMINAL_NO_SNAPSHOTS` | PASS |

The active-child case proves CPU-time snapshot retention. The idle-child case proves that an idle process still receives no progress or benefit claim. The placement-mismatch case deliberately changes only the dedicated child’s affinity after the real executor, detects the actual requested-mask mismatch, and restores the exact original affinity through the production rollback path. The active, mismatch, identity, and capture-failure cases each check exactly one matching persisted Phase 8 row by runtime-generated attempt ID; the pre-execution target terminals are covered by the earlier production persistence cases above.

### Remaining Limitations

- Structural CPU-time observation is not a progress, stall, degradation, or benefit signal.
- No comparable performance window, page-placement checkpoint, page rollback, or benefit/degradation policy exists.
- Production target selection and real cross-NUMA migration remain unavailable on this single-node host; execution remains disabled by default.

## Page Placement Checkpointing

`MigrationPageCheckpoint` now captures only an already supplied exact page-address set. It owns copied
entries bound to PID, start-time ticks, and attempt ID; validates non-null page alignment, duplicate
addresses, a 4,096-page limit, identity before/after query, and live topology node validity. It uses
non-moving `SYS_move_pages` query mode and retains each per-page kernel status. Node 0 is accepted only
when its `original_node_known` flag is true. Partial results remain incomplete and cannot make page
migration eligible.

The safety manager calls this gate for `MOVE_MEMORY` before target selection. Missing runtime addresses
return `PAGE_ADDRESS_SET_UNAVAILABLE`, persist exactly one Phase 8 terminal event, and invoke neither
target provider nor executor. Complete test-only checkpoints still stop at `PAGE_ROLLBACK_UNAVAILABLE`:
page rollback and all real page migration remain out of scope and unreachable.

The production runtime has no authoritative page-address provider. Phase 3's aggregate `numa_maps`
counts are not converted to addresses. `make test-page-checkpoint` and
`make test-runtime-page-checkpoint` passed; the latter recorded one isolated feedback row with zero
executor calls for a production-style address-absence attempt. Full detail is in
`results/page_checkpoint_validation_report.md`.

## Page Rollback Provider

`src/page_rollback.c` provides bounded best-effort restoration from immutable complete page checkpoints.
It uses `SYS_move_pages` with `MPOL_MF_MOVE`, then non-moving placement verification, and distinguishes
complete, partial, failed, unavailable, incomplete-checkpoint, identity, attempt, permission, and query
outcomes. The runtime rollback callback dispatches memory actions to this provider without changing thread
affinity rollback. Production page execution remains blocked by the missing page-candidate source, so no
real page rollback is reachable or claimed. See `results/page_rollback_validation_report.md`.

Page rollback summaries now flow through the safety manager's sole terminal `finish()` path and are
appended to Phase 8 persistence without raw addresses. This does not make page execution reachable;
the current production address-absence terminal remains unchanged.

### Authoritative Page Candidate Source Limitation

The current runtime has no trustworthy source of exact page candidates. Phase 5 memory selection uses
aggregate utility factors; Phase 3 records aggregate counters and `numa_maps` node totals only. The sole
perf integration is aggregate cache counting, not address/data-source sampling, and host perf permissions
are unavailable. `MigrationRequest.pages` is populated only by the standalone CLI's external CSV parser,
not by the production runtime. AWAVMA therefore does not turn mappings, RSS, page-fault counts, or NUMA
totals into addresses. The required future source is validated application instrumentation or a
permission-tested page-address sampling collector with explicit provenance and freshness semantics.
