# P5-C.2A Thread-Activity Calibration

P5-C.2A-1/C2A-2 are isolated offline foundations. C2A-2 persists and verifies
offline artifacts; it neither collects CloudLab evidence, changes C1 evidence
identity, nor enables runtime classification authority.

The metric is `registered_memory_load_rate` v1 in `ops_per_ms`:
`load_operations_delta / interval_ms`. It counts registered-memory benchmark
load operations, not bytes, cache misses, bandwidth, latency, or CPU use.

## Controlled Matrix A

Calibration profiles are controlled load intensities `LOW`, `MID`, and `HIGH`.
They are not runtime classes: only a valid calibration maps their bands to
`COLD`, `MODERATE`, and `HOT`. Matrix A v1 uses the declared 10/40/100
intensity percentages; locality patterns are not classes in this model. The
library does not require those exact values: a context is structurally valid
when its LOW, MID, and HIGH percentages are strictly ordered in 1..100. The
versioned `intensity_profile_version` and all three percentages still match
exactly between calibration and requested context.

Raw C1 interval rows carry `WARMUP_RUN`, `WARMUP_INTERVAL`, or
`MEASURED_INTERVAL`. Only consecutive valid measured rows form a window. A
window contains exactly five same-run, same-worker, same-profile, same-placement
intervals with increasing consecutive `sample_index`; windows do not overlap.
Interval scratch storage is distinct from dynamically sized completed
window-median storage, so a later window cannot overwrite an earlier median.
Incomplete trailing groups are ignored. Invalid, warmup, or gapped intervals
discard the partial group and begin a new sequence only at a later valid
measured interval. The median of all completed window medians is one
worker-run median, which is the sole final statistical unit. At least 12 valid
worker-run units are required for every profile.

Statistics use worker-run medians: min, max, mean, median, population standard
deviation, p10, and p90. Percentiles linearly interpolate sorted values at
`p * (n - 1)`. Calibration is valid only if `low.p90 < mid.p10` and
`mid.p90 < high.p10`. Valid inclusive bands are low/COLD, mid/MODERATE, and
high/HOT; gaps remain unavailable. Classification accepts only a calibrated
five-interval window median, never a profile label or raw interval.

Gap ratios, symmetric paired-worker relative differences, and leave-one-run-out
band coverage are report-only robustness metrics. They do not impose extra
validity thresholds. A structurally valid build with fewer than 12 worker-run
units returns successfully with `INSUFFICIENT_SAMPLES`; malformed input and
invalid context return an error.

## Context And Rejection

Context includes hardware/metric identity, placement, workload shape,
`intensity_profile_version`, the three percentages, NUMA balancing state,
window size, and minimum unit count. A requested context must exactly match
these fields, including placement and NUMA balancing. The builder rejects bad
schema/IDs/profiles/lifecycle values, non-finite or inconsistent rates, zero
intervals, mismatched metric/workload/placement, and duplicate raw identities
`(calibration_id, run_index, worker_index, sample_index)`.

## Artifact Contract

For a valid identifier, artifacts have frozen paths beneath
`ROOT/p5_thread_activity_calibration/ID/`: `raw_intervals.csv`,
`worker_run_summaries.csv`, and `calibration_manifest.csv`. All three are CSV
with exact headers and unquoted single-line cells. Profiles are `LOW`, `MID`,
and `HIGH`; sample kinds are `WARMUP_RUN`, `WARMUP_INTERVAL`, and
`MEASURED_INTERVAL`; placement is `LOCAL` or `REMOTE`; calibration statuses are
their named text values, never numeric enum encodings.

The manifest has exactly one data row. It carries the complete context, file
identity/counts/status, and every per-profile report field: worker/window
counts, all statistics including p10/p90, gap-ratio availability/value,
paired-worker agreement, and leave-one-run-out totals, in-band count, and
fraction. The loader strictly parses all three CSVs, validates every text enum
and numeric field, rebuilds raw evidence through the core, and requires exact
canonical summary and manifest output. Persisted reports are verified evidence,
not independent authority.

The writer creates all three completed files in a unique staging directory and
renames that directory only after every write succeeds. It refuses an existing
final ID directory rather than overwriting or repairing it, preventing partial
artifact corruption.

Controlled collection and authoritative CloudLab calibration are C2A-3/C2A-4
work. Runtime classification remains off.
