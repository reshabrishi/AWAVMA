# CloudLab Multi-NUMA Phase 4D Evaluation

## Required Environment

Use an exclusive Linux machine with at least two visible NUMA nodes, `numactl`, Python 3, a C11 compiler, and NUMA development headers. Do not publish host names, IP addresses, MAC addresses, credentials, or raw address fields.

## Exact Order

The required order is **metadata -> build -> tests -> environment check -> preflight -> calibration -> manifest IN_PROGRESS -> baseline -> AWAVMA -> aggregate -> graphs -> manifest PASS**. The runner discovers the first two observed NUMA node IDs, records topology, platform, compiler, locale, Git revision/status, and environment-check output, then uses the existing Makefile targets. The final PASS manifest is published only after aggregation and applicable graphs complete.

## Commands

### Fresh Node Setup

On a fresh CloudLab node, install dependencies and build the existing runtime binaries once:

```bash
cd ~/AWAVMA-validation
./scripts/setup_cloudlab_node.sh
```

Use `--skip-install` when packages are already present, or `--check-only` to validate an existing setup without installing or rebuilding. A new CloudLab node does not require repeating project phases: this script only installs dependencies and builds existing phase/runtime binaries. Calibration and experiment collection remain separate, explicit steps.

1. Validate the allocated environment: `scripts/run_full_experiment.sh --check-only`.
2. Build and run the required project checks: `scripts/run_full_experiment.sh --tests-only`.
3. Collect only default, local, and remote-memory baselines: `scripts/run_full_experiment.sh --baseline-only --output-dir DIR`.
4. Collect only the AWAVMA scenario: `scripts/run_full_experiment.sh --awavma-only --output-dir DIR --calibration FILE --migration-cost-artifact FILE`.
5. Run the complete collection: `scripts/run_full_experiment.sh --output-dir DIR --calibration FILE --migration-cost-artifact FILE`.
6. Aggregate without figures: add `--skip-graphs`.

The public Phase 4D interface is exactly `--check-only`, `--tests-only`, `--baseline-only`, `--awavma-only`, `--skip-graphs`, `--output-dir DIR`, `--calibration FILE`, `--migration-cost-artifact FILE`, and `--thread-evaluation-horizon-seconds SECONDS`. AWAVMA collection requires the benefit calibration and migration-cost artifact for the selected directed remote-to-local route, plus a positive active-work ROI horizon; the horizon defaults to 10 seconds. The runner rejects an artifact whose schema, validation status, or directed route does not match the AWAVMA placement. Partial collection modes aggregate their measurements and intentionally skip four-scenario graphs. Exit status `3` means the allocation cannot support collection; `1` means a measured workload failed.

## Layout And Outputs

Each collection is isolated at `DIR/raw/RUN_ID/` with `metadata/`, `baseline/logs/`, `awavma/logs/`, `awavma/runtime/`, `measurements.csv`, and `manifest.json`. Only CSV files beneath a passing run-local manifest with `data_source=REAL` and `collection_status=PASS` are ingested. The aggregator rejects address-named fields and IP/MAC values.

`DIR/unified/cloudlab_experiment_results.csv` is the canonical wide CSV. `cloudlab_comparator.csv` contains only complete four-scenario comparisons. `summaries/elapsed_seconds_summary.csv`, `summaries/throughput_summary.csv`, and `summaries/latency_ms_summary.csv` provide mean, median, and sample standard deviation. Graph generation consumes the comparator and its matching summary, skips each unavailable named graph independently, and fails when none can be generated.

The per-run graph report is `DIR/graphs/graph_summary.csv`; it records each named graph as `GENERATED` or `SKIPPED` with its precise input and reason.
