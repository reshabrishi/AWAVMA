# CloudLab Preparation Test Plan

CloudLab collection tooling is implemented by `scripts/run_full_experiment.sh`,
`scripts/aggregate_experiment_results.py`, and
`scripts/generate_multinuma_graphs.py`. Its focused standard-library tests are
`cloudlab_phase4d_tooling_test.py` and `cloudlab_phase4d_aggregation_test.py`.
The following cases remain required coverage for that shipped interface.

## Tooling Schema Validation

- Accept a complete collection record with required run identity, topology,
  workload, timestamp, metric, unit, and provenance fields.
- Reject missing required fields, empty identity values, malformed timestamps,
  non-finite numeric metrics, and unsupported units.
- Reject a record whose reported NUMA-node count or node identifiers conflict
  with the captured topology.
- Preserve `UNKNOWN` or unavailable values as unavailable; never coerce them
  to zero, node 0, or a measured metric.
- Require controlled runs to be explicitly marked as controlled and prohibit
  their aggregation with real CloudLab measurements.
- Reject duplicate records sharing the complete run and observation identity
  unless the schema defines an explicit deterministic replacement rule.

## Graph Input Validation

- Validate CSV headers before graph generation and report every missing
  required column.
- Reject rows with malformed numeric fields, `NaN`, infinity, incomplete join
  identifiers, or inconsistent measurement status.
- Verify graph joins use complete explicit identifiers rather than row order.
- Verify unavailable values produce `NO_DATA` rather than zero-valued points
  or fabricated comparisons.
- Verify generated graph metadata retains the source path and distinguishes
  real CloudLab data from controlled test data.

## Raw Address Persistence

Raw virtual or physical addresses are unsupported for persistence in CloudLab
collection records, graph inputs, summaries, logs, and test fixtures. Tests
for future tooling must assert that no persisted schema field accepts an
address value. If page-level evidence is needed, retain only validated,
non-address aggregate metadata with explicit provenance and availability.
