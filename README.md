# AWAVMA Phase 2 Benchmark through Phase 8 Feedback Learning

## Purpose

This program is the Phase 2 workload generator for **Adaptive Workload-Aware Virtual Memory Allocation for Multi-Core Architectures (AWAVMA)**. It creates controlled pthread memory workloads and records basic baseline execution information for later analysis.

Phase 8 adds bounded, application-specific adaptation from observed migration outcomes. Phase 7 still only executes an already-selected and validated migration request.

Phase 3 adds an observational monitoring module. It samples a selected Linux process periodically and writes time-series data for later phases. Phase 3 does not classify workloads or make any optimization decision.

Phase 4 adds a deterministic analysis module using explicit access observations, decay, rolling windows, and hysteresis. Phase 4 does not perform migration or optimization.

Phase 5 adds an explainable Decision Engine. It selects `MOVE_MEMORY`, `MOVE_THREAD`, or `NO_MIGRATION` from explicit normalized factors. It selects an action only; it never executes migration.
Phase 6 adds a validation-only safety gate for Phase 5 requests. It evaluates confidence, ROI, and safety, records an auditable result, and never executes migration.
Phase 7 executes only migration requests selected by Phase 5 and approved by Phase 6. It never makes a new migration decision.
Phase 9 reads Phase 2-8 CSV outputs and generates validated research SVG graphs. It does not change any prior-phase state or behavior.

## Scope

Implemented in this phase:

- Configurable pthread worker count, memory size, operation count, and duration.
- Sequential, random, hot, moderate, cold, mixed, local, remote, and changing-locality patterns.
- Deterministic pseudo-random generation with a configurable seed.
- Optional libnuma memory allocation and pthread CPU affinity.
- Human-readable console results and optional append-only CSV output.
- Configurable Phase 3 monitoring and Phase 4 classification binaries.
- Configurable Phase 5 Decision Engine with bounded per-application state and history.
- Phase 6 Validation Module with confidence, ROI, safety, bounded validation history, and explicit rejection reasons.
- Phase 7 Migration Module with authorization guards, controlled thread affinity migration, guarded page migration support, verification, state, logging, and bounded history.
- Phase 9 Graph Generation with real-versus-controlled data labeling, unavailable-value handling, deterministic SVG output, graph validation, and generation summaries.

## Dependencies

Ubuntu packages:

```bash
sudo apt update
sudo apt install build-essential libnuma-dev numactl
```

`libnuma-dev` supplies the header and linker development files. `numactl` is useful for inspecting hardware and independently checking placement.

## Compilation

```bash
make
make benchmark
make monitor
make classifier
make decision
make validation
make test-validation
make migration
make test-migration
make clean
```

The executables are `bin/benchmark`, `bin/monitor`, `bin/classifier`, `bin/decision`, `bin/validation`, `bin/migration`, and the Phase 3 test-only target `bin/monitor_test_target`. `bin/migration` uses Linux affinity and syscall interfaces and does not add a NUMA development-header dependency. The existing targets retain their `-pthread -lnuma` behavior.

## Phase 3 Monitoring

The architecture is:

```text
Phase 2 benchmark -> Phase 3 monitor -> Phase 4 classifier -> Phase 5 decision engine -> Phase 6 validation -> Phase 7 migration -> Phase 8 feedback
```

The monitor observes only. It does not change CPU affinity, NUMA policy, memory placement, pages, threads, or benchmark arguments. It does not implement hot/moderate/cold classification, thresholds, migration, decisions, feedback, or machine learning.

### Monitor An Existing PID

```bash
bin/monitor --pid 12345 --interval 100 \
  --output results/monitoring_results.csv \
  --thread-output results/monitoring_threads.csv \
  --log logs/monitor.log
```

`--interval` is in milliseconds and defaults to 100. The monitor validates the PID, samples until the process exits, and handles zombie/terminated targets without leaving a child process behind.

