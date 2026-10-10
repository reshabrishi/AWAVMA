# CloudLab P4-C Runbook

This procedure collects P4-C evidence only. It does not enable runtime migration
authority. Run from `~/AWAVMA` on a dual-NUMA CloudLab node.

## Build And Preflight

```bash
cd ~/AWAVMA
git fetch origin
git checkout p4c1b-cloudlab-validation
git pull --ff-only origin p4c1b-cloudlab-validation
git rev-parse HEAD
git status --short --branch
hostname
uname -a
lscpu
numactl --hardware
cat /proc/sys/kernel/numa_balancing
make benchmark calibration-validate bin/p4c-migration-cost-collector
```

## Smoke

```bash
cd ~/AWAVMA
sudo --preserve-env=P4C_OUTPUT_DIR env P4C_OUTPUT_DIR="$PWD/results/cloudlab_calibration" tools/collect_numa_calibration.sh --smoke
SMOKE_RUN="$(find results/cloudlab_calibration/smoke -mindepth 1 -maxdepth 1 -type d -name 'p4c-*' -printf '%T@ %p\n' | sort -nr | head -n1 | cut -d' ' -f2-)"
printf 'SMOKE_RUN=%s\n' "$SMOKE_RUN"
cat "$SMOKE_RUN/manifest.json"
```

Smoke runs one `mixed` workload with two warmup pairs and one measured pair,
two placements per pair: six 30-second benchmark launches. Nominal benchmark
time is three minutes, plus setup, verification, and one valid warmup migration
cost attempt (with bounded retries).
The collector itself never invokes `sudo`. The privileged command above is exact:
it lets the collector read, disable, verify, and restore
`/proc/sys/kernel/numa_balancing`. Smoke succeeds and is promoted only when all
timing evidence, cost evidence, the disable/readback transaction, and verified
restoration are valid.

## Full Collection

```bash
cd ~/AWAVMA
sudo --preserve-env=P4C_OUTPUT_DIR env P4C_OUTPUT_DIR="$PWD/results/cloudlab_calibration" tools/collect_numa_calibration.sh --full
FULL_RUN="$(find results/cloudlab_calibration/full -mindepth 1 -maxdepth 1 -type d -name 'p4c-*' -printf '%T@ %p\n' | sort -nr | head -n1 | cut -d' ' -f2-)"
printf 'FULL_RUN=%s\n' "$FULL_RUN"
python3 tools/check_cloudlab_p4_results.py "$FULL_RUN"
```

Full mode runs seven workloads (`sequential`, `random`, `hot`, `moderate`,
`cold`, `mixed`, `changing`). Each has two warmup pairs and seven measured
pairs, with LOCAL and REMOTE launches for every pair: 28 warmup rows and 98
measured rows, 126 total 30-second launches. Nominal benchmark time is 63
minutes; allow roughly 75-100 minutes for first-touch, verification, cost
collection, and bounded retries. Cost evidence requires two valid warmups and
seven valid measured exact-4096-page migrations.
Full mode additionally requires the promoted smoke gate to be schema 2 with
`timing_valid`, `cost_migration_valid`, `numa_balancing_transaction_valid`, and
`overall_valid` all true and `numa_balancing_restore_status` equal to `RESTORED`.

Retained inputs are `$FULL_RUN/raw_timing.csv` and `$FULL_RUN/raw_cost.csv`.

## Build Calibration

```bash
cd ~/AWAVMA
EXPERIMENT_ID="$(basename "$FULL_RUN")"
CREATED_AT_UTC="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
CPU_ARCHITECTURE="$(uname -m)"
CPU_MODEL="$(lscpu | awk -F: '/^Model name:/ {sub(/^[[:space:]]+/, "", $2); print $2; exit}')"
ONLINE_NUMA_NODES="$(numactl --hardware | awk '/^available:/ {print $2; exit}')"
LOCAL_NODE="$(python3 - "$FULL_RUN/raw_timing.csv" <<'PY'
import csv, sys
with open(sys.argv[1], newline='') as handle:
    print(next(csv.DictReader(handle))['local_node'])
PY
)"
LOCAL_PERMITTED_CPU_COUNT="$(numactl --hardware | awk -v node="$LOCAL_NODE" '$1 == "node" && $2 == node && $3 == "cpus:" {print NF - 3; exit}')"
CALIBRATION_BUNDLE="$FULL_RUN/calibration-bundle"
ARTIFACT="$CALIBRATION_BUNDLE/numa_calibration.csv"
CALIBRATION_MANIFEST="$CALIBRATION_BUNDLE/calibration-manifest.json"

python3 tools/build_numa_calibration.py \
  --raw "$FULL_RUN/raw_timing.csv" \
  --cost "$FULL_RUN/raw_cost.csv" \
  --output "$ARTIFACT" \
  --collection-manifest "$FULL_RUN/manifest.json" \
  --calibration-manifest "$CALIBRATION_MANIFEST" \
  --experiment-id "$EXPERIMENT_ID" \
  --created-at-utc "$CREATED_AT_UTC" \
  --cpu-architecture "$CPU_ARCHITECTURE" \
  --cpu-model "$CPU_MODEL" \
  --online-numa-nodes "$ONLINE_NUMA_NODES" \
  --local-permitted-cpu-count "$LOCAL_PERMITTED_CPU_COUNT" \
  --minimum-pairs 7 \
  --minimum-cost-samples 7 \
  --safety-margin-pct 1
```

