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
REPETITIONS=1
LOCAL_NODE=
REMOTE_NODE=
CALIBRATION_FILE=
MIGRATION_COST_ARTIFACT=
THREAD_EVALUATION_HORIZON_SECONDS=10
READY_TIMEOUT_SECONDS=30
CHILD_PIDS=()
RUN_STATUS=IN_PROGRESS

usage() {
    cat <<'EOF'
Usage: scripts/run_full_experiment.sh [--check-only|--tests-only|--baseline-only|--awavma-only] [--skip-graphs] [--output-dir DIR] [--calibration FILE] [--migration-cost-artifact FILE] [--thread-evaluation-horizon-seconds SECONDS]

Public options:
  --check-only      validate the allocation without collecting
  --tests-only      build and run the required project tests
  --baseline-only   collect the three baseline scenarios
  --awavma-only     collect the AWAVMA scenario
  --skip-graphs     aggregate but do not generate graphs
  --output-dir DIR  collection root (default: results/cloudlab)
  --calibration FILE  validated benefit calibration required for adaptive AWAVMA execution
  --migration-cost-artifact FILE  validated directed thread-migration cost artifact required for adaptive AWAVMA execution
  --thread-evaluation-horizon-seconds SECONDS  positive active-work ROI horizon (default: 10)
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
        --migration-cost-artifact) need_value "$@"; MIGRATION_COST_ARTIFACT=$2; shift ;;
        --thread-evaluation-horizon-seconds) need_value "$@"; THREAD_EVALUATION_HORIZON_SECONDS=$2; shift ;;
        *) printf 'unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done