### Launch Phase 2 Through The Monitor

Monitor options must appear before `--benchmark`; all later arguments are passed directly to `bin/benchmark` using `execv()` without shell interpolation.

```bash
bin/monitor --interval 100 \
  --output results/monitoring_results.csv \
  --thread-output results/monitoring_threads.csv \
  --benchmark \
  --threads 4 --memory 512 --duration 2 --pattern hot \
  --output results/benchmark_results.csv
```

Use `--benchmark-path PATH` to launch a different benchmark executable.

### Collected Metrics

The process CSV records system CPU utilization, process CPU utilization, RSS, VMS, RSS delta, cumulative and interval page faults, cache references/misses when permitted, thread count, configured NUMA node count, `/proc/<pid>/numa_maps` page summaries, and benchmark metadata in launcher mode.

The process CPU percentage is calculated from deltas in `/proc/<pid>/stat` and `/proc/stat`. It is normalized to one logical CPU, so a multithreaded process can legitimately exceed 100% (for example, approximately 200% for two fully utilized CPUs). System CPU is normalized to 0-100% across the machine.

The per-thread CSV records TID, current CPU, observed CPU NUMA node, state, and delta-based CPU utilization. The monitor never sets affinity.

### CSV Schemas

`results/monitoring_results.csv`:

```text
timestamp,elapsed_ms,pid,system_cpu_utilization_percent,process_cpu_utilization_percent,rss_kb,vms_kb,rss_delta_kb,minor_page_faults,major_page_faults,interval_minor_faults,interval_major_faults,cache_references,cache_misses,thread_count,numa_nodes,process_numa_pages,benchmark_pattern,benchmark_threads,benchmark_memory_mb,benchmark_iterations,benchmark_duration_sec
```

`results/monitoring_threads.csv`:

```text
timestamp,elapsed_ms,pid,tid,cpu,cpu_node,cpu_utilization_percent,state
```

Unavailable metrics are recorded as `-1`, never as zero. Hardware cache counters use `perf_event_open()` and are reported unavailable when PMU permissions or hardware support prevent collection. Exact per-page memory access frequency is not fabricated from `/proc` data.

### Metric Sources

| Metric | Source |
|---|---|
| System CPU | `/proc/stat` deltas |
| Process/thread CPU | `/proc/<pid>/stat` and `/proc/<pid>/task/<tid>/stat` deltas |
| RSS/VMS | `/proc/<pid>/status` |
| Page faults | `/proc/<pid>/stat` |
| Cache references/misses | `perf_event_open()` when available |
| Thread count/CPU/state | `/proc/<pid>/task/` |
| NUMA node/page summary | libnuma and `/proc/<pid>/numa_maps` |

### Standalone Monitor Target

`bin/monitor_test_target` is only a controlled Phase 3 test fixture. It is not part of AWAVMA optimization and does not make placement decisions.

```bash
bin/monitor_test_target --threads 1 --memory-mb 1 --duration 3 &
bin/monitor --pid $! --interval 100
```

## Phase 4 Workload Classifier

Phase 4 classifies an explicit access-observation signal into `HOT`, `MODERATE`, or `COLD`. The classifier is workload/entity-level when an `entity_id` column is supplied. It does not invent page IDs. If `entity_id` is absent, the entity is named `workload`.

The score is exactly:

```text
Score = sum(access_value_i * exp(-lambda * age_i))
```

`age_i` is the current observation time minus the observation time in seconds, and `lambda` is a configured decay factor per second. The default rolling window is 10 observations. Older observations are removed when the configured window is full.

Default initial configuration values are `lambda=0.1`, `hot-threshold=100`, `moderate-threshold=20`, and `hysteresis=5`. These are explicit starting parameters, not learned or empirically optimal thresholds. HOT requires entering at `hot-threshold + hysteresis`; HOT is retained down to `hot-threshold - hysteresis`. MODERATE uses the corresponding upper/lower boundaries. COLD uses the lower state boundary.

