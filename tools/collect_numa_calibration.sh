#!/usr/bin/env bash
# Explicit CloudLab-only collector. It never enables AWAVMA migration execution.
set -Eeuo pipefail
MODE=${1:---smoke}; [[ "$MODE" == --smoke || "$MODE" == --full ]] || exit 2
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd); OUT=${P4C_OUTPUT_DIR:-"$ROOT/results/cloudlab_calibration"}
NUMA_BALANCING_PATH=${P4C_NUMA_BALANCING_PATH:-/proc/sys/kernel/numa_balancing}
PAIRS=${P4C_PAIRS:-7}; WARMUPS=2; WORKLOADS=(mixed)
# "local" is a legacy NUMA placement pattern, not an independent access
# distribution. Controlled LOCAL/REMOTE placement supplies that dimension here.
[[ "$MODE" == --smoke ]] || WORKLOADS=(sequential random hot moderate cold mixed changing)
[[ "$MODE" == --smoke ]] || python3 "$ROOT/tools/validate_numa_calibration.py" smoke-gate "${P4C_SMOKE_MANIFEST:-$OUT/smoke/manifest.json}" || { printf 'FULL_REQUIRES_VALID_SCHEMA_2_SMOKE_GATE\n' >&2; exit 2; }
[[ -x "$ROOT/bin/benchmark" ]] || { printf 'ENV_LIMITED: benchmark binary unavailable\n' >&2; exit 3; }
make -C "$ROOT" bin/p4c-migration-cost-collector >/dev/null
[[ "$(uname -s)" == Linux ]] || { printf 'ENV_LIMITED: Linux is required\n' >&2; exit 3; }
command -v numactl >/dev/null || { printf 'ENV_LIMITED: numactl unavailable\n' >&2; exit 3; }
command -v awk >/dev/null || { printf 'PREFLIGHT_ERROR: awk unavailable\n' >&2; exit 2; }
if ! NUMA_HARDWARE=$(numactl --hardware 2>&1); then
  printf 'PREFLIGHT_ERROR: numactl --hardware failed: %s\n' "$NUMA_HARDWARE" >&2
  exit 2
fi
if ! NUMA_NODE_COUNT=$(awk '/^available:[[:space:]]+[0-9]+[[:space:]]+nodes/ { print $2; exit }' <<<"$NUMA_HARDWARE"); then
  printf 'PREFLIGHT_ERROR: unable to parse numactl topology\n' >&2
  exit 2
fi
[[ "$NUMA_NODE_COUNT" =~ ^[0-9]+$ ]] || { printf 'PREFLIGHT_ERROR: malformed numactl topology\n' >&2; exit 2; }
(( NUMA_NODE_COUNT >= 2 )) || { printf 'ENV_LIMITED: fewer than two NUMA nodes\n' >&2; exit 3; }

