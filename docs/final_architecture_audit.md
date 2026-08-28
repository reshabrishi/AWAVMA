# AWAVMA Final Architecture Audit

## 1. Existing Architecture

Phases 3-8 are separate C modules and command-line programs connected mainly
through CSV files. Phase 3 collects samples for one PID. Phase 4 reads a
monitoring CSV and classifies observations. Phase 5 reads classification rows,
creates application contexts, and writes decisions plus persistent state.
Phases 6-8 consume the preceding CSV outputs and enforce validation,
migration, and feedback boundaries respectively.

Runtime-related source already exists in `src/application_manager.c` and
`src/worker_pool.c`. The Makefile builds it as `bin/application_manager`, but
the manager currently schedules only Phase 3 monitoring; it does not invoke the
Phase 4-8 programs.

## 2. Phase 3 Monitoring Interface

- **Executable:** `bin/monitor`, built from `src/monitor.c` and
  `src/monitor_main.c`.
- **Important functions:** `monitor_run_pid()` in `src/monitor.c`; the CLI
  installs `SIGINT`/`SIGTERM` handlers and calls it from `monitor_main.c`.
- **PID handling:** `monitor_config_t.pid` is a required positive `pid_t`.
  `monitor_run_pid()` verifies `/proc/<pid>` and reads process, thread, memory,
  NUMA, and perf data for that PID.
- **Existing process:** `--pid PID` monitors an already-running process. The
  monitor does not launch or discover it in this mode.
- **Launcher mode:** `--benchmark` forks and execs the benchmark, then passes
  the child PID to the same `monitor_run_pid()` function.
- **Continuous lifecycle:** the function loops at `interval_ms` and writes
  process and per-thread samples until the target exits or `stop_requested` is
  set. It also stops when the target becomes a zombie/dead process.
- **Termination:** `SIGINT` or `SIGTERM` sets the stop flag. An existing target
  is not killed by Phase 3. In benchmark launcher mode, the CLI may send
  `SIGTERM` to its child after monitoring stops.
- **Automatic discovery:** **NO.** `/proc` scanning is not part of Phase 3;
  it is implemented separately by `application_manager_discover_once()`.

Phase 3 identifies output rows by PID and has no `app_id` field in
`monitor_config_t` or its CSV header. Each concurrent invocation must use
separate output, thread-output, and log paths.

## 3. Phase 4 Interface

- **Input:** `classifier_config_t` supplies an input monitoring/workload CSV,
  output CSV, access column, entity column, rolling window, decay and
  thresholds. The executable defaults to
  `results/monitoring_results.csv` and `results/classification_results.csv`.
- **Output:** rows contain timestamp, elapsed time, PID, `entity_id`, score,
  previous/current class, parameters, and status.
- **Application identity:** Phase 4 has no `app_id` configuration or output
  field. It uses `entity_id`, defaulting to `workload`, and requires one
  consistent PID for the complete input (`expected_pid` in `classifier_run()`).
  Multiple entities can be represented, but multiple application PIDs cannot
  be processed by one classifier run.
- **Important function:** `classifier_run()` in `src/classifier.c`.

## 4. Phase 5 Interface

- **Input:** `decision_config_t` supplies the Phase 4 CSV, output path, state
  directory, history directory, log path, optional `app_id`, and decision
  parameters. `decision_run()` accepts an `app_id` column when present and
  otherwise requires `config->app_id`.
- **Output:** decision CSV rows contain `app_id`, PID, `entity_id`, factors,
  scores, biases, versions, decision, and status. It also updates
  `weights.csv`, `biases.csv`, `application_state.csv`, decision history, and
  weight history.
- **Application-specific state:** yes. `decision_run()` maintains one local
  `application_context_t` per distinct `app_id`, including weights, biases,
  last PID, entities, class counts, and versions. Persistent records are also
  keyed by `app_id`; missing application records are initialized from global
  defaults.
- **Phase 3 to Phase 4 to Phase 5 identity flow:** there is no automatic
  in-process flow. Phase 3 writes PID-only monitoring CSV; Phase 4 writes PID
  and entity rows without `app_id`; Phase 5 gets `app_id` only from a supplied
  input column or its `--app-id` fallback. The runtime must therefore provide
  the per-application file boundary and pass the manager-generated ID to Phase
  5 and later phases.
- **Multiple applications:** Phase 5 can represent multiple applications in
  one input because it creates contexts by `app_id` and reports
  `summary->applications`. This does not make the preceding classifier or the
  shared file pipeline multi-application safe.
