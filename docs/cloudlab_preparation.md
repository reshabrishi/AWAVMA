# CloudLab Multi-NUMA Phase 4D Evaluation

## Required Environment

Use an exclusive Linux machine with at least two visible NUMA nodes, `numactl`, Python 3, a C11 compiler, and NUMA development headers. Do not publish host names, IP addresses, MAC addresses, credentials, or raw address fields.

## Exact Order

The required order is **metadata -> build -> tests -> environment check -> preflight -> baseline -> AWAVMA -> aggregate -> graphs -> manifests**. The benchmark selects the lowest permitted local node and deterministic farthest permitted remote node, temporarily binds first-touch only, restores default memory policy, and publishes aggregate placement evidence. The runner records topology, platform, compiler, locale, Git revision/status, and environment-check output without imposing inherited NUMA binding.

## Commands

1. Validate the allocated environment: `scripts/run_full_experiment.sh --check-only`.
2. Build and run the required project checks: `scripts/run_full_experiment.sh --tests-only`.
3. Collect only default, local, and remote-memory baselines: `scripts/run_full_experiment.sh --baseline-only --output-dir DIR`.
4. Collect only the AWAVMA scenario: `scripts/run_full_experiment.sh --awavma-only --calibration FILE --output-dir DIR`.
5. Run the complete collection: `scripts/run_full_experiment.sh --calibration FILE --output-dir DIR`.
6. Aggregate without figures: add `--skip-graphs`.

The public Phase 4D interface is `--check-only`, `--tests-only`, `--baseline-only`, `--awavma-only`, `--skip-graphs`, `--output-dir DIR`, and `--calibration FILE`. Calibration is required only when AWAVMA is collected. Exit status `3` means the allocation cannot support collection; `1` means a measured workload failed.

## Layout And Outputs

Each collection is isolated at `DIR/raw/RUN_ID/` with `metadata/`, `baseline/logs/`, `awavma/logs/`, `awavma/runtime/`, `measurements.csv`, and `manifest.json`. Before any measured run, AWAVMA collection validates the supplied regular readable calibration file with `bin/calibration-validate`, copies it byte-for-byte to `metadata/calibration.csv`, and records its SHA-256 without recording the source path. The runner verifies that the supplied file still matches the validated metadata copy before each AWAVMA repetition, then passes the exact CLI-supplied path through `--calibration-artifact` alongside `--production-real-migration -S -M -R`. The run is accepted only after the runtime writes `runtime_execution_profile.csv` with `ACTIVE` effective production mode, all three effective mechanisms, and the strict production capability profile. A missing, changed, or invalid calibration, or a missing, malformed, inactive, or environment-limited profile, fails collection rather than bypassing the loader or being reported as monitoring-only. Baselines start no runtime and remain calibration- and migration-free. Only CSV files beneath a passing run-local manifest with `data_source=REAL` and `collection_status=PASS` are ingested. The aggregator rejects address-named fields and IP/MAC values.

LOCAL, REMOTE, and AWAVMA also require a passing benchmark-owned `placement_evidence.csv` equivalent artifact. REMOTE and AWAVMA must have identical aggregate initial placement evidence; AWAVMA additionally requires P2 registration ACK. Placement establishes an initial state only and neither authorizes nor executes migration.

`DIR/unified/cloudlab_experiment_results.csv` is the canonical wide CSV. `cloudlab_comparator.csv` contains only complete four-scenario comparisons. `summaries/elapsed_seconds_summary.csv`, `summaries/throughput_summary.csv`, and `summaries/latency_ms_summary.csv` provide mean, median, and sample standard deviation. Graph generation consumes the comparator and its matching summary, skips each unavailable named graph independently, and fails when none can be generated.

The per-run graph report is `DIR/graphs/graph_summary.csv`; it records each named graph as `GENERATED` or `SKIPPED` with its precise input and reason.
