# Page Rollback Validation

## Architecture

`page_rollback_restore()` consumes an immutable complete `MigrationPageCheckpoint`. It validates the
checkpoint PID/start-time/attempt binding and revalidates live identity immediately before the rollback
syscall and again before verification. It builds temporary page and node arrays only from checkpoint
entries, uses `SYS_move_pages` with the existing migration path's conservative `MPOL_MF_MOVE` flag, and
then uses non-moving query mode to verify every restored node.

The provider is best effort: syscall success is not rollback success. It returns complete only when all
pages verify at their checkpointed original nodes; otherwise it returns partial, failed, or a specific
identity, permission, verification, incomplete-checkpoint, or attempt-mismatch result. Node zero is a
valid destination only with explicit original-node validity. No raw address is persisted.

## Test Evidence

`make test-page-rollback` passed synthetic bounded adapter cases: PR01 complete, PR02 partial, PR03
failed, PR04 incomplete checkpoint, PR05 identity mismatch, PR07 attempt mismatch, PR08 node-zero
validity, PR09 verification-query failure, PR10 permission denial, and PR12 checkpoint immutability.
These validate classification and the production provider's syscall contract, not hardware NUMA movement.

`make test-runtime-page-checkpoint` still passed: the production `MOVE_MEMORY` path has no authoritative
page addresses, returns `PAGE_ADDRESS_SET_UNAVAILABLE`, and has zero target-provider, executor, and
rollback calls with exactly one terminal feedback event.

## Limits

The runtime callback dispatches `MOVE_MEMORY` to the page provider when an attempt-bound checkpoint is
retained, while preserving the established thread-affinity rollback path. Current production page
execution remains unreachable before this point because Problem #2 has no authoritative page candidates.
Therefore no real cross-node page move/restore, production rollback callback execution, or Phase 8 page
rollback-summary row was claimed. The latter remains a required integration follow-up before page rollback
can be marked fully supported.

| Capability | Status |
|---|---|
| Page rollback provider API and verification logic | SUPPORTED |
| Synthetic complete/partial/failed classification | SUPPORTED |
| Runtime dispatch integration | PARTIAL |
| Phase 8 page rollback summary persistence | NOT IMPLEMENTED |
| Real cross-NUMA page rollback | ENVIRONMENT-LIMITED |
| Production reachability | BLOCKED BY PROBLEM #2 |
| Problem #3 | PARTIALLY RESOLVED |

## Runtime Translation And Phase 8 Fields

The safety manager now supplies an attempt-local `PageRollbackSummary` output to the registered rollback
callback. `runtime_rollback_migration()` copies the real provider summary back to the manager, which
attaches it only to its centralized `finish()` feedback event. The feedback CSV appends
`page_rollback_known`, `page_rollback_attempted`, `page_rollback_result`, requested, restored, failed,
and verified counts. Addresses are not recorded. Thread rollback receives no page summary and keeps its
existing affinity path.

This preserves page detail through the runtime translation, including partial and verification failures,
but no controlled post-candidate runtime recovery entry has been added yet. The normal production
missing-address test remains the safety proof: one terminal row and zero rollback calls. Exactly-once
page-recovery finalization therefore remains partially, not fully, demonstrated.

## Runtime Post-Checkpoint Cases

The `AWAVMA_RUNTIME_TESTING` seam receives the active runtime attempt ID separately from the immutable
checkpoint-bound attempt ID. It rejects a cross-attempt checkpoint before callback invocation and routes
the detailed result through the normal `finish()` feedback path. Incomplete checkpoints follow the same
early-finalization boundary. The runtime deep-copies only a complete checkpoint whose attempt binding
matches; rejected checkpoints are not retained.

| Case | Runtime callback | Move | Query | Feedback | Rows | Result |
|---|---:|---:|---:|---:|---:|---|
| RTS01 complete | 1 | 1 | 1 | 1 | 1 | `PAGE_ROLLBACK_COMPLETE` |
| RPR06 attempt mismatch | 0 | 0 | 0 | 1 | 1 | `PAGE_ROLLBACK_ATTEMPT_MISMATCH` |
| RPR07 incomplete checkpoint | 0 | 0 | 0 | 1 | 1 | `PAGE_ROLLBACK_CHECKPOINT_INCOMPLETE` |

The normal production missing-address gate and the thread affinity rollback path remain unchanged.

### Final Runtime Page Rollback Outcome Matrix

`make test-runtime-page-rollback` passed after the final adapter and identity-gate changes. Every case
asserts one terminal-feedback call and one matching isolated Phase 8 CSV row. `finish()` is the sole
finalization path, although it has no separate test counter.

| Case | Result layer | Runtime callback | Provider | Move | Query | Feedback | Rows | Detailed result |
|---|---|---:|---:|---:|---:|---:|---:|---|
| RTS01 | provider verification | 1 | 1 | 1 | 1 | 1 | 1 | `PAGE_ROLLBACK_COMPLETE` |
| RPR02 | provider verification | 1 | 1 | 1 | 1 | 1 | 1 | `PAGE_ROLLBACK_PARTIAL` |
| RPR03 | provider verification | 1 | 1 | 1 | 1 | 1 | 1 | `PAGE_ROLLBACK_FAILED` |
| RPR04 | provider query failure | 1 | 1 | 1 | 1 | 1 | 1 | `PAGE_ROLLBACK_VERIFICATION_FAILED` |
| RPR05 | provider identity gate | 1 | 1 | 0 | 0 | 1 | 1 | `PAGE_ROLLBACK_IDENTITY_CHANGED` |
| RPR06 | test seam | 0 | 0 | 0 | 0 | 1 | 1 | `PAGE_ROLLBACK_ATTEMPT_MISMATCH` |
| RPR07 | test seam | 0 | 0 | 0 | 0 | 1 | 1 | `PAGE_ROLLBACK_CHECKPOINT_INCOMPLETE` |

The provider tests establish the persisted summary semantics: complete is 4/4 restored and verified,
partial is 3 restored/verified with 1 failed, and failed is 0 restored/verified with 4 failed. Non-attempted
identity, attempt, and incomplete outcomes retain `attempted=false`. Successful rollback remains a terminal
migration safety failure with a complete recovery result; it does not convert the migration into success.
The checkpoint is consumed read-only; PR12 and the runtime fixture's value-owned entries preserve address,
node, validity, and attempt binding. Phase 8 serializes only result and count fields, never page addresses.

## Final Status

Production `MOVE_MEMORY` still finalizes at `PAGE_ADDRESS_SET_UNAVAILABLE` before execution or rollback,
with one feedback event. Thread rollback remains the independent exact-affinity checkpoint/restore path.
Test controls are compiled only under `AWAVMA_RUNTIME_TESTING`; `nm bin/awavma-runtime` found no test seam
or adapter symbols. Page rollback is **RESOLVED**; production page-migration reachability is separately
blocked by Problem #2 and real cross-NUMA movement remains environment-limited.