- **Thread safety:** **NO for shared runtime state.** Context arrays are local
  to one `decision_run()` call, but persistent state and history files are
  shared and have no mutex. Atomic `rename()` prevents partial replacement,
  not lost updates from concurrent load-modify-save operations. The public
  `decision_feedback_apply()` function has the same unprotected shared-file
  update problem.

## 5. Phase 6 Interface

- **Input:** `validation_main.c` reads the Phase 5 decision CSV and maps each
  row into `MonitorData`, `ClassifierData`, and `DecisionData`. `app_id` and
  `entity_id` are read from the row, with `--app-id` as the fallback.
- **Output:** `ValidationResult` contains migration ID, `app_id`, PID, entity,
  gate scores/statuses, validation score, status, and final decision. The
  logger writes configured results, history, and human log files.
- **Important functions:** `Validation_Init()`, `ValidateMigration()`, the
  three gate functions, `CalculateValidationScore()`, and
  `LogValidationResult()`.
- **Authorization boundary:** Phase 6 only approves or rejects. It does not
  execute migration. Phase 7 independently requires
  `phase6_validation.final_decision == "APPROVED"` before executing an action.
- **Thread safety:** gate calculations that receive all state explicitly are
  suitable as read-only calculations. The module lifecycle and logger are not
  thread-safe: `active_config`, `initialized`, `log_config`, and `log_count`
  are static process-wide state, and result/history append and compaction have
  no shared lock.

## 6. Phase 7 Interface

- **Input:** `MigrationRequest` combines the Phase 5 `DecisionData`, Phase 6
  `ValidationResult`, PID/TID, NUMA nodes, destination CPU, page metadata,
  placement requirements, and memory lock/pin flags. The CLI reads these from
  the Phase 6 CSV and supports an `--app-id` fallback.
- **Output:** `MigrationReport` records app/entity identity, action, target,
  requested/attempted/migrated/failed pages, affinity, verification, result,
  error, and execution time. Results, history, state, and human log files are
  written by the migration logger.
- **Migration boundary:** `Migration_Execute()` rejects invalid actions,
  requires Phase 6 approval, validates the target and requested placement,
  then performs the selected thread-affinity or memory migration. No Phase 7
  execution bypasses the Phase 6 approval check.
- **Locking:** a process-wide pthread mutex protects fixed-size execution-lock
  and cooldown tables. The key is PID/TID and, for process requests, entity ID.
  The lock prevents conflicting migrations and cooldown violations, but it does
  not protect migration result/history/state file writes or module lifecycle.
  The tables are limited to 64 execution locks and 64 cooldown entries.
- **Thread safety:** execution locking is thread-safe for the protected
  migration-conflict state. `Migration_Init()`, `Migration_Shutdown()`, and
  migration logging use process-wide mutable state without a logger mutex, so
  concurrent calls sharing the configured files are not generally safe.

## 7. Phase 8 Interface

- **Input:** `FeedbackEvent` carries feedback/migration/app/entity IDs, PID,
  Phase 6 result, migration result, before/after metrics and sample counts,
  confidence, factor signals, migration cost, page results, and verification.
  The CLI reads these from `feedback_inputs.csv`; `--app-id` is a fallback.
- **Output:** `FeedbackResult` reports reward, confidence, relevance, feedback
  class/status, processing time, applied factor/bias update, and a decision
  state snapshot. Feedback history, result, and human log files are persisted.
- **Persistent state:** `ProcessFeedback()` calls
  `decision_feedback_apply()` to update Phase 5 per-application weights and
  biases in `state/weights.csv` and `state/biases.csv`; it also updates shared
  feedback history/results.
- **Application isolation:** updates are selected by `event->app_id`, so the
  logical state is application-specific. The physical files are shared by all
  applications unless the runtime supplies separate state/history paths.
  Feedback refuses to learn for an application marked `EXPIRED` in the shared
  Phase 5 application state.
- **Thread safety:** **NO.** `active_config`, `initialized`, and `event_count`
  are static process-wide state. Feedback logging appends and periodically
  compacts shared files without a mutex, and the delegated Phase 5 state update
  has no cross-thread or cross-process serialization.

## 8. Current Multi-Application Capability

**PARTIAL**

The current system can discover and register multiple applications in
`application_manager.c`; identity is generated as `APP_<pid>_<start_time>` and
the manager stores a bounded application table. Its worker pool can run
separate Phase 3 monitor jobs, and each job uses per-application monitoring,
thread, and log paths.