Run the classifier:

```bash
bin/classifier --input results/monitoring_results.csv \
  --output results/classification_results.csv
```

The input must contain `timestamp`, `elapsed_ms`, and `pid`. A defensible access signal must be supplied in `access_value` or another column selected with `--access-column`. `entity_id` is optional and selected with `--entity-column`. The classifier validates the header, numeric fields, PID consistency, and chronological elapsed time.

The output schema is:

```text
timestamp,elapsed_ms,pid,entity_id,score,previous_class,current_class,lambda,window_size,hot_threshold,moderate_threshold,hysteresis,status
```

When the access column is absent or contains `-1`, output rows are marked `UNAVAILABLE_ACCESS_SIGNAL` and score `-1`. The current Phase 3 output has no access-frequency column; its CPU, RSS, page-fault, NUMA-residency, and unavailable cache fields are not used as access-frequency substitutes. Therefore current Phase 3 integration cannot claim real page-level HOT/MODERATE/COLD classification.

Synthetic classifier fixtures are under `tests/classifier_inputs/`. They test high, moderate, low, decay, rolling-window, hysteresis, and recent-activity behavior. Their `access_value` values are algorithm test inputs, not measurements from Phase 3.

## Phase 5 Decision Engine

Phase 5 operates on an application/workload context. It consumes classifier output and optional explicit normalized decision factors. It does not reimplement monitoring or classification and does not fabricate page IDs.

Run it on an enriched decision input:

```bash
bin/decision --input tests/decision_inputs/memory_wins.csv \
  --output results/decision_results.csv \
  --app-id APP_001
```

For actual Phase 4 output, provide an explicit application context:

```bash
bin/decision --input results/classification_results.csv \
  --app-id APP_HOT \
  --output results/decision_results.csv
```

Current Phase 4 output has no normalized decision factors and marks classification unavailable, so this produces `INSUFFICIENT_DECISION_SIGNAL`. It does not substitute CPU, RSS, faults, NUMA residency, benchmark pattern names, or unavailable cache counters.

### Decision Factors

The enriched input uses normalized `[0,1]` factors:

```text
f_access
f_threshold
f_gain_memory, f_cost_memory, f_cpu_memory, f_sharing_memory
f_gain_thread, f_cost_thread, f_cpu_thread, f_sharing_thread
```

The engine applies twelve individual weights:

```text
Memory: w_M_access, w_M_threshold, w_M_gain, w_M_cost, w_M_cpu, w_M_sharing
Thread: w_T_access, w_T_threshold, w_T_gain, w_T_cost, w_T_cpu, w_T_sharing
```

The formulas are:

```text
MemoryScore = w_M_access*f_access + w_M_threshold*f_threshold
            + w_M_gain*f_gain_memory - w_M_cost*f_cost_memory
            - w_M_cpu*f_cpu_memory - w_M_sharing*f_sharing_memory

ThreadScore = w_T_access*f_access + w_T_threshold*f_threshold
            + w_T_gain*f_gain_thread - w_T_cost*f_cost_thread
            - w_T_cpu*f_cpu_thread - w_T_sharing*f_sharing_thread
```

Biases are bounded by default to `[-0.25, 0.25]` and are added after the raw scores. The default epsilon is `0.05`.

```text
both final scores <= 0                  -> NO_MIGRATION
memory > thread + epsilon               -> MOVE_MEMORY
thread > memory + epsilon               -> MOVE_THREAD
otherwise                               -> NO_MIGRATION
```

Migration is never compulsory. Missing factors produce `INSUFFICIENT_DECISION_SIGNAL`, not a guessed decision.

### State and History

Current state is stored per application in `state/application_state.csv`, `state/weights.csv`, and `state/biases.csv`. Bounded history is stored in `history/decisions.csv` and `history/weight_history.csv`.