## Strict Validation

```bash
cd ~/AWAVMA
bin/calibration-validate "$ARTIFACT"
```

The expected success message is `CALIBRATION_VALIDATED records=7`. The helper
`tools/check_cloudlab_p4_results.py` is diagnostic only and does not replace this
strict validator.

## P5-C.2B Matrix B Locality Validation

This collection is behavioral validation only. It records
`registered_memory_load_rate` in `ops/ms` and does not assume an ordering among
`cold`, `moderate`, and `hot`. A default 54-run collection is authoritative as
descriptive locality evidence; classification authority remains `UNAVAILABLE`
and runtime authority remains `DISABLED`. Smoke or any protocol override is
non-authoritative.
Each replicate requires worker affinity, P2 page registration, C1 evidence
continuity, and verified start/end placement. Invalid replicates remain in the
diagnostic artifact but are excluded from statistics; each context requires at
least 12 of 14 expected worker-run units. A NUMA-balancing restoration failure
stops collection immediately. The launch intentionally omits `--memory-node`.

Build and verify the 54-launch plan without changing NUMA balancing:

```bash
cd ~/AWAVMA
make benchmark p5-c2b-locality-collector test-p5-c2b-locality
ID="p5-c2b-$(date -u +%Y%m%dT%H%M%SZ)"
bin/p5-c2b-locality-collector --matrix-b-v1 --dry-run --id "$ID"
```

The dry run prints `configurations=54 authoritative=1`. On a one-node host, exit status `3` and
`status=ENV_LIMITED` are expected; no output directory or sysctl mutation is
performed.

Run the 12-launch non-authoritative smoke matrix:

The smoke preset uses two workers, 64 MiB, one warmup, one measured run,
5 seconds per run, and a 1-second startup discard.

```bash
cd ~/AWAVMA
SMOKE_ID="p5-c2b-smoke-$(date -u +%Y%m%dT%H%M%SZ)"
sudo bin/p5-c2b-locality-collector --matrix-b-v1 --execute --smoke --id "$SMOKE_ID" --output-root "$PWD/results"
python3 tools/check_p5_locality_validation.py "$PWD/results/p5_locality_validation/$SMOKE_ID"
```

Run the fixed full matrix (three patterns, intensity 100, LOCAL and REMOTE, two
warmups plus seven measured runs per cell, two workers, 256 MiB, 20 seconds,
2-second startup discard, seed 12345):

```bash
cd ~/AWAVMA
FULL_ID="p5-c2b-$(date -u +%Y%m%dT%H%M%SZ)"
sudo bin/p5-c2b-locality-collector --matrix-b-v1 --execute --id "$FULL_ID" --output-root "$PWD/results"
make check-p5-c2b-locality DIR="$PWD/results/p5_locality_validation/$FULL_ID"
```

Retain exactly the separate C2B artifacts under
`results/p5_locality_validation/$FULL_ID`: `raw_intervals.csv`,
`worker_run_summaries.csv`, `locality_manifest.csv`,
`locality_validation_runs.csv`, and per-run placement evidence. The strict
checker validates schema, hardware/topology consistency, all 54 diagnostics,
deterministic base-plus-run seeds, K=5 non-overlapping windows, 14 worker-run
median replicates per pattern and placement, the >=12 validity gate, and
min/max/mean/median/population-standard-deviation/p10/p90 arithmetic. Percentiles
use linear interpolation at `p * (n - 1)`. These statistics are descriptive;
the checker does not test or impose pattern order.

After the checker succeeds, commit and push the immutable Matrix B evidence:

```bash
cd ~/AWAVMA
git add "results/p5_locality_validation/$FULL_ID"
git diff --cached --check
git commit -m "Add CloudLab Matrix B locality evidence"
git push origin p4c1b-cloudlab-validation
```