The standalone `bin/monitor` handles one PID per invocation. Phase 4 also
rejects a PID change within one input, and the normal Phase 3-8 defaults are
single shared CSV paths. Phase 5 supports multiple `app_id` contexts, but
Phase 3 and Phase 4 do not automatically supply that identity, while Phase 5
and Phase 8 shared persistence is not thread-safe. The manager's
`run_monitor_job()` currently stops after Phase 3; it does not invoke the
classifier, decision, validation, migration, or feedback interfaces.

Therefore App A, App B, and App C can be represented in the runtime registry
and monitored concurrently, but they cannot currently run as isolated,
continuously scheduled Phase 3-8 pipelines without an orchestration adapter
and a state/file concurrency boundary.

## 9. Missing Runtime Layer

These components already exist as partial runtime code, but are not connected
to the complete Phase 3-8 pipeline:

- **Application Discovery:** present in `application_manager_discover_once()`;
  it scans `/proc`, filters processes, and registers `(pid,start_time_ticks)`.
  It is not part of Phase 3 itself.
- **Application Manager:** present in `application_manager.c` and built by the
  Makefile. It tracks applications and schedules monitor jobs, but does not
  orchestrate Phases 4-8 or propagate `app_id` downstream.
- **Worker Pool:** present in `worker_pool.c`, with pthread workers and a
  bounded queue. It currently receives monitor jobs only.
- **Continuous Scheduling:** present for discovery in
  `application_manager_run()` and for one monitor job per application. The
  missing capability is continuous per-application scheduling of the complete
  Phase 3-8 pipeline and feedback cycle.

The actual missing runtime integration is therefore the adapter/job path after
`run_monitor_job()`, plus serialization or isolation for shared Phase 5/8
state. No Phase 3-9 algorithm needs to be redesigned for this connection.

## 10. Recommended Connection Point

The existing manager and worker pool are the connection point before
`monitor_run_pid()`. A per-application job should retain the manager's stable
identity and route isolated CSV paths and `--app-id` fallbacks through the
existing phase interfaces:

```text
Application Discovery
        ↓
Application Manager
        ↓
Worker Pool
        ↓
Existing Phase 3
        ↓
Phase 4
        ↓
Phase 5
        ↓
Phase 6
        ↓
Phase 7
        ↓
Phase 8
        ↓
Phase 9
```

The manager's current enqueue site is `application_manager.c:363`, and the
current Phase 3 handoff is `run_monitor_job()` at `application_manager.c:205`
through `monitor_run_pid()` at `application_manager.c:226`. The downstream
handoff must preserve the same `app_id` and use per-application result paths;
shared Phase 5/8 writers must be serialized if shared state directories remain
in use.

## 11. Minimal Implementation Plan

1. Use the existing discovery and application registry to create one stable
   job identity per `(pid,start_time_ticks)`.
2. Extend the existing monitor job adapter to run Phase 4-8 with per-
   application input/output paths and the manager-generated `app_id` fallback.
3. Use the existing bounded worker pool for independent monitoring jobs while
   enforcing one active job per application.
4. Serialize shared Phase 5/8 state and history writers, or assign isolated
   state/history directories per application.
5. Add concurrent App A/App B/App C integration coverage, then run the existing
   Phase 2-9 regression checks.

## 12. Risks

- Phase 3 can lose the target between discovery, monitor startup, and a sample
  read; `/proc` disappearance is treated as termination.
- PID reuse is possible unless the manager's start-time identity is retained
  and rechecked; Phase 3 itself accepts only PID.
- Phase 4 rejects mixed PIDs in one input and does not carry `app_id` forward.
- Shared Phase 5 weights, biases, application state, histories, results, and
  logs have no general runtime mutex; atomic rename alone does not prevent lost
  updates.
- Phase 6 and Phase 8 process-wide configuration, initialization, and counters
  are not safe for concurrent lifecycle or logging calls.
- Phase 7's execution lock protects migration conflicts but not its shared
  result/history/state logging files; its in-memory lock tables are bounded.
- A worker can block in continuous `monitor_run_pid()` until the target exits
  or the shared stop flag is set, reducing available pool capacity.
- The runtime Makefile target does not link `src/monitor.c`; the manager uses a
  weak monitor symbol and otherwise forks the external `bin/monitor`, so the
  executable must be available for the fallback path.