ORIGINAL_NUMA_BALANCING=; NUMA_BALANCING_DURING=; NUMA_BALANCING_RESTORE_STATUS=NOT_ATTEMPTED
TIMING_VALID=false; COST_VALID=false; PLACEMENT_VALID=false; TRANSACTION_VALID=false; TRANSACTION_BROKEN=false; STRICT_VALID=false; OVERALL_VALID=false; RUN=; EXPERIMENT=
write_manifest() {
  [[ -n "$RUN" ]] || return 0
  local status=NOT_PRODUCTION_CALIBRATION raw_artifacts='[]'
  [[ "$MODE" == --full && "$OVERALL_VALID" == true ]] && status=PRODUCTION_AUTHORITY_CANDIDATE
  # The collector only records regular files below its run directory; builder re-verifies these bytes.
  raw_artifacts=$(python3 - "$RUN" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
entries = []
for path, role in [(root / "raw_timing.csv", "TIMING"), (root / "raw_cost.csv", "COST")]:
    if path.is_file() and not path.is_symlink():
        data = path.read_bytes()
        entries.append({"path": path.relative_to(root).as_posix(), "role": role,
                        "sha256": hashlib.sha256(data).hexdigest(), "size_bytes": len(data)})
for path in sorted((root / "placement").glob("*.csv")):
    if path.is_file() and not path.is_symlink():
        data = path.read_bytes()
        entries.append({"path": path.relative_to(root).as_posix(), "role": "PLACEMENT",
                        "sha256": hashlib.sha256(data).hexdigest(), "size_bytes": len(data)})
print(json.dumps(sorted(entries, key=lambda item: item["path"]), separators=(",", ":")))
PY
)
  printf '{"schema_version":4,"kind":"AWAVMA_P4_CALIBRATION_COLLECTION","collection_experiment_id":"%s","mode":"%s","status":"%s","production_authority":false,"numa_balancing_original":"%s","numa_balancing_during":"%s","numa_balancing_restore_status":"%s","timing_valid":%s,"cost_migration_valid":%s,"placement_validation_valid":%s,"numa_balancing_transaction_valid":%s,"strict_validation_valid":%s,"overall_valid":%s,"git_dirty":%s,"raw_timing":"raw_timing.csv","raw_cost":"raw_cost.csv","raw_artifacts":%s,"thread_calibration":"NOT_IMPLEMENTED","awavma_remote_equivalence":"PENDING"}\n' \
    "$EXPERIMENT" "${MODE#--}" "$status" "$ORIGINAL_NUMA_BALANCING" "$NUMA_BALANCING_DURING" "$NUMA_BALANCING_RESTORE_STATUS" "$TIMING_VALID" "$COST_VALID" "$PLACEMENT_VALID" "$TRANSACTION_VALID" "$STRICT_VALID" "$OVERALL_VALID" "$(git -C "$ROOT" diff --quiet && printf false || printf true)" "$raw_artifacts" >"$RUN/manifest.json"
}
finish() {
  local code=$? restored=
  trap - EXIT HUP INT TERM
  if [[ "$ORIGINAL_NUMA_BALANCING" =~ ^[01]$ ]]; then
    if printf '%s\n' "$ORIGINAL_NUMA_BALANCING" >"$NUMA_BALANCING_PATH" 2>/dev/null; then restored=$(<"$NUMA_BALANCING_PATH") || restored=; fi
    if [[ "$restored" == "$ORIGINAL_NUMA_BALANCING" ]]; then
      NUMA_BALANCING_RESTORE_STATUS=RESTORED
      [[ "$NUMA_BALANCING_DURING" == 0 && "$TRANSACTION_BROKEN" == false ]] && TRANSACTION_VALID=true
    else
      NUMA_BALANCING_RESTORE_STATUS=FAILED; TRANSACTION_VALID=false; code=1
      printf 'NUMA_BALANCING_RESTORE_FAILED\n' >&2
    fi
  fi
  [[ "$TIMING_VALID" == true ]] && PLACEMENT_VALID=true
  if [[ "$TIMING_VALID" == true && "$COST_VALID" == true && "$PLACEMENT_VALID" == true && "$TRANSACTION_VALID" == true ]]; then STRICT_VALID=true; else STRICT_VALID=false; fi
  if [[ "$code" == 0 && "$STRICT_VALID" == true ]]; then OVERALL_VALID=true; else OVERALL_VALID=false; code=1; fi
  write_manifest
  if [[ "$MODE" == --smoke && "$OVERALL_VALID" == true ]]; then
    local temporary="$OUT/smoke/.manifest.$$"; cp "$RUN/manifest.json" "$temporary" && mv -f "$temporary" "$OUT/smoke/manifest.json" || code=1
  fi
  exit "$code"
}
trap finish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
[[ -r "$NUMA_BALANCING_PATH" && -w "$NUMA_BALANCING_PATH" ]] || { printf 'NUMA_BALANCING_CONTROL_UNAVAILABLE: %s\n' "$NUMA_BALANCING_PATH" >&2; exit 2; }
ORIGINAL_NUMA_BALANCING=$(<"$NUMA_BALANCING_PATH")
[[ "$ORIGINAL_NUMA_BALANCING" =~ ^[01]$ ]] || { printf 'NUMA_BALANCING_ORIGINAL_INVALID\n' >&2; exit 2; }
printf '0\n' >"$NUMA_BALANCING_PATH" || { printf 'NUMA_BALANCING_DISABLE_FAILED\n' >&2; exit 2; }
NUMA_BALANCING_DURING=$(<"$NUMA_BALANCING_PATH") || NUMA_BALANCING_DURING=
[[ "$NUMA_BALANCING_DURING" == 0 ]] || { printf 'NUMA_BALANCING_DISABLE_READBACK_FAILED\n' >&2; exit 2; }
require_numa_balancing_disabled() {
  local current=; current=$(<"$NUMA_BALANCING_PATH") || current=
  [[ "$current" == 0 ]] || { TRANSACTION_BROKEN=true; printf 'NUMA_BALANCING_CHANGED_DURING_COLLECTION\n' >&2; return 1; }
}
EXPERIMENT="p4c-$(date -u +%Y%m%dT%H%M%SZ)-$$"; RUN="$OUT/${MODE#--}/$EXPERIMENT"
mkdir -p "$OUT/${MODE#--}"
mkdir "$RUN" || { printf 'COLLECTION_DIRECTORY_EXISTS: %s\n' "$RUN" >&2; exit 2; }
mkdir "$RUN/placement" "$RUN/logs"
printf '%s\n' 'run_id,pair_index,pair_order,warmup,action_kind,benchmark_pattern,placement_mode,threads,memory_bytes,page_size,local_node,remote_node,numa_distance,duration_seconds,elapsed_ms,verification_status,memory_policy_restored,total_pages,queryable_pages,other_pages,unknown_pages,placement_artifact,benchmark_exit_status,measurement_valid,invalid_reason' >"$RUN/raw_timing.csv"
printf '%s\n' 'run_id,warmup,requested_pages,attempted_pages,migrated_pages,failed_pages,elapsed_ms,source_node,destination_node,distance,page_size,measurement_valid,failure_reason' >"$RUN/raw_cost.csv"
printf 'NOT_PRODUCTION_CALIBRATION\n' >"$RUN/status.txt"
# P3 chooses deterministic permitted nodes; each row is accepted only after strict evidence validation.
MEASURED_PAIRS=$PAIRS; [[ "$MODE" == --smoke ]] && MEASURED_PAIRS=1
for workload in "${WORKLOADS[@]}"; do
  for warmup in true false; do
   RUNS=$MEASURED_PAIRS; [[ "$warmup" == true ]] && RUNS=$WARMUPS
   for pair in $(seq 1 "$RUNS"); do
    for placement in $([[ $((pair % 2)) == 1 ]] && printf 'local remote' || printf 'remote local'); do
      require_numa_balancing_disabled
      artifact="$RUN/placement/$workload-$pair-$placement.csv"; log="$RUN/logs/$workload-$pair-$placement.log"; start=$(date +%s%N); code=0
      "$ROOT/bin/benchmark" --threads "${P4C_THREADS:-2}" --memory "${P4C_MEMORY_MB:-1024}" --duration "${P4C_DURATION_SECONDS:-30}" --pattern "$workload" --placement-mode "$placement" --placement-evidence "$artifact" >"$log" 2>&1 || code=$?
      require_numa_balancing_disabled
      elapsed=$((($(date +%s%N)-start)/1000000)); valid=false; reason=BENCHMARK_FAILED; status=
      if [[ "$code" == 0 ]] && python3 "$ROOT/tools/validate_numa_calibration.py" placement "$artifact" "$placement"; then valid=true; reason=; status=PASS; else status=FAIL; fi
      # The source artifact is retained; its strict fields are copied only after validation.
      python3 "$ROOT/tools/validate_numa_calibration.py" raw-row "$artifact" "$EXPERIMENT" "$pair" "$workload" "$placement" "${P4C_THREADS:-2}" "$(( ${P4C_MEMORY_MB:-1024} * 1024 * 1024 ))" "${P4C_DURATION_SECONDS:-30}" "$elapsed" "$code" "$valid" "$reason" "$warmup" >>"$RUN/raw_timing.csv" || true
    done
  done
  done
