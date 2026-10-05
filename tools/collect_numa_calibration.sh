#!/usr/bin/env bash
# Explicit CloudLab-only collector. It never enables AWAVMA migration execution.
set -Eeuo pipefail
MODE=${1:---smoke}; [[ "$MODE" == --smoke || "$MODE" == --full ]] || exit 2
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd); OUT=${P4C_OUTPUT_DIR:-"$ROOT/results/cloudlab_calibration"}
PAIRS=${P4C_PAIRS:-7}; WARMUPS=2; WORKLOADS=(mixed)
[[ "$MODE" == --smoke ]] || WORKLOADS=(sequential random hot moderate cold mixed changing local)
[[ "$MODE" == --smoke ]] || [[ -f "${P4C_SMOKE_MANIFEST:-$OUT/smoke/manifest.json}" ]] || { printf 'FULL_REQUIRES_SUCCESSFUL_SMOKE_MANIFEST\n' >&2; exit 2; }
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
EXPERIMENT="p4c-$(date -u +%Y%m%dT%H%M%SZ)-$$"; RUN="$OUT/${MODE#--}/$EXPERIMENT"; mkdir -p "$RUN/placement" "$RUN/logs"
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
      artifact="$RUN/placement/$workload-$pair-$placement.csv"; log="$RUN/logs/$workload-$pair-$placement.log"; start=$(date +%s%N); code=0
      "$ROOT/bin/benchmark" --threads "${P4C_THREADS:-2}" --memory "${P4C_MEMORY_MB:-1024}" --duration "${P4C_DURATION_SECONDS:-30}" --pattern "$workload" --placement-mode "$placement" --placement-evidence "$artifact" >"$log" 2>&1 || code=$?
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
    attempts=$((attempts + 1))
    if "$ROOT/bin/p4c-migration-cost-collector" --output "$RUN/raw_cost.csv" --run-id "$EXPERIMENT-cost-$warmup-$attempts" --warmup "$warmup" >>"$RUN/logs/cost-$warmup-$attempts.log" 2>&1; then
      successful=$((successful + 1))
    fi
  done
  [[ $successful -eq $required ]]
}
SMOKE_COST_VALID=false; FULL_COST_VALID=true
if [[ "$MODE" == --smoke ]]; then
  if collect_costs true 1; then SMOKE_COST_VALID=true; else printf 'SMOKE_COST_SAMPLE_INVALID\n' >&2; fi
else
  collect_costs true "$WARMUPS" || { printf 'FULL_COST_WARMUPS_INSUFFICIENT\n' >&2; FULL_COST_VALID=false; }
  collect_costs false 7 || { printf 'FULL_COST_SAMPLES_INSUFFICIENT\n' >&2; FULL_COST_VALID=false; }
fi
printf '{"schema_version":1,"mode":"%s","status":"NOT_PRODUCTION_CALIBRATION","cost_migration_valid":%s,"git_dirty":%s,"raw_timing":"raw_timing.csv","raw_cost":"raw_cost.csv","thread_calibration":"NOT_IMPLEMENTED","awavma_remote_equivalence":"PENDING"}\n' "${MODE#--}" "$SMOKE_COST_VALID" "$(git -C "$ROOT" diff --quiet && printf false || printf true)" >"$RUN/manifest.json"
if [[ "$MODE" == --smoke && "$SMOKE_COST_VALID" == true ]]; then
  temporary="$OUT/smoke/.manifest.$$"; cp "$RUN/manifest.json" "$temporary"; mv -f "$temporary" "$OUT/smoke/manifest.json"
fi
printf 'P4-C collection retained at %s; raw_cost.csv contains retained controlled migration-cost attempts.\n' "$RUN"
[[ "$MODE" == --smoke || "$FULL_COST_VALID" == true ]]