`history_max_records`, `history_max_days`, `history_decay_lambda`, and `cleanup_interval` are configurable. Compaction writes a temporary file, flushes and closes it, then atomically renames it. Historical relevance is recorded as `exp(-history_decay_lambda * age_days)`; it does not update weights or biases in Phase 5.

Application records are marked `ACTIVE` when seen, `INACTIVE` after `--inactive-days`, and `EXPIRED` after `--expire-days`. Active state is never removed by history cleanup.

Manual initial configuration is supported with `--weight MEMORY,GAIN,2.0`, `--memory-bias`, and `--thread-bias`. These values are bounded and are not learned from outcomes.

The decision output records factor values, raw scores, biases, final scores, epsilon, selected action/status, weight version, and bias version for explainability.

## Phase 6 Validation Module

The validation module consumes an enriched decision CSV. It performs no migration and has no migration-system calls. The default configuration is `config/awavma.conf`.

```bash
bin/validation --input tests/validation_inputs/validation_fixtures.csv
```

For the real Phase 5 output, unavailable factors are rejected explicitly:

```bash
bin/validation --input results/decision_results.csv \
  --output results/validation_results.csv \
  --history history/validation_history.csv \
  --log logs/validation.log
```

The confidence gate is:

```text
Sconfidence = w_stability*stability
            + w_samples*min(sample_count/sample_reference_count, 1)*100
            + w_classifier*classifier_confidence
```

The ROI gate preserves the raw signed result:

```text
SROI = w_gain*predicted_gain - w_cost*estimated_cost
```

The safety gate uses CPU availability (`100 - cpu_utilization`), memory availability (`100 - memory_utilization`), and concurrency. Hard vetoes always reject before the soft score is considered. The final score is:

```text
Svalidation = w_confidence*Sconfidence + w_roi*SROI + w_safety*Ssafety
```

`NO_MIGRATION` is recorded as `VALID_NO_MIGRATION`. Phase 5 `INSUFFICIENT_DECISION_SIGNAL` is recorded as `REJECT_INSUFFICIENT_SIGNAL`. Other failures include `REJECT_LOW_CONFIDENCE`, `REJECT_LOW_ROI`, `REJECT_UNSAFE`, and the specific hard-veto reason.

Validation results are written to `results/validation_results.csv`, bounded historical records to `history/validation_history.csv`, and human-readable events to `logs/validation.log`. History compaction removes records older than the configured age, bounds record count, recalculates `exp(-history_decay_lambda * age_days)`, and atomically renames a flushed temporary file.

The standalone fixtures under `tests/validation_inputs/` cover approval, low ROI, a hard safety veto, no migration, and insufficient signal. Run them with `make test-validation`.

## Phase 7 Migration Module

Phase 7 executes only migration requests selected by Phase 5 and approved by Phase 6. Authorization is strict:

```text
Phase 5 MOVE_MEMORY + Phase 6 APPROVED -> memory execution path
Phase 5 MOVE_THREAD + Phase 6 APPROVED -> thread execution path
anything else                         -> no migration
```

`NO_MIGRATION` becomes `MIGRATION_NO_ACTION`; `INSUFFICIENT_DECISION_SIGNAL` becomes `MIGRATION_INSUFFICIENT_INFORMATION`; rejected approved-action requests become `MIGRATION_NOT_AUTHORIZED`.

### Thread Migration

Thread requests require both PID and TID. Phase 7 verifies that the process exists, the TID exists, and the TID belongs to the PID before using `sched_setaffinity()`. It records the old, requested, and new affinity and only reports `MIGRATION_SUCCESS` after post-operation verification. Invalid CPUs, exited targets, PID/TID mismatches, permission failures, duplicate locks, and cooldown violations are reported explicitly.

Controlled thread tests use a dedicated child process and CPUs 0-7. They do not modify the terminal, OpenCode, desktop processes, or unrelated system services. A same-NUMA CPU movement is CPU-affinity migration, not remote NUMA migration.

