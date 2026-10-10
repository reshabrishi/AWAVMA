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
REFERENCE_RUN=
NUMA_BALANCING_PATH=${AWAVMA_NUMA_BALANCING_PATH:-/proc/sys/kernel/numa_balancing}
NUMA_BALANCING_ORIGINAL=

usage() {
    cat <<'EOF'
Usage: scripts/run_full_experiment.sh [--check-only|--tests-only|--baseline-only|--awavma-only] [--skip-graphs] [--workload PATTERN] [--output-dir DIR] [--calibration FILE] [--reference-run DIR]

Public options:
  --check-only      validate the allocation without collecting
  --tests-only      build and run the required project tests
  --baseline-only   collect the three baseline scenarios
  --awavma-only     collect the AWAVMA scenario
  --skip-graphs     aggregate but do not generate graphs
  --workload NAME   sequential, random, hot, moderate, cold, mixed, or changing (default: mixed)
  --output-dir DIR  collection root (default: results/cloudlab)
  --calibration FILE  validated calibration required by all/AWAVMA collection
  --reference-run DIR  passing baseline run required by --awavma-only
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
        --workload) need_value "$@"; WORKLOAD=$2; shift ;;
        --output-dir) need_value "$@"; OUTPUT_DIR=$2; shift ;;
        --calibration) need_value "$@"; CALIBRATION_FILE=$2; shift ;;
        --reference-run) need_value "$@"; REFERENCE_RUN=$2; shift ;;
        *) printf 'unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done
