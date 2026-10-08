# AWAVMA Problem Ledger

| ID | Phase | Title | Status | CloudLab required | Commit | Evidence path | Remaining risk |
| --- | --- | --- | --- | --- | --- | --- | --- |
| B1A-001 | B1A | Persistent temporal history integration | LOCAL_PASS | YES | da608e7 | `apps/<app>/history/thread_confidence.csv` | No canonical worker candidate TID exists; B1A rows cannot establish thread confidence. |
| B2-001 | B2 | Real worker-TID selection and provenance | LOCAL_PASS | YES | pending | `apps/<app>/history/thread_confidence.csv` | Candidate evidence is observational only; no confidence or migration authority exists. |
| B1B-001 | B1B | Candidate-bound thread-confidence streak | LOCAL_PASS | YES | pending | `apps/<app>/thread_confidence_state.csv` | LIVE_RUNTIME_PASS_CAPABILITY=NO_REAL_QUALIFYING_EVIDENCE; numeric TID incarnation protection remains partial. |
| P5-001 | P5 | Genuine beneficial opportunity | LOCAL_PASS | YES | pending | `apps/<app>/p5_opportunity_state.csv` | Live candidate-bound classification, memory relation, and workload-equivalent gain evidence are unavailable, so live state fails closed. |
| P5B-001 | P5-B | Real candidate-bound placement/gain/cost evidence | LOCAL_PASS | YES | pending | `apps/<app>/p5_opportunity_state.csv` | Classification and measured activity remain unavailable, so P5 remains fail-closed. |

## B1A-001

- Description: Persist app/process-incarnation temporal observations without changing monitor, Phase 5, Phase 6, migration, or frozen P1-P4 behavior.
- Root cause: Current runtime has persistent monitor sessions but no durable per-cycle temporal history; old history semantics had weak deduplication and no true generation adjacency.
- Severity: high.
- Blocked by: canonical candidate-TID selection for future candidate-bound confidence only.
- Affected files: `src/awavma_runtime.c`, `src/thread_confidence_history.c`, `include/thread_confidence_history.h`.
- Proposed fix: app-owned, atomic, process-cycle history with a distinct temporal generation and explicit unavailable candidate status.
- Implemented fix: app-owned atomic process-cycle history with a separate temporal generation; every B1A row records candidate TID unavailable.
- Tests: thread-confidence-history unit test, runtime history integration, monitor/runtime regression suite.
- Remaining risks: history is intentionally candidate-neutral; it must not be connected to decision, ROI, safety, or migration until a real worker candidate exists.
- CloudLab validation attempt at `daaa754`: failed. The reported dedicated target compiled a misspelled history-test source, and `test-awavma-runtime` reported AR02-AR09 failures. The checked-out commit already uses the correct source spelling; the repair centralizes that source list, removes the target during `clean`, and makes AR01-AR09 report their independent assertions for the required second CloudLab diagnosis.
- CloudLab root cause: runtime discovery found 649 processes, then reconciled all of them against the runtime manager capacity of 256 before runtime target admission. Manager `ENOSPC` was mapped to monitor `EIO`; no filesystem capacity issue existed. Runtime admission now filters discovery records before manager reconciliation, so `max_applications` means admitted AWAVMA-managed targets rather than arbitrary host processes. CloudLab AR02 admission-order validation: `CLOUDLAB_PASS` at `f823e79`. Overall B1A remains `LOCAL_PASS` pending CloudLab execution of the runtime-admission target; its Makefile link contract was missing `libnuma`.

## B2-001

- Root cause: current runtime had no canonical verified worker TID before Phase 7.
- Old implementation reference: `origin/fix-persistent-monitor-session`.
- Chosen integration: current-native `/proc/<pid>/task` enumeration with process identity, task existence, task-stat, and per-thread affinity verification.
- Authority boundary: candidate provenance reaches B1A only; Phase 5/6/7 and `MigrationRequest.tid` remain unchanged.

## B1B-001

- Root cause: verified candidate TIDs and persistent temporal history existed, but no strict candidate-specific adjacent-generation confidence evaluator existed.
- Old reference: `origin/fix-persistent-monitor-session`.
- Chosen design: current-native exact-identity, generation-adjacent, duplicate-safe, row-order-independent, fail-closed, observational evaluator.
- Identity: `(app_id, pid, start_time_ticks, candidate_tid)` over `temporal_generation`.
- Required streak: `THREAD_CONFIDENCE_REQUIRED_STREAK=3`.
- Evidence boundary: `CANDIDATE_VERIFIED` with `observation_valid=false` is insufficient and cannot pass. `LIVE_RUNTIME_PASS_CAPABILITY=NO_REAL_QUALIFYING_EVIDENCE`.
- Authority boundary: derived state is not consumed by Phase 5/6/7, ROI, safety, migration selection, affinity, or memory migration.
- Residual risk: schema v1 does not persist candidate task start ticks. TID incarnation protection remains partial; a future additive schema may add `candidate_tid_start_time_ticks`.

## P5-001

- Root cause: the runtime has verified candidates, temporal confidence infrastructure, and calibrated migration cost, but still lacks an end-to-end real opportunity determination using genuine candidate-bound evidence.
- Chosen design: current-native fail-closed real-evidence opportunity evaluator.
- Status after local pass: LOCAL_PASS.
- Evidence boundary: the evaluator requires candidate-bound classification, a measured REMOTE candidate-memory relation, a distinct permitted analysis destination, a strict P4 calibration match, and the existing decision margin. Missing factors remain unavailable and cannot become zero or positive by default.
- Authority boundary: P5 writes only the atomic current-state artifact. It does not update B1B history, invoke `sched_setaffinity` or `move_pages`, mutate `MigrationRequest`, modify P4 artifacts, or authorize Phase 6/7 execution.

## P5B-001

- Root cause: P5 evaluator is correct, but live runtime lacked real candidate-bound placement, destination, gain, and cost evidence.
- Chosen design: read-only current-native evidence plumbing with exact identity and strict unit-compatible ROI semantics.
- Implemented evidence: verified task CPU from `/proc/<pid>/task/<tid>/stat`, task affinity from `sched_getaffinity(tid)`, CPU/node topology, and read-only `move_pages(..., nodes=NULL)` queries restricted to P2-authenticated owned registrations.
- Unit contract: P5 schema v2 names gain and cost in milliseconds. Gain remains unavailable without measured relevant memory activity; P4 conservative cost is exposed in milliseconds only for an exact frozen calibration match.
- Authority boundary: no affinity changes, page moves, memory-policy changes, migration request mutation, B1B qualification changes, or P4 artifact writes occur.
