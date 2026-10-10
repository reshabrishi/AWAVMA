# CloudLab Multi-NUMA Phase 4D Evaluation

## Required Environment

Use an exclusive Linux machine with at least two visible NUMA nodes, `numactl`, Python 3, a C11 compiler, and NUMA development headers. Do not publish host names, IP addresses, MAC addresses, credentials, or raw address fields.

## Exact Order

The required order is **metadata -> build -> tests -> environment check -> preflight -> baseline -> AWAVMA -> aggregate -> graphs -> manifests**. The benchmark selects the lowest permitted local node and deterministic farthest permitted remote node, temporarily binds first-touch only, restores default memory policy, and publishes aggregate placement evidence. The runner records topology, platform, compiler, locale, Git revision/status, and environment-check output without imposing inherited NUMA binding.

## Commands

1. Validate the allocated environment: `scripts/run_full_experiment.sh --check-only`.
2. Build and run the required project checks: `scripts/run_full_experiment.sh --tests-only`.
3. Collect only default, local, and remote-memory baselines: `scripts/run_full_experiment.sh --baseline-only --output-dir DIR`.
4. Collect only the AWAVMA scenario: `scripts/run_full_experiment.sh --awavma-only --reference-run PASSING_BASELINE_RUN --calibration FILE --output-dir DIR`.
5. Run the complete collection: `scripts/run_full_experiment.sh --calibration FILE --output-dir DIR`.
6. Aggregate without figures: add `--skip-graphs`.
7. Select a calibrated access pattern with `--workload sequential|random|hot|moderate|cold|mixed|changing` (default `mixed`). Placement-only benchmark patterns are rejected.

The public Phase 4D interface also includes `--reference-run DIR`. AWAVMA-only collection requires an explicit passing baseline reference; aggregation is current-run-only and never searches or pools other runs. Calibration and its adjacent production `manifest.json` are required whenever AWAVMA is collected. Exit status `3` means the allocation cannot support collection; `1` means a measured workload failed.

## Layout And Outputs

Each collection is isolated at `DIR/raw/RUN_ID/` with `metadata/`, `baseline/logs/`, `awavma/logs/`, `awavma/runtime/`, `measurements.csv`, and `manifest.json`. Before any measured run, AWAVMA collection validates the supplied regular readable calibration file with `bin/calibration-validate`, copies it byte-for-byte to `metadata/calibration.csv`, and records its SHA-256 without recording the source path. The runner verifies that the supplied file still matches the validated metadata copy before each AWAVMA repetition, then passes the exact CLI-supplied path through `--calibration-artifact` alongside `--production-real-migration -S -M -R`. Runtime sockets live in a private, short, per-runtime directory rather than the artifact root; the runtime publishes both actual paths in `runtime_execution_profile.csv`, and the benchmark launcher consumes those fields before replacing itself with the benchmark. The runtime removes the sockets and directory at shutdown while all persistent artifacts remain under `awavma/runtime/`. The run is accepted only after the runtime writes `runtime_execution_profile.csv` with `ACTIVE` effective production mode, all three effective mechanisms, and the strict production capability profile. A missing, changed, or invalid calibration, or a missing, malformed, inactive, or environment-limited profile, fails collection rather than bypassing the loader or being reported as monitoring-only. Baselines start no runtime and remain calibration- and migration-free. Only CSV files beneath a passing run-local manifest with `data_source=REAL` and `collection_status=PASS` are ingested. The aggregator rejects address-named fields and IP/MAC values.

LOCAL, REMOTE, and AWAVMA require passing benchmark-owned initial placement artifacts named `*.start.csv`. AWAVMA is compared only with REMOTE initial placement from the same run or explicit reference. Final placement status is recorded separately and is never substituted with initial evidence. Registration, benchmark, runtime, initial-placement, and final-placement outcomes are distinct fields. Placement establishes an initial state only and neither authorizes nor executes migration. The runner transactionally disables automatic NUMA balancing for collection, verifies the disabled state, and restores and verifies the original value on every exit path.

`DIR/unified/cloudlab_experiment_results.csv` is the canonical explicit-run CSV. `cloudlab_comparator.csv` records the primary AWAVMA-vs-REMOTE comparison and secondary LOCAL/default comparisons for operations, benchmark execution time, throughput, and precisely derived mean wall time per operation (`execution_time_seconds / operations`). Summaries provide mean, median, sample standard deviation, minimum, maximum, and two-sided 95% Student t confidence intervals for samples of 2 through 30. Failed or incomplete runs are rejected. Baseline-only runs aggregate their three scenarios and publish a reusable PASS reference manifest without requiring AWAVMA or calibration. Full runs require all four scenarios, and AWAVMA-only runs fail closed unless their explicit reference matches protocol, topology, NUMA distance, NUMA-balancing policy, workload configuration, and available provenance. The final PASS manifest is written only after aggregation, graph generation, and NUMA-balancing restoration succeed.

The per-run graph report is `DIR/graphs/graph_summary.csv`; it records each named graph as `GENERATED` or `SKIPPED` with its precise input and reason.

The runner copies and hashes both the calibration CSV and production manifest, passes both run-local copies to the runtime, and supplies the same explicit pattern, threads, memory bytes, and duration contract used by the benchmark. The provisional and final manifests and every measurement retain that workload identity. Aggregation rejects disagreement among those records, benchmark actual output, runtime execution profiles, or an explicit baseline reference.

Generate a strict row-preserving decision report with `python3 tools/report_beneficial_calibration.py --calibration FILE --production-manifest MANIFEST --output REPORT.csv`. Rows are sorted by conservative ROI and classified as `BENEFICIAL`, `NOT_BENEFICIAL`, or `INVALID`. Zero effective benefit is a successful `NOT_BENEFICIAL` result; malformed inputs and calibration/manifest provenance mismatch fail the command.

This runner contract must not be read as live migration authority. The checked P4 artifact has negative conservative ROI after its safety margin and an adjacent manifest whose `NOT_PRODUCTION_CALIBRATION` status contradicts the CSV production labels, so it fails closed. The runtime empirical memory bridge remains unavailable, `MOVE_THREAD` lacks production thread calibration, and comparable post-migration observations required for feedback learning are unavailable.
