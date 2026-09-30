#!/usr/bin/env bash
# Phase 4D reproducible multi-NUMA collection driver.
set -Eeuo pipefail

ENV_LIMITED=3
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUTPUT_DIR="$ROOT/results/cloudlab"
MODE=all
SKIP_GRAPHS=false
RUNS=5
DURATION_SECONDS=30
THREADS=2
MEMORY_MB=1024
WORKLOAD=mixed
LOCAL_NODE=
REMOTE_NODE=
CALIBRATION_FILE=

usage() {
    cat <<'EOF'
Usage: scripts/run_full_experiment.sh [--check-only|--tests-only|--baseline-only|--awavma-only] [--skip-graphs] [--output-dir DIR] [--calibration FILE]

Public options:
  --check-only      validate the allocation without collecting
  --tests-only      build and run the required project tests
  --baseline-only   collect the three baseline scenarios
  --awavma-only     collect the AWAVMA scenario
  --skip-graphs     aggregate but do not generate graphs
  --output-dir DIR  collection root (default: results/cloudlab)
  --calibration FILE  validated benefit calibration required for adaptive AWAVMA execution
EOF
}

need_value() { [[ $# -ge 2 ]] || { printf 'missing value for %s\n' "$1" >&2; exit 2; }; }
while (($#)); do
    case "$1" in
        --check-only) MODE=check ;;
        --tests-only) MODE=tests ;;
        --baseline-only) MODE=baseline ;;
        --awavma-only) MODE=awavma ;;
        --skip-graphs) SKIP_GRAPHS=true ;;
        --output-dir) need_value "$@"; OUTPUT_DIR=$2; shift ;;
        --calibration) need_value "$@"; CALIBRATION_FILE=$2; shift ;;
        *) printf 'unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done