[[ "$OUTPUT_DIR" = /* ]] || OUTPUT_DIR="$ROOT/$OUTPUT_DIR"

[[ "$THREAD_EVALUATION_HORIZON_SECONDS" =~ ^([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$ ]] &&
    awk -v horizon="$THREAD_EVALUATION_HORIZON_SECONDS" 'BEGIN { exit !(horizon + 0 > 0) }' || {
    printf '%s\n' '--thread-evaluation-horizon-seconds must be positive' >&2
    exit 2
}

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
    numactl --cpunodebind="$REMOTE_NODE" --membind="$LOCAL_NODE" true >/dev/null 2>&1 || limited 'remote-thread/local-memory NUMA binding is not permitted'
}
run_required_tests() {
    make -C "$ROOT" test-runtime test-awavma-runtime test-runtime-migration-validation \
        test-benefit-calibration test-runtime-evidence test-phase5-benefit-evidence test-decision-benefit-evidence test-validation \
        test-migration test-feedback test-phase4d-tooling test-phase4d-aggregation
    python3 "$ROOT/tests/runtime_diagnostic_builder_test.py"
    python3 "$ROOT/tests/runtime_diagnostic_graph_test.py"
    python3 "$ROOT/tests/adaptive_outcome_test.py"
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
# calibration, manifests, baseline, AWAVMA, aggregate, graphs.
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
printf '%s\n' 'timestamp_utc,repetition,adaptive_outcome,runtime_status,detail' >"$RUN_DIR/adaptive_outcomes.csv"

validate_calibration_route() {
    local source=$1 target=$2

    [[ -n "$CALIBRATION_FILE" && -f "$CALIBRATION_FILE" ]] || {
        printf 'awavma execution requires --calibration FILE\n' >&2
        return 1
    }
    awk -F, -v source="$source" -v target="$target" '
        NR == 1 {
            for (i = 1; i <= NF; i++) column[$i] = i
            next
        }
        NR == 2 {
            required = "source_node target_node throughput_gain_percent state validation_status environment_check_status"
            split(required, names, " ")
            for (i in names) if (!(names[i] in column)) invalid = 1
            gain = $(column["throughput_gain_percent"])
            if ($(column["source_node"]) != source || $(column["target_node"]) != target ||
                gain !~ /^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$/ || gain + 0 <= 0 ||
                $(column["state"]) != "VALIDATED_PRODUCTION" ||
                $(column["validation_status"]) != "PASS" ||
                $(column["environment_check_status"]) != "READY") invalid = 1
            valid = 1
            next
        }
        { invalid = 1 }
        END { exit valid && !invalid ? 0 : 1 }
    ' "$CALIBRATION_FILE"
}

validate_migration_cost_route() {
    local source=$1 target=$2

    [[ -n "$MIGRATION_COST_ARTIFACT" && -f "$MIGRATION_COST_ARTIFACT" ]] || {
        printf 'awavma execution requires --migration-cost-artifact FILE\n' >&2
        return 1
    }
    awk -F, -v source="$source" -v target="$target" '
        NR == 1 {
            if ($0 != "schema_version,source_node,target_node,migration_cost_seconds,validation_status,provenance") invalid = 1
            next
        }
        NR == 2 {
            if (NF != 6 || $1 != "1" || $2 !~ /^[0-9]+$/ || $3 !~ /^[0-9]+$/ ||
                $2 != source || $3 != target ||
                $4 !~ /^[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?$/ || $4 + 0 <= 0 ||
                $5 != "PASS" || $6 == "") invalid = 1
            valid = 1
            next
        }
        { invalid = 1 }
        END { exit valid && !invalid ? 0 : 1 }
    ' "$MIGRATION_COST_ARTIFACT"
}

write_manifest() {
    local status=$1 temporary="$RUN_DIR/manifest.json.tmp.$$"

    printf '{"schema_version":4,"run_id":"%s","data_source":"REAL","collection_status":"%s","local_node":%s,"remote_node":%s,"benefit_calibration":"%s","migration_cost_artifact":"%s","thread_evaluation_horizon_seconds":"%s","workload":"%s","threads":%s,"memory_mb":%s,"duration_seconds":%s,"repetitions":%s,"topology_source":"metadata/numa_topology.txt","stage_order":"metadata,build,tests,environment-check,preflight,calibration,manifests,baseline,awavma,aggregate,graphs"}\n' \
        "$EXPERIMENT_ID" "$status" "$LOCAL_NODE" "$REMOTE_NODE" "$CALIBRATION_FILE" "$MIGRATION_COST_ARTIFACT" "$THREAD_EVALUATION_HORIZON_SECONDS" "$WORKLOAD" "$THREADS" "$MEMORY_MB" "$DURATION_SECONDS" "$REPETITIONS" >"$temporary"
    mv "$temporary" "$RUN_DIR/manifest.json"
}

cleanup_children() {
    local pid
    for pid in "${CHILD_PIDS[@]}"; do
        [[ "$pid" =~ ^[1-9][0-9]*$ ]] || continue
        kill -TERM "$pid" 2>/dev/null || true
    done
    for pid in "${CHILD_PIDS[@]}"; do
        [[ "$pid" =~ ^[1-9][0-9]*$ ]] || continue
        wait "$pid" 2>/dev/null || true
    done
    CHILD_PIDS=()
}

finish_run() {
    local exit_code=$?
    cleanup_children
    if [[ "$RUN_STATUS" == IN_PROGRESS ]]; then
        RUN_STATUS=FAILED
        write_manifest "$RUN_STATUS"
    fi
    exit "$exit_code"
}

wait_for_signal() {
    local path=$1 pid=$2 label=$3 deadline=$((SECONDS + READY_TIMEOUT_SECONDS))

    while [[ ! -f "$path" ]]; do
        kill -0 "$pid" 2>/dev/null || {
            printf '%s exited before signaling readiness\n' "$label" >&2
            return 1
        }
        ((SECONDS < deadline)) || {
            printf 'timed out waiting for %s readiness\n' "$label" >&2
            return 1
        }
        sleep 0.1
    done
}

if [[ "$MODE" != baseline ]]; then
    validate_calibration_route "$REMOTE_NODE" "$LOCAL_NODE" || {
        printf 'calibration must be validated for directed route node %s -> node %s with positive throughput gain\n' "$REMOTE_NODE" "$LOCAL_NODE" >&2
        exit 1
    }
    validate_migration_cost_route "$REMOTE_NODE" "$LOCAL_NODE" || {
        printf 'migration cost artifact must be validated for directed route node %s -> node %s\n' "$REMOTE_NODE" "$LOCAL_NODE" >&2
        exit 1
    }
fi
write_manifest "$RUN_STATUS"
trap finish_run EXIT
trap 'exit 130' INT TERM

run_one() {
    local scenario=$1 repetition=$2 directory placement=() numa_args=() runtime=false code=0 runtime_code=0 start end elapsed status=FAILED native_csv operations= execution_time= throughput=
    local thread_node= memory_node=
    case "$scenario" in
        baseline-default) directory="$RUN_DIR/baseline" ;;
        baseline-local) directory="$RUN_DIR/baseline"; placement=(numactl "--cpunodebind=$LOCAL_NODE" "--membind=$LOCAL_NODE"); thread_node=$LOCAL_NODE; memory_node=$LOCAL_NODE ;;
        baseline-remote) directory="$RUN_DIR/baseline"; placement=(numactl "--cpunodebind=$REMOTE_NODE" "--membind=$LOCAL_NODE"); thread_node=$REMOTE_NODE; memory_node=$LOCAL_NODE ;;
        awavma) directory="$RUN_DIR/awavma"; placement=(numactl "--cpunodebind=$REMOTE_NODE" "--membind=$LOCAL_NODE"); thread_node=$REMOTE_NODE; memory_node=$LOCAL_NODE; runtime=true ;;
    esac
    [[ -n "$thread_node" ]] && numa_args=(--thread-node "$thread_node" --memory-node "$memory_node")
    native_csv="$directory/native/$scenario-$repetition.csv"
    start=$(date +%s%N)
    if [[ "$scenario" == awavma ]]; then
        local control_dir="$RUN_DIR/awavma/runtime/$repetition/control"
        local benchmark_ready="$control_dir/benchmark.ready" benchmark_start="$control_dir/benchmark.start"
        local runtime_ready="$control_dir/runtime.ready" target_ready="$control_dir/target.ready"
        mkdir -p "$control_dir"
        "${placement[@]}" "$ROOT/bin/benchmark" --threads "$THREADS" --memory "$MEMORY_MB" --duration "$DURATION_SECONDS" --pattern "$WORKLOAD" "${numa_args[@]}" --output "$native_csv" --ready-file "$benchmark_ready" --start-file "$benchmark_start" >"$directory/logs/$scenario-$repetition.benchmark.log" 2>&1 &
        local benchmark_pid=$!
        CHILD_PIDS+=("$benchmark_pid")
        wait_for_signal "$benchmark_ready" "$benchmark_pid" benchmark || code=1
        local runtime_pid=
        if [[ "$code" == 0 ]]; then
            "$ROOT/bin/awavma-runtime" --duration-ms $((DURATION_SECONDS * 1000 + 5000)) --pid "$benchmark_pid" --root-dir "$RUN_DIR/awavma/runtime/$repetition" --config "$ROOT/config/awavma.conf" --migration-safety-enabled --migration-execution-enabled --benefit-calibration "$CALIBRATION_FILE" --migration-cost-artifact "$MIGRATION_COST_ARTIFACT" --thread-evaluation-horizon-seconds "$THREAD_EVALUATION_HORIZON_SECONDS" --ready-file "$runtime_ready" --target-ready-file "$target_ready" >"$directory/logs/$scenario-$repetition.runtime.log" 2>&1 &
            runtime_pid=$!
        fi
        if [[ "$code" == 0 ]]; then
            CHILD_PIDS+=("$runtime_pid")
            wait_for_signal "$runtime_ready" "$runtime_pid" runtime || code=1
            [[ "$code" != 0 ]] || wait_for_signal "$target_ready" "$runtime_pid" runtime-target || code=1
            [[ "$code" != 0 ]] || : >"$benchmark_start"
        fi
        if [[ "$code" == 0 ]]; then
            wait "$benchmark_pid" || code=$?
        else
            kill -TERM "$benchmark_pid" 2>/dev/null || true
            wait "$benchmark_pid" 2>/dev/null || true
        fi
        if [[ -n "$runtime_pid" ]]; then
            kill -TERM "$runtime_pid" 2>/dev/null || true
            wait "$runtime_pid" 2>/dev/null || runtime_code=$?
            [[ "$runtime_code" == 0 || "$runtime_code" == 143 ]] || code=$runtime_code
        fi
        CHILD_PIDS=()
    else
        "${placement[@]}" "$ROOT/bin/benchmark" --threads "$THREADS" --memory "$MEMORY_MB" --duration "$DURATION_SECONDS" --pattern "$WORKLOAD" "${numa_args[@]}" --output "$native_csv" >"$directory/logs/$scenario-$repetition.benchmark.log" 2>&1 || code=$?
    fi
    end=$(date +%s%N)
    elapsed=$(printf '%d.%09d' $(((end - start) / 1000000000)) $(((end - start) % 1000000000)))
    [[ "$code" == 0 ]] && status=MEASURED
    if [[ "$status" == MEASURED ]]; then
        IFS=, read -r _ _ _ _ _ _ _ _ _ operations execution_time throughput < <(tail -n 1 "$native_csv")
        [[ "$operations" =~ ^[0-9]+$ && "$execution_time" =~ ^[0-9]+([.][0-9]+)?$ && "$throughput" =~ ^[0-9]+([.][0-9]+)?$ ]] || { status=FAILED; code=1; }
    fi
    printf '%s\n' "4,$EXPERIMENT_ID,$(date -u +%Y-%m-%dT%H:%M:%SZ),$EXPERIMENT_ID-$scenario-$repetition,$scenario,$WORKLOAD,$repetition,$thread_node,$memory_node,$runtime,$THREADS,$MEMORY_MB,$DURATION_SECONDS,$elapsed,$operations,$execution_time,$throughput,$code,$status" >>"$RUN_DIR/measurements.csv"
    if [[ "$scenario" == awavma ]]; then
        local runtime_results="$RUN_DIR/awavma/runtime/$repetition/runtime_results.csv" adaptive=EXPERIMENT_COMPLETED_NO_ADAPTIVE_ACTION runtime_status=NA detail=NA
        if [[ -f "$runtime_results" ]]; then
            IFS=, read -r _ _ _ _ _ _ runtime_status detail < <(tail -n 1 "$runtime_results")
        fi
        adaptive=$(python3 "$ROOT/scripts/determine_adaptive_outcome.py" --runtime-dir "$RUN_DIR/awavma/runtime/$repetition")
        printf '%s,%s,%s,%s,%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$repetition" "$adaptive" "$runtime_status" "$detail" >>"$RUN_DIR/adaptive_outcomes.csv"
        python3 "$ROOT/scripts/build_runtime_diagnostics.py" --runtime-dir "$RUN_DIR/awavma/runtime/$repetition" --output "$RUN_DIR/awavma/runtime/$repetition/runtime_diagnostics.csv"
    fi
    [[ "$status" == MEASURED ]]
}

failed=0
if [[ "$MODE" != awavma ]]; then
    for scenario in baseline-default baseline-local baseline-remote; do for repetition in $(seq 1 "$RUNS"); do run_one "$scenario" "$repetition" || failed=$((failed + 1)); done; done
fi
if [[ "$MODE" != baseline ]]; then
    for repetition in $(seq 1 "$RUNS"); do run_one awavma "$repetition" || failed=$((failed + 1)); done
fi
((failed == 0)) || exit 1
STAGING_DIR="$RUN_DIR/finalized"
UNIFIED="$STAGING_DIR/unified"
GRAPHS="$STAGING_DIR/graphs"
python3 "$ROOT/scripts/aggregate_experiment_results.py" --input-dir "$OUTPUT_DIR/raw" --include-in-progress-run "$EXPERIMENT_ID" --output "$UNIFIED/cloudlab_experiment_results.csv" --comparison-output "$UNIFIED/cloudlab_comparator.csv" --summary-dir "$UNIFIED/summaries"
if [[ "$MODE" == all && "$SKIP_GRAPHS" == false ]]; then
    diagnostic_args=()
    while IFS= read -r diagnostic; do diagnostic_args+=(--diagnostic "$diagnostic"); done < <(find "$RUN_DIR/awavma/runtime" -name runtime_diagnostics.csv -type f)
    python3 "$ROOT/scripts/generate_multinuma_graphs.py" --input "$UNIFIED/cloudlab_comparator.csv" --summary-dir "$UNIFIED/summaries" --summary "$GRAPHS/graph_summary.csv" --output-dir "$GRAPHS" "${diagnostic_args[@]}"
fi
mkdir -p "$OUTPUT_DIR/unified" "$OUTPUT_DIR/graphs"
mv "$UNIFIED/cloudlab_experiment_results.csv" "$UNIFIED/cloudlab_comparator.csv" "$OUTPUT_DIR/unified/"
mkdir -p "$OUTPUT_DIR/unified/summaries"
mv "$UNIFIED/summaries"/* "$OUTPUT_DIR/unified/summaries/"
if [[ -d "$GRAPHS" ]]; then
    mv "$GRAPHS"/* "$OUTPUT_DIR/graphs/"
fi
RUN_STATUS=PASS
write_manifest "$RUN_STATUS"
printf 'Phase 4D artifacts: %s\n' "$RUN_DIR"