done
# A collector invocation owns one allocation and appends one retained row. Invalid rows never count.
MAX_COST_ATTEMPTS=${P4C_COST_MAX_ATTEMPTS:-27}
collect_costs() {
  local warmup=$1 required=$2 successful=0 attempts=0
  while [[ $successful -lt $required && $attempts -lt $MAX_COST_ATTEMPTS ]]; do
    require_numa_balancing_disabled || return 1
    attempts=$((attempts + 1))
    if "$ROOT/bin/p4c-migration-cost-collector" --output "$RUN/raw_cost.csv" --run-id "$EXPERIMENT-cost-$warmup-$attempts" --warmup "$warmup" >>"$RUN/logs/cost-$warmup-$attempts.log" 2>&1; then
      successful=$((successful + 1))
    fi
    require_numa_balancing_disabled || return 1
  done
  [[ $successful -eq $required ]]
}
if [[ "$MODE" == --smoke ]]; then
  collect_costs true 1 || printf 'SMOKE_COST_SAMPLE_INVALID\n' >&2
else
  collect_costs true "$WARMUPS" || printf 'FULL_COST_WARMUPS_INSUFFICIENT\n' >&2
  collect_costs false 7 || printf 'FULL_COST_SAMPLES_INSUFFICIENT\n' >&2
fi
if python3 "$ROOT/tools/validate_numa_calibration.py" cost-valid "$RUN/raw_cost.csv" "${MODE#--}"; then COST_VALID=true; else printf 'COST_EVIDENCE_INVALID\n' >&2; fi
if python3 "$ROOT/tools/validate_numa_calibration.py" timing-valid "$RUN/raw_timing.csv" "${MODE#--}"; then TIMING_VALID=true; else printf 'TIMING_EVIDENCE_INVALID\n' >&2; fi
printf 'P4-C collection retained at %s; raw_cost.csv contains retained controlled migration-cost attempts.\n' "$RUN"
[[ "$TIMING_VALID" == true && "$COST_VALID" == true ]]
