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

usage() {
    cat <<'EOF'
Usage: scripts/run_full_experiment.sh [--check-only|--tests-only|--baseline-only|--awavma-only] [--skip-graphs] [--output-dir DIR]

Public options (exactly six):
  --check-only      validate the allocation without collecting
  --tests-only      build and run the required project tests
  --baseline-only   collect the three baseline scenarios
  --awavma-only     collect the AWAVMA scenario
  --skip-graphs     aggregate but do not generate graphs
  --output-dir DIR  collection root (default: results/cloudlab)
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
validate_placement() {
    local artifact=$1 mode=$2
    python3 - "$artifact" "$mode" <<'PY'
import csv, sys
path, mode = sys.argv[1:]
with open(path, newline='', encoding='utf-8') as handle:
    rows = list(csv.DictReader(handle))
if len(rows) != 1:
    raise SystemExit('placement artifact must contain one row')
row = rows[0]
required = ('placement_mode', 'verification_status', 'memory_policy_restored')
if any(not row.get(field, '') for field in required) or row['placement_mode'] != mode:
    raise SystemExit('placement artifact is malformed or has wrong mode')
if mode != 'default' and (row['verification_status'] != 'PASS' or row['memory_policy_restored'] != 'true'):
    raise SystemExit('controlled placement was not verified and restored')
print(','.join(row.get(field, '') for field in ('placement_mode', 'local_node', 'requested_memory_node', 'numa_distance', 'total_pages', 'queryable_pages', 'local_pages', 'remote_pages', 'other_pages', 'unknown_pages', 'expected_node_ratio', 'verification_status', 'memory_policy_restored')))
PY
}
validate_remote_equivalence() {
    local remote=$1 awavma=$2
    python3 - "$remote" "$awavma" <<'PY'
import csv, sys
def row(path):
    with open(path, newline='', encoding='utf-8') as handle: return next(csv.DictReader(handle))
left, right = row(sys.argv[1]), row(sys.argv[2])
fields = ('placement_mode', 'local_node', 'requested_memory_node', 'numa_distance', 'total_pages', 'queryable_pages', 'expected_node_pages', 'local_pages', 'remote_pages', 'other_pages', 'unknown_pages', 'expected_node_ratio', 'verification_status', 'memory_policy_restored')
if any(left.get(field) != right.get(field) for field in fields):
    raise SystemExit('REMOTE/AWAVMA placement evidence differs')
PY
}
preflight() {
    local metadata=$1 require_production=${2:-false} environment_code=0
    "$ROOT/bin/environment-check" >"$metadata/environment_check.txt" 2>&1 || environment_code=$?
    if [[ "$environment_code" != 0 ]]; then
        [[ "$environment_code" == "$ENV_LIMITED" ]] &&
            limited 'environment capability profile is unavailable'
        limited 'environment capability check failed'
    fi
    if [[ "$require_production" == true ]]; then
        python3 "$ROOT/scripts/verify_runtime_execution_profile.py" \
            --environment-check "$metadata/environment_check.txt" ||
            limited 'production real-migration capability profile is unavailable'
    fi
    discover_nodes
    # Benchmark-owned P3 setup verifies the required temporary policy; do not inherit a binding here.
}
run_required_tests() {
    make -C "$ROOT" test-runtime test-awavma-runtime test-runtime-migration-validation \
        test-environment-capabilities test-runtime-execution-profile \
        test-phase5-benefit-evidence test-decision-benefit-evidence test-validation \
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
    preflight "$temporary" true
    printf 'Phase 4D environment check: OK (visible nodes %s and %s; benchmark selects placement)\n' "$LOCAL_NODE" "$REMOTE_NODE"
    exit 0
fi

EXPERIMENT_ID="phase4d-$(date -u +%Y%m%dT%H%M%SZ)-$$"
RUN_DIR="$OUTPUT_DIR/raw/$EXPERIMENT_ID"
[[ ! -e "$RUN_DIR" ]] || { printf 'run directory already exists: %s\n' "$RUN_DIR" >&2; exit 2; }
mkdir -p "$RUN_DIR/metadata" "$RUN_DIR/baseline/logs" "$RUN_DIR/awavma/logs" "$RUN_DIR/awavma/runtime"

# Required order: metadata, build, tests, environment check, preflight,
# baseline, AWAVMA, aggregate, graphs, manifests.
date -u +%Y-%m-%dT%H:%M:%SZ >"$RUN_DIR/metadata/collection_started_utc.txt"
make -C "$ROOT" benchmark awavma-runtime environment-check >"$RUN_DIR/metadata/build.log" 2>&1
run_required_tests >"$RUN_DIR/metadata/tests.log" 2>&1
preflight "$RUN_DIR/metadata" "$([[ "$MODE" != baseline ]] && printf true || printf false)"
numactl --hardware >"$RUN_DIR/metadata/numa_topology.txt"
uname -a >"$RUN_DIR/metadata/uname.txt"
locale >"$RUN_DIR/metadata/locale.txt"
(gcc --version || cc --version) >"$RUN_DIR/metadata/compiler.txt" 2>&1
git -C "$ROOT" rev-parse HEAD >"$RUN_DIR/metadata/git_revision.txt" 2>/dev/null || true
git -C "$ROOT" status --short >"$RUN_DIR/metadata/git_status.txt" 2>/dev/null || true
    printf '%s\n' 'schema_version,experiment_id,timestamp_utc,run_id,scenario,workload,repetition,thread_node,memory_node,runtime_enabled,requested_execution_mode,effective_execution_mode,activation_state,activation_reason,migration_safety_effective,migration_execution_effective,page_registration_effective,page_registration_ttl_ms,registration_required,registration_acknowledged,registration_status,placement_mode,numa_distance,initial_total_pages,initial_queryable_pages,initial_local_pages,initial_remote_pages,initial_other_pages,initial_unknown_pages,initial_expected_node_ratio,placement_verification_status,memory_policy_restored,numa_nodes,thread_migration_ready,page_migration_ready,phase7_ready,production_real_migration_ready,threads,memory_mb,duration_seconds,elapsed_seconds,exit_code,status' >"$RUN_DIR/measurements.csv"

run_one() {
    local scenario=$1 repetition=$2 directory runtime=false code=0 runtime_code=0 start end elapsed status=FAILED placement_mode=default placement_path placement_fields=
    local thread_node= memory_node=
    local requested_mode=MONITORING effective_mode=MONITORING activation_state=NOT_APPLICABLE
    local activation_reason=baseline migration_safety=false migration_execution=false page_registration=false
    local registration_ttl= registration_required=false registration_acknowledged=false registration_status=NOT_REQUIRED numa_nodes= thread_ready= page_ready= phase7_ready= production_ready= distance= total_pages= queryable_pages= local_pages= remote_pages= other_pages= unknown_pages= expected_ratio= placement_status= policy_restored=
    case "$scenario" in
        baseline-default) directory="$RUN_DIR/baseline" ;;
        baseline-local) directory="$RUN_DIR/baseline"; placement_mode=local ;;
        baseline-remote) directory="$RUN_DIR/baseline"; placement_mode=remote ;;
        awavma) directory="$RUN_DIR/awavma"; placement_mode=remote; runtime=true ;;
    esac
    placement_path="$directory/placement-$scenario-$repetition.csv"
    start=$(date +%s%N)
    if [[ "$scenario" == awavma ]]; then
        local runtime_root="$RUN_DIR/awavma/runtime/$repetition"
        local profile_path="$runtime_root/runtime_execution_profile.csv"
        local registration_socket="$runtime_root/page-registration.sock"
        [[ ! -e "$runtime_root" ]] || {
            printf 'runtime root already exists: %s\n' "$runtime_root" >&2
            return 1
        }
        registration_required=true
        registration_status=PENDING
        "$ROOT/bin/benchmark" --threads "$THREADS" --memory "$MEMORY_MB" --duration "$DURATION_SECONDS" --pattern "$WORKLOAD" --placement-mode "$placement_mode" --placement-evidence "$placement_path" --page-registration-required --page-registration-socket "$registration_socket" --page-registration-timeout-ms 30000 >"$directory/logs/$scenario-$repetition.benchmark.log" 2>&1 &
        local benchmark_pid=$!
        "$ROOT/bin/awavma-runtime" --production-real-migration -S -M -R \
            --duration-ms $((DURATION_SECONDS * 1000 + 5000)) --pid "$benchmark_pid" \
            --root-dir "$runtime_root" --config "$ROOT/config/awavma.conf" \
            >"$directory/logs/$scenario-$repetition.runtime.log" 2>&1 &
        local runtime_pid=$!
        local active=false profile_fields=
        for _ in $(seq 1 50); do
            if [[ -f "$profile_path" ]] && profile_fields=$(python3 "$ROOT/scripts/verify_runtime_execution_profile.py" --profile "$profile_path" --emit-fields 2>>"$directory/logs/$scenario-$repetition.runtime.log"); then
                active=true
                break
            fi
            if ! kill -0 "$runtime_pid" 2>/dev/null; then
                break
            fi
            sleep 0.1
        done
        if [[ "$active" == true ]]; then
            IFS=, read -r requested_mode effective_mode migration_safety migration_execution page_registration registration_ttl numa_nodes thread_ready page_ready phase7_ready production_ready activation_state activation_reason <<<"$profile_fields"
            cp "$profile_path" "$RUN_DIR/metadata/awavma-runtime-profile-$repetition.csv"
            wait "$benchmark_pid" || code=$?
            if [[ "$code" == 0 ]] && ! placement_fields=$(validate_placement "$placement_path" "$placement_mode"); then
                code=1
                registration_status=PLACEMENT_VALIDATION_FAILED
            fi
            if [[ "$code" == 0 ]] && ! validate_remote_equivalence "$RUN_DIR/baseline/placement-baseline-remote-$repetition.csv" "$placement_path"; then
                code=1
                registration_status=PLACEMENT_EQUIVALENCE_FAILED
            fi
            if [[ "$code" == 0 ]]; then
                registration_acknowledged=true
                registration_status=ACCEPTED
            else
                registration_status=BENCHMARK_REGISTRATION_FAILED
            fi
        else
            activation_state=ACTIVATION_FAILED
            activation_reason=runtime_profile_missing_or_inactive
            if [[ -f "$profile_path" ]]; then
                cp "$profile_path" "$RUN_DIR/metadata/awavma-runtime-profile-$repetition.csv"
                IFS=, read -r requested_mode effective_mode migration_safety migration_execution page_registration registration_ttl numa_nodes thread_ready page_ready phase7_ready production_ready activation_state activation_reason < <(python3 - "$profile_path" <<'PY'
import csv, sys
with open(sys.argv[1], newline='', encoding='utf-8') as handle:
    row = next(csv.DictReader(handle), {})
fields = ('requested_execution_mode', 'effective_execution_mode', 'migration_safety_effective', 'migration_execution_effective', 'page_registration_effective', 'page_registration_ttl_ms', 'numa_nodes', 'thread_migration_ready', 'page_migration_ready', 'phase7_ready', 'production_real_migration_ready', 'activation_state', 'activation_reason')
print(','.join(row.get(field, '') for field in fields))
PY
)
            fi
            code=1
            kill -TERM "$benchmark_pid" 2>/dev/null || true
            wait "$benchmark_pid" || true
        fi
        kill -TERM "$runtime_pid" 2>/dev/null || true
        wait "$runtime_pid" || runtime_code=$?
        if [[ "$runtime_code" != 0 ]]; then
            code=$runtime_code
            [[ "$activation_state" == ACTIVE ]] && activation_state=RUNTIME_TERMINATED
        fi
    else
        "$ROOT/bin/benchmark" --threads "$THREADS" --memory "$MEMORY_MB" --duration "$DURATION_SECONDS" --pattern "$WORKLOAD" --placement-mode "$placement_mode" --placement-evidence "$placement_path" >"$directory/logs/$scenario-$repetition.benchmark.log" 2>&1 || code=$?
        if [[ "$code" == 0 ]] && ! placement_fields=$(validate_placement "$placement_path" "$placement_mode"); then
            code=1
        fi
    fi
    end=$(date +%s%N)
    elapsed=$(printf '%d.%09d' $(((end - start) / 1000000000)) $(((end - start) % 1000000000)))
    [[ "$code" == 0 ]] && status=MEASURED
    if [[ -n "$placement_fields" ]]; then IFS=, read -r placement_mode thread_node memory_node distance total_pages queryable_pages local_pages remote_pages other_pages unknown_pages expected_ratio placement_status policy_restored <<<"$placement_fields"; fi
    printf '%s\n' "7,$EXPERIMENT_ID,$(date -u +%Y-%m-%dT%H:%M:%SZ),$EXPERIMENT_ID-$scenario-$repetition,$scenario,$WORKLOAD,$repetition,$thread_node,$memory_node,$runtime,$requested_mode,$effective_mode,$activation_state,$activation_reason,$migration_safety,$migration_execution,$page_registration,$registration_ttl,$registration_required,$registration_acknowledged,$registration_status,$placement_mode,$distance,$total_pages,$queryable_pages,$local_pages,$remote_pages,$other_pages,$unknown_pages,$expected_ratio,$placement_status,$policy_restored,$numa_nodes,$thread_ready,$page_ready,$phase7_ready,$production_ready,$THREADS,$MEMORY_MB,$DURATION_SECONDS,$elapsed,$code,$status" >>"$RUN_DIR/measurements.csv"
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
{"schema_version":5,"run_id":"$EXPERIMENT_ID","data_source":"REAL","collection_status":"$status","local_node":$LOCAL_NODE,"remote_node":$REMOTE_NODE,"topology_source":"metadata/numa_topology.txt","runtime_execution_profile":"awavma/runtime/*/runtime_execution_profile.csv","stage_order":"metadata,build,tests,environment-check,preflight,baseline,awavma,aggregate,graphs,manifests"}
EOF
((failed == 0)) || exit 1
UNIFIED="$OUTPUT_DIR/unified"
python3 "$ROOT/scripts/aggregate_experiment_results.py" --input-dir "$OUTPUT_DIR/raw" --output "$UNIFIED/cloudlab_experiment_results.csv" --comparison-output "$UNIFIED/cloudlab_comparator.csv" --summary-dir "$UNIFIED/summaries"
[[ "$SKIP_GRAPHS" == true ]] || python3 "$ROOT/scripts/generate_multinuma_graphs.py" --input "$UNIFIED/cloudlab_comparator.csv" --summary-dir "$UNIFIED/summaries" --summary "$OUTPUT_DIR/graphs/graph_summary.csv" --output-dir "$OUTPUT_DIR/graphs"
printf 'Phase 4D artifacts: %s\n' "$RUN_DIR"