case "$WORKLOAD" in sequential|random|hot|moderate|cold|mixed|changing) ;; *) printf 'unsupported workload: %s\n' "$WORKLOAD" >&2; exit 2 ;; esac
[[ "$OUTPUT_DIR" = /* ]] || OUTPUT_DIR="$ROOT/$OUTPUT_DIR"
[[ -z "$REFERENCE_RUN" || "$REFERENCE_RUN" = /* ]] || REFERENCE_RUN="$ROOT/$REFERENCE_RUN"

limited() { printf 'ENV_LIMITED: %s\n' "$1" >&2; exit "$ENV_LIMITED"; }
write_numa_balancing() {
    local value=$1
    if [[ -w "$NUMA_BALANCING_PATH" ]]; then printf '%s\n' "$value" >"$NUMA_BALANCING_PATH"
    elif command -v sudo >/dev/null 2>&1; then printf '%s\n' "$value" | sudo tee "$NUMA_BALANCING_PATH" >/dev/null
    else return 1
    fi
    [[ "$(tr -d '[:space:]' <"$NUMA_BALANCING_PATH")" == "$value" ]]
}
restore_numa_balancing() {
    [[ -z "$NUMA_BALANCING_ORIGINAL" ]] || write_numa_balancing "$NUMA_BALANCING_ORIGINAL" || {
        printf 'failed to restore and verify NUMA balancing=%s\n' "$NUMA_BALANCING_ORIGINAL" >&2
        return 1
    }
}
begin_numa_transaction() {
    [[ -r "$NUMA_BALANCING_PATH" ]] || limited "cannot read $NUMA_BALANCING_PATH"
    NUMA_BALANCING_ORIGINAL=$(tr -d '[:space:]' <"$NUMA_BALANCING_PATH")
    [[ "$NUMA_BALANCING_ORIGINAL" == 0 || "$NUMA_BALANCING_ORIGINAL" == 1 ]] || limited 'invalid NUMA balancing state'
    trap 'code=$?; restore_numa_balancing || code=1; exit "$code"' EXIT
    write_numa_balancing 0 || limited 'cannot disable and verify NUMA balancing'
}
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
if row['verification_status'] != 'PASS' or row['memory_policy_restored'] != 'true':
    raise SystemExit('placement was not verified and memory policy was not restored')
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

if [[ "$MODE" == all || "$MODE" == awavma ]]; then
    [[ -n "$CALIBRATION_FILE" ]] || { printf '%s\n' '--calibration FILE is required for AWAVMA collection' >&2; exit 2; }
    [[ -f "$CALIBRATION_FILE" && -r "$CALIBRATION_FILE" ]] || { printf 'calibration must be a readable regular file\n' >&2; exit 2; }
fi
if [[ "$MODE" == awavma ]]; then
    [[ -n "$REFERENCE_RUN" ]] || { printf '%s\n' '--reference-run DIR is required for --awavma-only' >&2; exit 2; }
    [[ -f "$REFERENCE_RUN/manifest.json" && -f "$REFERENCE_RUN/measurements.csv" && -f "$REFERENCE_RUN/benchmark_results.csv" ]] || { printf 'reference run is incomplete\n' >&2; exit 2; }
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
begin_numa_transaction
CALIBRATION_COPY=
CALIBRATION_SHA256=
CALIBRATION_MANIFEST=
CALIBRATION_MANIFEST_COPY=
CALIBRATION_MANIFEST_SHA256=
if [[ "$MODE" == all || "$MODE" == awavma ]]; then
    CALIBRATION_COPY="$RUN_DIR/metadata/calibration.csv"
    CALIBRATION_MANIFEST="$(dirname -- "$CALIBRATION_FILE")/manifest.json"
    [[ -f "$CALIBRATION_MANIFEST" && -r "$CALIBRATION_MANIFEST" ]] || { printf 'calibration production manifest must be readable beside calibration CSV\n' >&2; exit 2; }
    CALIBRATION_MANIFEST_COPY="$RUN_DIR/metadata/calibration-manifest.json"
    [[ -x "$ROOT/bin/calibration-validate" ]] || make -C "$ROOT" calibration-validate >>"$RUN_DIR/metadata/build.log" 2>&1
    "$ROOT/bin/calibration-validate" "$CALIBRATION_FILE" >"$RUN_DIR/metadata/calibration-validation.txt" 2>&1 || {
        printf 'calibration validation failed\n' >&2
        exit 1
    }
    cp -- "$CALIBRATION_FILE" "$CALIBRATION_COPY"
    cp -- "$CALIBRATION_MANIFEST" "$CALIBRATION_MANIFEST_COPY"
    cmp -s -- "$CALIBRATION_FILE" "$CALIBRATION_COPY" || { printf 'run-local calibration copy differs from source\n' >&2; exit 1; }
    cmp -s -- "$CALIBRATION_MANIFEST" "$CALIBRATION_MANIFEST_COPY" || { printf 'run-local calibration manifest differs from source\n' >&2; exit 1; }
    CALIBRATION_SHA256=$(sha256sum "$CALIBRATION_COPY" | awk '{print $1}')
    printf '%s  %s\n' "$CALIBRATION_SHA256" 'calibration.csv' >"$RUN_DIR/metadata/calibration.sha256"
    CALIBRATION_MANIFEST_SHA256=$(sha256sum "$CALIBRATION_MANIFEST_COPY" | awk '{print $1}')
    printf '%s  %s\n' "$CALIBRATION_MANIFEST_SHA256" 'calibration-manifest.json' >"$RUN_DIR/metadata/calibration-manifest.sha256"
fi
numactl --hardware >"$RUN_DIR/metadata/numa_topology.txt"
TOPOLOGY_SHA256=$(sha256sum "$RUN_DIR/metadata/numa_topology.txt" | awk '{print $1}')
NUMA_DISTANCE=$(numactl --hardware | awk -v local="$LOCAL_NODE" -v remote="$REMOTE_NODE" '$1 == "node" && $2 ~ /^[0-9]+$/ && $3 !~ /cpus:/ {for (i=2; i<=NF; i++) if ($i == remote) column=i} $1 == local ":" && column {print $column; exit}')
[[ "$NUMA_DISTANCE" =~ ^[0-9]+$ ]] || limited 'NUMA distance matrix is unavailable'
uname -a >"$RUN_DIR/metadata/uname.txt"
locale >"$RUN_DIR/metadata/locale.txt"
(gcc --version || cc --version) >"$RUN_DIR/metadata/compiler.txt" 2>&1
git -C "$ROOT" rev-parse HEAD >"$RUN_DIR/metadata/git_revision.txt" 2>/dev/null || true
git -C "$ROOT" status --short >"$RUN_DIR/metadata/git_status.txt" 2>/dev/null || true
    printf '%s\n' 'schema_version,experiment_id,timestamp_utc,run_id,scenario,workload,repetition,thread_node,memory_node,runtime_enabled,requested_execution_mode,effective_execution_mode,activation_state,activation_reason,migration_safety_effective,migration_execution_effective,page_registration_effective,page_registration_ttl_ms,registration_required,registration_acknowledged,registration_status,benchmark_status,runtime_status,initial_placement_status,final_placement_status,placement_mode,numa_distance,initial_total_pages,initial_queryable_pages,initial_local_pages,initial_remote_pages,initial_other_pages,initial_unknown_pages,initial_expected_node_ratio,memory_policy_restored,numa_nodes,thread_migration_ready,page_migration_ready,phase7_ready,production_real_migration_ready,threads,memory_mb,duration_seconds,elapsed_seconds,exit_code,status' >"$RUN_DIR/measurements.csv"
    printf '%s\n' 'scenario,repetition,timestamp,pattern,threads,memory_mb,iterations,duration_sec,thread_node,memory_node,numa_nodes,operations,execution_time_sec,throughput_ops_sec' >"$RUN_DIR/benchmark_results.csv"

run_one() {
    local scenario=$1 repetition=$2 directory runtime=false code=0 runtime_code=0 start end elapsed status=FAILED placement_mode=default placement_path placement_fields=
    local thread_node= memory_node= final_placement_path final_placement_fields=
    local requested_mode=MONITORING effective_mode=MONITORING activation_state=NOT_APPLICABLE
    local activation_reason=baseline migration_safety=false migration_execution=false page_registration=false
    local registration_ttl= registration_required=false registration_acknowledged=false registration_status=NOT_REQUIRED benchmark_status=FAILED runtime_status=NOT_REQUIRED initial_placement_status=FAILED final_placement_status=NOT_CAPTURED numa_nodes= thread_ready= page_ready= phase7_ready= production_ready= distance= total_pages= queryable_pages= local_pages= remote_pages= other_pages= unknown_pages= expected_ratio= placement_status= policy_restored=
    case "$scenario" in
        baseline-default) directory="$RUN_DIR/baseline" ;;
        baseline-local) directory="$RUN_DIR/baseline"; placement_mode=local ;;
        baseline-remote) directory="$RUN_DIR/baseline"; placement_mode=remote ;;
        awavma) directory="$RUN_DIR/awavma"; placement_mode=remote; runtime=true ;;
    esac
    placement_path="$directory/placement-$scenario-$repetition.csv"
    final_placement_path="${placement_path%.csv}.final.csv"
    start=$(date +%s%N)
    if [[ "$scenario" == awavma ]]; then
        local runtime_root="$RUN_DIR/awavma/runtime/$repetition"
        local profile_path="$runtime_root/runtime_execution_profile.csv"
        [[ ! -e "$runtime_root" ]] || {
            printf 'runtime root already exists: %s\n' "$runtime_root" >&2
            return 1
        }
        registration_required=true
        registration_status=PENDING
        cmp -s -- "$CALIBRATION_FILE" "$CALIBRATION_COPY" || {
            printf 'calibration changed after validation\n' >&2
            return 1
        }
        cmp -s -- "$CALIBRATION_MANIFEST" "$CALIBRATION_MANIFEST_COPY" || { printf 'calibration manifest changed after validation\n' >&2; return 1; }
        (
            local registration_socket= worker_evidence_socket=
            for _ in $(seq 1 300); do
                if [[ -f "$profile_path" ]] &&
                   "$ROOT/scripts/verify_runtime_execution_profile.py" --profile "$profile_path" >/dev/null 2>>"$directory/logs/$scenario-$repetition.runtime.log" &&
                   registration_socket=$("$ROOT/scripts/verify_runtime_execution_profile.py" --profile "$profile_path" --emit-field page_registration_socket 2>>"$directory/logs/$scenario-$repetition.runtime.log") &&
                   worker_evidence_socket=$("$ROOT/scripts/verify_runtime_execution_profile.py" --profile "$profile_path" --emit-field worker_evidence_socket 2>>"$directory/logs/$scenario-$repetition.runtime.log"); then
                    exec "$ROOT/bin/benchmark" --threads "$THREADS" --memory "$MEMORY_MB" --duration "$DURATION_SECONDS" --pattern "$WORKLOAD" --placement-mode "$placement_mode" --placement-evidence "$placement_path" --output "$directory/benchmark-$scenario-$repetition.csv" --page-registration-required --page-registration-socket "$registration_socket" --page-registration-timeout-ms 30000 --worker-evidence-socket "$worker_evidence_socket"
                fi
                sleep 0.1
            done
            printf 'timed out waiting for runtime socket metadata\n' >&2
            exit 1
        ) >"$directory/logs/$scenario-$repetition.benchmark.log" 2>&1 &
        local benchmark_pid=$!
        # These names are kept together so a concurrent runtime CLI change is trivial to reconcile.
        local controlled_args=(--controlled-workload-pattern "$WORKLOAD" --controlled-workload-threads "$THREADS" --controlled-workload-memory-bytes "$((MEMORY_MB * 1024 * 1024))" --controlled-workload-duration-seconds "$DURATION_SECONDS")
        "$ROOT/bin/awavma-runtime" --production-real-migration -S -M -R --calibration-artifact "$CALIBRATION_COPY" --calibration-manifest "$CALIBRATION_MANIFEST_COPY" "${controlled_args[@]}" \
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
            local provider_ready=
            IFS=, read -r requested_mode effective_mode migration_safety migration_execution page_registration registration_ttl numa_nodes thread_ready page_ready phase7_ready production_ready activation_state activation_reason provider_ready <<<"$profile_fields"
            cp "$profile_path" "$RUN_DIR/metadata/awavma-runtime-profile-$repetition.csv"
            wait "$benchmark_pid" || code=$?
            [[ "$code" == 0 ]] && benchmark_status=PASS
            if [[ "$code" == 0 ]]; then
                registration_acknowledged=true
                registration_status=ACCEPTED
            else
                registration_status=BENCHMARK_REGISTRATION_FAILED
            fi
            if [[ "$code" == 0 ]] && ! placement_fields=$(validate_placement "$placement_path.start" "$placement_mode"); then
                code=1
            fi
            if [[ "$code" == 0 ]] && ! final_placement_fields=$(validate_placement "$placement_path" "$placement_mode"); then code=1; fi
            local remote_initial="$RUN_DIR/baseline/placement-baseline-remote-$repetition.start.csv"
            [[ "$MODE" == awavma ]] && remote_initial="$REFERENCE_RUN/baseline/placement-baseline-remote-$repetition.start.csv"
            if [[ "$code" == 0 ]] && ! validate_remote_equivalence "$remote_initial" "$placement_path.start"; then
                code=1
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
        [[ "$runtime_code" == 0 && "$activation_state" == ACTIVE ]] && runtime_status=PASS || runtime_status=FAILED
    else
        "$ROOT/bin/benchmark" --threads "$THREADS" --memory "$MEMORY_MB" --duration "$DURATION_SECONDS" --pattern "$WORKLOAD" --placement-mode "$placement_mode" --placement-evidence "$placement_path" --output "$directory/benchmark-$scenario-$repetition.csv" >"$directory/logs/$scenario-$repetition.benchmark.log" 2>&1 || code=$?
        [[ "$code" == 0 ]] && benchmark_status=PASS
        if [[ "$code" == 0 ]] && ! placement_fields=$(validate_placement "$placement_path.start" "$placement_mode"); then
            code=1
        fi
        if [[ "$code" == 0 ]] && ! final_placement_fields=$(validate_placement "$placement_path" "$placement_mode"); then code=1; fi
    fi
    end=$(date +%s%N)
    elapsed=$(printf '%d.%09d' $(((end - start) / 1000000000)) $(((end - start) % 1000000000)))
    [[ "$code" == 0 ]] && status=MEASURED
    if [[ -n "$placement_fields" ]]; then IFS=, read -r placement_mode thread_node memory_node distance total_pages queryable_pages local_pages remote_pages other_pages unknown_pages expected_ratio placement_status policy_restored <<<"$placement_fields"; fi
    initial_placement_status=$placement_status
    if [[ -n "$final_placement_fields" ]]; then
        local final_mode final_thread final_memory final_distance final_total final_queryable final_local final_remote final_other final_unknown final_ratio final_status final_restored
        IFS=, read -r final_mode final_thread final_memory final_distance final_total final_queryable final_local final_remote final_other final_unknown final_ratio final_status final_restored <<<"$final_placement_fields"
        final_placement_status=$final_status
    fi
    [[ -f "$placement_path.start" ]] && mv "$placement_path.start" "${placement_path%.csv}.start.csv"
    [[ -f "$placement_path" ]] && mv "$placement_path" "$final_placement_path"
    if [[ -f "$directory/benchmark-$scenario-$repetition.csv" ]]; then
        tail -n +2 "$directory/benchmark-$scenario-$repetition.csv" | while IFS= read -r line; do printf '%s,%s,%s\n' "$scenario" "$repetition" "$line"; done >>"$RUN_DIR/benchmark_results.csv"
    fi
    printf '%s\n' "8,$EXPERIMENT_ID,$(date -u +%Y-%m-%dT%H:%M:%SZ),$EXPERIMENT_ID-$scenario-$repetition,$scenario,$WORKLOAD,$repetition,$thread_node,$memory_node,$runtime,$requested_mode,$effective_mode,$activation_state,$activation_reason,$migration_safety,$migration_execution,$page_registration,$registration_ttl,$registration_required,$registration_acknowledged,$registration_status,$benchmark_status,$runtime_status,$initial_placement_status,$final_placement_status,$placement_mode,$distance,$total_pages,$queryable_pages,$local_pages,$remote_pages,$other_pages,$unknown_pages,$expected_ratio,$policy_restored,$numa_nodes,$thread_ready,$page_ready,$phase7_ready,$production_ready,$THREADS,$MEMORY_MB,$DURATION_SECONDS,$elapsed,$code,$status" >>"$RUN_DIR/measurements.csv"
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
cat >"$RUN_DIR/manifest.json" <<EOF
{"schema_version":8,"run_id":"$EXPERIMENT_ID","data_source":"REAL","collection_status":"IN_PROGRESS","collection_mode":"$MODE","protocol_version":"phase4d-v1","workload":"$WORKLOAD","threads":$THREADS,"memory_bytes":$((MEMORY_MB * 1024 * 1024)),"duration_seconds":$DURATION_SECONDS,"local_node":$LOCAL_NODE,"remote_node":$REMOTE_NODE,"numa_distance":"$NUMA_DISTANCE","numa_balancing_during":"0","topology_sha256":"$TOPOLOGY_SHA256","calibration_sha256":"$CALIBRATION_SHA256","calibration_manifest_sha256":"$CALIBRATION_MANIFEST_SHA256"}
EOF
UNIFIED="$OUTPUT_DIR/unified"
STAGING="$RUN_DIR/publication-staging"
aggregate_args=(--run-dir "$RUN_DIR" --output "$STAGING/unified/cloudlab_experiment_results.csv" --comparison-output "$STAGING/unified/cloudlab_comparator.csv" --summary-dir "$STAGING/unified/summaries")
[[ -z "$REFERENCE_RUN" ]] || aggregate_args+=(--reference-run "$REFERENCE_RUN")
python3 "$ROOT/scripts/aggregate_experiment_results.py" "${aggregate_args[@]}"
[[ "$SKIP_GRAPHS" == true || "$MODE" == baseline ]] || python3 "$ROOT/scripts/generate_multinuma_graphs.py" --allow-staging --input "$STAGING/unified/cloudlab_comparator.csv" --summary-dir "$STAGING/unified/summaries" --summary "$STAGING/graphs/graph_summary.csv" --output-dir "$STAGING/graphs"
restore_numa_balancing
NUMA_BALANCING_RESTORED_VALUE=$NUMA_BALANCING_ORIGINAL
NUMA_BALANCING_ORIGINAL=
trap - EXIT
cat >"$RUN_DIR/manifest.json" <<EOF
{"schema_version":8,"run_id":"$EXPERIMENT_ID","data_source":"REAL","collection_status":"PASS","collection_mode":"$MODE","protocol_version":"phase4d-v1","workload":"$WORKLOAD","threads":$THREADS,"memory_bytes":$((MEMORY_MB * 1024 * 1024)),"duration_seconds":$DURATION_SECONDS,"local_node":$LOCAL_NODE,"remote_node":$REMOTE_NODE,"numa_distance":"$NUMA_DISTANCE","numa_balancing_original":"$NUMA_BALANCING_RESTORED_VALUE","numa_balancing_during":"0","numa_balancing_restore_status":"RESTORED","topology_source":"metadata/numa_topology.txt","topology_sha256":"$TOPOLOGY_SHA256","calibration_artifact":"$([[ -n "$CALIBRATION_COPY" ]] && printf metadata/calibration.csv)","calibration_sha256":"$CALIBRATION_SHA256","calibration_manifest":"$([[ -n "$CALIBRATION_MANIFEST_COPY" ]] && printf metadata/calibration-manifest.json)","calibration_manifest_sha256":"$CALIBRATION_MANIFEST_SHA256","runtime_execution_profile":"$([[ "$MODE" != baseline ]] && printf 'awavma/runtime/*/runtime_execution_profile.csv')","stage_order":"metadata,build,tests,environment-check,preflight,baseline,awavma,aggregate,graphs,manifests"}
EOF
python3 - "$STAGING" "$OUTPUT_DIR" <<'PY'
import csv, os, sys
from pathlib import Path
staging, destination = map(Path, sys.argv[1:])
comparator = staging / "unified/cloudlab_comparator.csv"
if comparator.is_file():
    with comparator.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle); rows = list(reader); fields = reader.fieldnames
    if any(row.get("collection_status") != "IN_PROGRESS" for row in rows):
        raise SystemExit("staged comparator has unexpected publication status")
    for row in rows: row["collection_status"] = "PASS"
    temporary = comparator.with_suffix(".promoting")
    with temporary.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields); writer.writeheader(); writer.writerows(rows)
    os.replace(temporary, comparator)
for source in sorted(staging.rglob("*")):
    if source.is_file():
        target = destination / source.relative_to(staging)
        target.parent.mkdir(parents=True, exist_ok=True)
        os.replace(source, target)
PY
printf 'Phase 4D artifacts: %s\n' "$RUN_DIR"