### Memory Migration

Memory requests require valid source and destination NUMA nodes, verified page/region metadata, and caller-supplied page addresses. Phase 7 never scans arbitrary process memory, invents page addresses, or migrates unknown/locked/pinned regions. When compiled with `SYS_move_pages`, it queries source ownership, requests `MPOL_MF_MOVE`, counts attempted/migrated/failed pages, and verifies destination placement. Missing page metadata returns `MIGRATION_INSUFFICIENT_INFORMATION`.

The current host has one NUMA node (`node 0`) and CPUs 0-7. Remote NUMA migration cannot be demonstrated here. A same-node memory request returns `MIGRATION_NO_MIGRATION_REQUIRED` unless explicit placement is requested. The implementation does not claim remote migration or NUMA speedup on this host.

### State, Logs, and History

Migration results are appended to `results/migration_results.csv`; human-readable events go to `logs/migration.log`; current state is replaced atomically in `state/migration_state.csv`; bounded history is stored in `history/migration_history.csv`. Configuration keys are `migration_history_max_days`, `migration_history_max_records`, `migration_history_decay_lambda`, `migration_cleanup_interval`, `migration_cooldown_ms`, `migration_lock_timeout_ms`, and `migration_verification_enabled`.

History compaction writes a temporary file, flushes and closes it, then atomically renames it. If compaction fails, the original history remains intact.

### Migration Tests

`make test-migration` runs the controlled test harness and CLI fixtures. It covers authorization, CPU 0 to 1 and CPU 1 to 2 affinity movement, invalid CPUs, lifecycle races, PID/TID validation, duplicate locks, same-node behavior, missing/invalid page metadata, logging, history age/count retention, atomic cleanup failure handling, controlled pipeline execution, one-node NUMA reporting, and execution overhead. Remote NUMA and real locked/pinned page tests are reported as environment-limited when they cannot be safely reproduced.

## Phase 8 Feedback Learning

Phase 8 evaluates a migration only when Phase 5 selected a migration, Phase 6 approved it, and Phase 7 returned a learnable execution result. It compares supplied before/after throughput, execution time, latency, and page-fault measurements using direction-aware normalized improvements. Unavailable `-1` values are excluded. The weighted reward is clamped to `[-1,1]` and includes a bounded migration-cost penalty when a valid cost reference exists.

The update is interpretable and application-specific. For each participating Phase 5 factor, `delta_i = learning_rate * reward * confidence * relevance * factor_signal_i`. The selected action's memory or thread bias uses the corresponding bias learning rate and the same effective reward. All values are finite and clamped to the Phase 5-compatible bounds; no global application state is updated by another application.

Confidence uses sample coverage, comparable-metric coverage, supplied measurement confidence when present, and migration verification evidence. Historical relevance is `exp(-feedback_decay_lambda * age_days)`. Deadband and minimum-confidence gates prevent small or weak observations from changing state. Feedback results are written to `results/feedback_results.csv`, explainable bounded history to `history/feedback_history.csv`, and human-readable events to `logs/feedback.log` with atomic state/history replacement.

Controlled fixtures under `tests/feedback_inputs/` are explicitly labeled test inputs, not real system performance measurements. Phase 8 updates future decision behavior from observed migration outcomes. It does not directly perform migration. Limitations include the current Phase 3-4 pipeline's lack of normalized decision factors and the host's lack of remote NUMA evidence, so a real pipeline run can correctly produce no learning.

## Phase 9 Graph Generation

Run `make graphs` or `python3 scripts/generate_graphs.py`. The generator reads actual Phase 2-8 CSV outputs, state, and history without changing them. It writes dependency-free SVG graphs under `graphs/phase2/` through `graphs/phase8/` and `graphs/combined/`, plus `results/graph_generation_summary.csv` and `logs/graph_generation.log`.