[[ "$OUTPUT_DIR" = /* ]] || OUTPUT_DIR="$ROOT/$OUTPUT_DIR"

limited() { printf 'ENV_LIMITED: %s\n' "$1" >&2; exit "$ENV_LIMITED"; }
discover_nodes() {
    command -v numactl >/dev/null 2>&1 || limited 'numactl is required'
    local line
    NODES=()
    while IFS= read -r line; do
        [[ "$line" =~ ^node[[:space:]]+([0-9]+)[[:space:]]+cpus: ]] && NODES+=("${BASH_REMATCH[1]}")
    done < <(numactl --hardware)
    ((${#NODES[@]} >= 2)) || limited 'fewer than two NUMA nodes are visible'
    LOCAL_NODE=${NODES[0]}
    REMOTE_NODE=${NODES[1]}
}
preflight() {
    local metadata=$1
    discover_nodes
    "$ROOT/bin/environment-check" >"$metadata/environment_check.txt" 2>&1 || limited 'environment capability check failed'
    numactl --cpunodebind="$LOCAL_NODE" --membind="$LOCAL_NODE" true >/dev/null 2>&1 || limited 'local NUMA binding is not permitted'
    numactl --cpunodebind="$LOCAL_NODE" --membind="$REMOTE_NODE" true >/dev/null 2>&1 || limited 'remote NUMA binding is not permitted'
}
run_required_tests() {
    make -C "$ROOT" test-runtime test-awavma-runtime test-runtime-migration-validation \
        test-benefit-calibration test-phase5-benefit-evidence test-decision-benefit-evidence test-validation \
        test-migration test-feedback test-phase4d-tooling test-phase4d-aggregation
}

if [[ "$MODE" == tests ]]; then
    run_required_tests
    exit 0
fi
if [[ "$MODE" == check ]]; then
    make -C "$ROOT" environment-check
    temporary=$(mktemp -d)
    trap 'rm -rf "$temporary"' EXIT
    preflight "$temporary"
    printf 'Phase 4D environment check: OK (local node %s, remote node %s)\n' "$LOCAL_NODE" "$REMOTE_NODE"
    exit 0
fi

EXPERIMENT_ID="phase4d-$(date -u +%Y%m%dT%H%M%SZ)-$$"
RUN_DIR="$OUTPUT_DIR/raw/$EXPERIMENT_ID"
[[ ! -e "$RUN_DIR" ]] || { printf 'run directory already exists: %s\n' "$RUN_DIR" >&2; exit 2; }
mkdir -p "$RUN_DIR/metadata" "$RUN_DIR/baseline/logs" "$RUN_DIR/baseline/native" "$RUN_DIR/awavma/logs" "$RUN_DIR/awavma/native" "$RUN_DIR/awavma/runtime"

# Required order: metadata, build, tests, environment check, preflight,
# baseline, AWAVMA, aggregate, graphs, manifests.
date -u +%Y-%m-%dT%H:%M:%SZ >"$RUN_DIR/metadata/collection_started_utc.txt"
make -C "$ROOT" benchmark awavma-runtime environment-check >"$RUN_DIR/metadata/build.log" 2>&1
run_required_tests >"$RUN_DIR/metadata/tests.log" 2>&1
preflight "$RUN_DIR/metadata"
numactl --hardware >"$RUN_DIR/metadata/numa_topology.txt"
uname -a >"$RUN_DIR/metadata/uname.txt"
locale >"$RUN_DIR/metadata/locale.txt"
(gcc --version || cc --version) >"$RUN_DIR/metadata/compiler.txt" 2>&1
git -C "$ROOT" rev-parse HEAD >"$RUN_DIR/metadata/git_revision.txt" 2>/dev/null || true
git -C "$ROOT" status --short >"$RUN_DIR/metadata/git_status.txt" 2>/dev/null || true
printf '%s\n' 'schema_version,experiment_id,timestamp_utc,run_id,scenario,workload,repetition,thread_node,memory_node,runtime_enabled,threads,memory_mb,duration_seconds,elapsed_seconds,operations,benchmark_execution_time_sec,throughput_ops_sec,exit_code,status' >"$RUN_DIR/measurements.csv"

run_one() {
    local scenario=$1 repetition=$2 directory placement=() numa_args=() runtime=false code=0 runtime_code=0 start end elapsed status=FAILED native_csv operations= execution_time= throughput=
    local thread_node= memory_node=
    case "$scenario" in
        baseline-default) directory="$RUN_DIR/baseline" ;;
        baseline-local) directory="$RUN_DIR/baseline"; placement=(numactl "--cpunodebind=$LOCAL_NODE" "--membind=$LOCAL_NODE"); thread_node=$LOCAL_NODE; memory_node=$LOCAL_NODE ;;
        baseline-remote) directory="$RUN_DIR/baseline"; placement=(numactl "--cpunodebind=$LOCAL_NODE" "--membind=$REMOTE_NODE"); thread_node=$LOCAL_NODE; memory_node=$REMOTE_NODE ;;
        awavma) directory="$RUN_DIR/awavma"; placement=(numactl "--cpunodebind=$LOCAL_NODE" "--membind=$LOCAL_NODE"); thread_node=$LOCAL_NODE; memory_node=$LOCAL_NODE; runtime=true ;;
    esac
    [[ -n "$thread_node" ]] && numa_args=(--thread-node "$thread_node" --memory-node "$memory_node")
    native_csv="$directory/native/$scenario-$repetition.csv"
    start=$(date +%s%N)
    if [[ "$scenario" == awavma ]]; then
        [[ -n "$CALIBRATION_FILE" ]] || { printf 'awavma execution requires --calibration FILE\n' >&2; return 1; }
        "${placement[@]}" "$ROOT/bin/benchmark" --threads "$THREADS" --memory "$MEMORY_MB" --duration "$DURATION_SECONDS" --pattern "$WORKLOAD" "${numa_args[@]}" --output "$native_csv" >"$directory/logs/$scenario-$repetition.benchmark.log" 2>&1 &
        local benchmark_pid=$!
        "$ROOT/bin/awavma-runtime" --duration-ms $((DURATION_SECONDS * 1000 + 5000)) --pid "$benchmark_pid" --root-dir "$RUN_DIR/awavma/runtime/$repetition" --config "$ROOT/config/awavma.conf" --migration-safety-enabled --migration-execution-enabled --benefit-calibration "$CALIBRATION_FILE" >"$directory/logs/$scenario-$repetition.runtime.log" 2>&1 &
        local runtime_pid=$!
        wait "$benchmark_pid" || code=$?
        kill -TERM "$runtime_pid" 2>/dev/null || true
        wait "$runtime_pid" || runtime_code=$?
        [[ "$runtime_code" == 0 ]] || code=$runtime_code
    else
        "${placement[@]}" "$ROOT/bin/benchmark" --threads "$THREADS" --memory "$MEMORY_MB" --duration "$DURATION_SECONDS" --pattern "$WORKLOAD" "${numa_args[@]}" --output "$native_csv" >"$directory/logs/$scenario-$repetition.benchmark.log" 2>&1 || code=$?
    fi
    end=$(date +%s%N)
    elapsed=$(printf '%d.%09d' $(((end - start) / 1000000000)) $(((end - start) % 1000000000)))
    [[ "$code" == 0 ]] && status=MEASURED
    if [[ "$status" == MEASURED ]]; then IFS=, read -r _ _ _ _ _ _ _ _ _ operations execution_time throughput < <(tail -n 1 "$native_csv"); fi
    printf '%s\n' "4,$EXPERIMENT_ID,$(date -u +%Y-%m-%dT%H:%M:%SZ),$EXPERIMENT_ID-$scenario-$repetition,$scenario,$WORKLOAD,$repetition,$thread_node,$memory_node,$runtime,$THREADS,$MEMORY_MB,$DURATION_SECONDS,$elapsed,$operations,$execution_time,$throughput,$code,$status" >>"$RUN_DIR/measurements.csv"
    [[ "$status" == MEASURED ]]
}

failed=0
if [[ "$MODE" != awavma ]]; then
    for scenario in baseline-default baseline-local baseline-remote; do for repetition in $(seq 1 "$RUNS"); do run_one "$scenario" "$repetition" || failed=$((failed + 1)); done; done
fi
if [[ "$MODE" != baseline ]]; then
    for repetition in $(seq 1 "$RUNS"); do run_one awavma "$repetition" || failed=$((failed + 1)); done
fi
status=PASS; ((failed == 0)) || status=FAIL
cat >"$RUN_DIR/manifest.json" <<EOF
{"schema_version":4,"run_id":"$EXPERIMENT_ID","data_source":"REAL","collection_status":"$status","local_node":$LOCAL_NODE,"remote_node":$REMOTE_NODE,"topology_source":"metadata/numa_topology.txt","stage_order":"metadata,build,tests,environment-check,preflight,baseline,awavma,aggregate,graphs,manifests"}
EOF
((failed == 0)) || exit 1
UNIFIED="$OUTPUT_DIR/unified"
python3 "$ROOT/scripts/aggregate_experiment_results.py" --input-dir "$OUTPUT_DIR/raw" --output "$UNIFIED/cloudlab_experiment_results.csv" --comparison-output "$UNIFIED/cloudlab_comparator.csv" --summary-dir "$UNIFIED/summaries"
[[ "$SKIP_GRAPHS" == true ]] || python3 "$ROOT/scripts/generate_multinuma_graphs.py" --input "$UNIFIED/cloudlab_comparator.csv" --summary-dir "$UNIFIED/summaries" --summary "$OUTPUT_DIR/graphs/graph_summary.csv" --output-dir "$OUTPUT_DIR/graphs"
printf 'Phase 4D artifacts: %s\n' "$RUN_DIR"
