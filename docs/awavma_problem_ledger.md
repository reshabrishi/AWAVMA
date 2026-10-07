# AWAVMA Problem Ledger

| ID | Phase | Title | Status | CloudLab required | Commit | Evidence path | Remaining risk |
| --- | --- | --- | --- | --- | --- | --- | --- |
| B1A-001 | B1A | Persistent temporal history integration | LOCAL_PASS | YES | pending | `apps/<app>/history/thread_confidence.csv` | No canonical worker candidate TID exists; B1A rows cannot establish thread confidence. |

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