Graphs include benchmark pattern/throughput, monitoring CPU/RSS/page faults/NUMA residency, classification states, decision distributions and biases, feedback reward/confidence/relevance, all 12 factor weights, and controlled APP_A versus APP_B adaptation. Each graph records its source, `REAL` or `CONTROLLED_TEST` data type, records available/used/skipped, status, output path, and generation time. `REAL` and `CONTROLLED_TEST` data are never silently combined.

Unavailable values such as `-1`, NaN, and infinity are excluded rather than converted to zero. Cross-phase joins require complete identifier matches; row order is never used as a join key. Missing validation or migration result files produce explicit `NO_DATA` summary entries; unsupported comparisons are marked `SKIPPED`. SVG files are checked for existence, nonzero size, valid dimensions, parseability, and absence of non-finite numeric tokens. The preferred pandas/numpy/matplotlib backend is not required by this implementation; the current host uses the standard-library SVG backend. Phase 9 visualizes measurements only and does not perform Phase 10 performance comparison.

## Multi-Application Performance

Run `make multi-application-performance` to measure the integrated foreground runtime with 1, 2, 4, and 8 simultaneous benchmark applications and 1, 2, and 4 Phase 3 workers. The driver uses the production 250 ms discovery cadence, a fixed calibrated iteration count, three warm-ups and five measured repetitions per mode/configuration, and records timestamped sequential launch skew because the frozen benchmark has no external pre-execution barrier.

`bin/awavma-runtime --pid PID` may be repeated. Each supplied PID is resolved at startup to `(pid,start_time_ticks)`, repeated PIDs are deduplicated, and later PID reuse is not accepted as the original target. The performance driver runs one isolated runtime for all selected benchmark identities; it does not broadly schedule unrelated host processes.

Results are written to `results/multi_application_performance_raw.csv`, `results/multi_application_performance_summary.csv`, `results/multi_application_scaling.csv`, `results/multi_application_fairness.csv`, and `results/multi_application_status_counts.csv`. Graphs are written to `graphs/multi-application-performance/`. These results measure AWAVMA runtime overhead and scalability on the current single-node host, not comparative NUMA allocation performance. Queue-full events are recorded separately from ordinary FIFO waiting and generic submission failures.

## Phase 4-6 Pipeline Profiling

Run `make phase46-pipeline-profile` for the 1/2/4/8-application, two-worker serialized pipeline profile. It records parent artifact preparation, normalization/delta/adaptation/result parsing, fork-to-reap wall time, and isolated child API timing under `AWAVMA_PROFILE`. Outputs include `results/phase46_pipeline_profile_raw.csv`, `results/phase46_pipeline_profile_summary.csv`, `results/phase46_pipeline_profile_concurrency_audit.csv`, and `graphs/phase46-pipeline/`.

The coordinator remains serialized. Phase 4 has an explicit experimental `--phase4-mode in-process` adapter, but production defaults to `subprocess`: the measured candidate removed Phase 4 launch cost but regressed complete multi-application pipeline wall time and aggregate throughput. Phase 5 and Phase 6 remain subprocesses; in particular, Phase 6 has process-global initialization/logger state and is not an in-process per-application concurrency candidate without a dedicated context refactor.

## Usage

```text
bin/benchmark [options]
  --threads N              Worker thread count
  --memory MB              Allocated memory in MiB
  --iterations N           Operations per worker thread
  --duration SEC           Duration mode; takes precedence over iterations
  --pattern NAME           Workload pattern
  --hot-percent N          Hot region percentage
  --moderate-percent N     Moderate region percentage
  --cold-percent N         Cold region percentage
  --change-phases N        Number of phases for changing (2-16)
  --seed N                 Reproducible pseudo-random seed
  --thread-node N          Bind workers to NUMA node N
  --memory-node N          Allocate memory on NUMA node N
  --numa-node N            Set both placement nodes to N
  --output FILE            Append a CSV result row to FILE
```

