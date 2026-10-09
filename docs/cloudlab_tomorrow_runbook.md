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
P4C_OUTPUT_DIR="$PWD/results/cloudlab_calibration" tools/collect_numa_calibration.sh --smoke
SMOKE_RUN="$(find results/cloudlab_calibration/smoke -mindepth 1 -maxdepth 1 -type d -name 'p4c-*' -printf '%T@ %p\n' | sort -nr | head -n1 | cut -d' ' -f2-)"
printf 'SMOKE_RUN=%s\n' "$SMOKE_RUN"
cat "$SMOKE_RUN/manifest.json"
```

Smoke runs one `mixed` workload with two warmup pairs and one measured pair,
two placements per pair: six 30-second benchmark launches. Nominal benchmark
time is three minutes, plus setup, verification, and one valid warmup migration
cost attempt (with bounded retries).

## Full Collection

```bash
cd ~/AWAVMA
P4C_OUTPUT_DIR="$PWD/results/cloudlab_calibration" tools/collect_numa_calibration.sh --full
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
ARTIFACT="$FULL_RUN/numa_calibration.csv"

python3 tools/build_numa_calibration.py \
  --raw "$FULL_RUN/raw_timing.csv" \
  --cost "$FULL_RUN/raw_cost.csv" \
  --output "$ARTIFACT" \
  --experiment-id "$EXPERIMENT_ID" \
  --created-at-utc "$CREATED_AT_UTC" \
  --cpu-architecture "$CPU_ARCHITECTURE" \
  --cpu-model "$CPU_MODEL" \
  --online-numa-nodes "$ONLINE_NUMA_NODES" \
  --local-permitted-cpu-count "$LOCAL_PERMITTED_CPU_COUNT" \
  --minimum-pairs 7 \
  --minimum-cost-samples 7 \
  --safety-margin-pct 1 \
  --production
```

## Strict Validation

```bash
cd ~/AWAVMA
bin/calibration-validate "$ARTIFACT"
```

The expected success message is `CALIBRATION_VALIDATED records=7`. The helper
`tools/check_cloudlab_p4_results.py` is diagnostic only and does not replace this
strict validator.