Defaults are 1 thread, 64 MiB, 1,000,000 iterations per thread, sequential access, seed `12345`, and hot/moderate/cold region sizes of 10/30/60 percent. Percentages must sum to 100. Duration mode checks the deadline in batches, so its operation count is approximate.

## Patterns

- `sequential`: each worker streams through its assigned slice.
- `random`: each worker uses a deterministic pseudo-random sequence within its slice.
- `hot`: 99% of accesses target the configured hot region and 1% samples the full allocation.
- `moderate`: 70% target the moderate region and 30% samples the full allocation.
- `cold`: 10% target the small hot subset and 90% sample the full allocation, leaving most pages relatively cold.
- `mixed`: 70% hot, 25% moderate, and 5% cold-region accesses.
- `changing`: execution is divided into phases; each phase concentrates accesses in a different allocation segment.
- `local`: sequential access with workers and memory placed on the same NUMA node.
- `remote`: sequential access with workers and memory placed on different NUMA nodes.

The hot, moderate, and cold percentages describe the physical regions used by the frequency-based patterns. Reads are used in worker loops to avoid data races and synchronization overhead while still forcing memory loads through volatile accesses.

## Examples

```bash
bin/benchmark --threads 4 --memory 512 --iterations 1000000 --pattern sequential
bin/benchmark --threads 8 --memory 4096 --iterations 5000000 --pattern random
bin/benchmark --threads 8 --memory 4096 --iterations 5000000 --pattern hot
bin/benchmark --threads 8 --memory 4096 --iterations 5000000 --pattern mixed
bin/benchmark --threads 8 --memory 4096 --iterations 5000000 --pattern changing --output results/benchmark_results.csv
```

Local and remote placement examples:

```bash
bin/benchmark --threads 8 --memory 4096 --iterations 5000000 --pattern local --thread-node 0 --memory-node 0
bin/benchmark --threads 8 --memory 4096 --iterations 5000000 --pattern remote --thread-node 0 --memory-node 1
```

Run the remote command only when node 1 exists.

## NUMA Configuration

The benchmark calls libnuma to detect available nodes. Explicit placement uses `numa_alloc_onnode()` with strict allocation behavior and binds each worker to a CPU belonging to the requested thread node. The console reports each worker's CPU and observed CPU NUMA node. If fewer than two nodes exist, `remote` exits with a clear error instead of falling back to local access.

Normal allocation does not request NUMA placement. Explicit NUMA allocation avoids relying on accidental Linux first-touch placement; the initial page touch occurs after the node-aware allocation request. Actual placement can still be affected by OS permissions, container restrictions, or memory pressure, so placement should be independently inspected on the target system.

Inspect the machine before NUMA tests:

```bash
lscpu
numactl --hardware
```

## Output and CSV

Console output includes the selected configuration, NUMA nodes, seed, total operations, execution time, throughput, checksum, and per-thread timing/CPU information. `--output FILE` appends:

```text
timestamp,pattern,threads,memory_mb,iterations,duration_sec,thread_node,memory_node,numa_nodes,operations,execution_time_sec,throughput_ops_sec
```

`iterations` is the requested per-thread count. In duration mode, `duration_sec` is the requested duration and `operations` is the measured total.

## Validation Tests

The intended Phase 2 test matrix is:

```bash
bin/benchmark --threads 1 --memory 16 --iterations 10000 --pattern sequential --output results/benchmark_results.csv
bin/benchmark --threads 4 --memory 32 --iterations 10000 --pattern sequential --output results/benchmark_results.csv
bin/benchmark --threads 4 --memory 32 --iterations 10000 --pattern random --output results/benchmark_results.csv
bin/benchmark --threads 4 --memory 32 --iterations 10000 --pattern hot --output results/benchmark_results.csv
bin/benchmark --threads 4 --memory 32 --iterations 10000 --pattern moderate --output results/benchmark_results.csv
bin/benchmark --threads 4 --memory 32 --iterations 10000 --pattern cold --output results/benchmark_results.csv
bin/benchmark --threads 4 --memory 32 --iterations 10000 --pattern mixed --output results/benchmark_results.csv
bin/benchmark --threads 4 --memory 32 --iterations 10000 --pattern local --thread-node 0 --memory-node 0 --output results/benchmark_results.csv
bin/benchmark --threads 4 --memory 32 --iterations 10000 --pattern changing --output results/benchmark_results.csv
```

The remote test is conditional on at least two available NUMA nodes:

```bash
bin/benchmark --threads 4 --memory 32 --iterations 10000 --pattern remote --thread-node 0 --memory-node 1 --output results/benchmark_results.csv
```

## Known Limitations

- Remote NUMA behavior requires at least two available NUMA nodes and appropriate OS/container permissions.
- Duration mode checks time once per 1,024 operations, so it is not a real-time scheduler.
- Memory placement is requested through libnuma but is not a replacement for kernel-level page-placement tracing.
- The benchmark intentionally has no monitoring, classification, migration, or feedback functionality; those remain separate phase modules.
- The current system denies `perf_event_open()` cache counters, so cache references and misses are reported as `-1`.
- Monitoring process CPU utilization intentionally reports multi-core capacity percentage and may exceed 100%.
- Phase 3 does not provide exact memory-access frequency or page-level hotness; those require later instrumentation/classification work.

## Phase 3 Validation

Standalone tests use `monitor_test_target` before the AWAVMA benchmark: CPU workload, 64 MiB memory workload, multithreaded workload, CSV/header and timestamp checks, unavailable-cache-counter handling, and target termination detection. Integration tests launch sequential, random, hot, mixed, changing, and local Phase 2 workloads through `bin/monitor`; each produces separate monitoring and benchmark CSV files. Sampling intervals of 100, 500, and 1000 ms are supported and can be tested with `--interval`.

The current machine reports one NUMA node and CPUs 0-7. Local placement and NUMA observation can be tested. Remote workload/monitoring validation is not performed because a second NUMA node is unavailable.

Phase 3 is strictly observational. Phase 4 performs classification analysis only. Phase 5 selects an action but does not execute migration. Phase 6 validates, Phase 7 executes migration, Phase 8 learns from measured outcomes, and Phase 9 visualizes outputs. Phase 10 performance comparison remains unimplemented.

## Phase 4 Validation

Example standalone runs:

```bash
bin/classifier --input tests/classifier_inputs/high.csv --output /tmp/class_high.csv
bin/classifier --input tests/classifier_inputs/moderate.csv --output /tmp/class_moderate.csv
bin/classifier --input tests/classifier_inputs/cold.csv --output /tmp/class_cold.csv
bin/classifier --input tests/classifier_inputs/hysteresis.csv --window 1 --output /tmp/class_hysteresis.csv
bin/classifier --input tests/classifier_inputs/old_high_recent_low.csv --lambda 1.0 --output /tmp/class_decay.csv
bin/classifier --input tests/classifier_inputs/rolling_window.csv --window 5 --output /tmp/class_window.csv
```

The same classifier can process actual Phase 3 files, but with the current schema it reports `UNAVAILABLE_ACCESS_SIGNAL`. This is intentional: Phase 3 does not expose page-level or explicit access-frequency observations. No benchmark pattern label is used as classification evidence.

## Phase 5 Validation

Synthetic decision fixtures are under `tests/decision_inputs/`. They cover memory wins, thread wins, both-negative scores, epsilon ties, unavailable factors, bias changes, weight changes, application isolation, history retention, and deterministic output. Synthetic factor values are algorithm validation inputs, not real migration measurements.

The current Phase 3/4 pipeline can be passed to Phase 5, but because it lacks normalized decision factors and valid access classification, the resulting status is `INSUFFICIENT_DECISION_SIGNAL`. No migration action is executed in any case.
