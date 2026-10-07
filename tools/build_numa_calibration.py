#!/usr/bin/env python3
"""Build strict P4-B calibration CSVs from safe, retained CloudLab raw rows."""
import argparse, csv, hashlib, math, os, statistics, sys, tempfile
from collections import defaultdict
from validate_numa_calibration import valid_cost

SCHEMA = 1
MIN_SAMPLES = 7
HEADER = "schema_version,calibration_version,calibration_id,created_at_utc,collection_experiment_id,calibration_status,cpu_architecture,cpu_model,online_numa_nodes,topology_fingerprint,local_node,remote_node,numa_distance,local_permitted_cpu_count,page_size_bytes,benchmark_pattern,threads,memory_bytes,memory_pages,duration_seconds,action_kind,source_node,destination_node,migration_page_bucket,valid_pair_count,local_mean_ms,local_stddev_ms,remote_mean_ms,remote_stddev_ms,paired_penalty_mean_ms,paired_penalty_lower_bound_ms,expected_recoverable_gain_pct,uncertainty_pct,safety_margin_pct,cost_sample_count,migration_cost_mean_ms,migration_cost_stddev_ms,migration_cost_conservative_ms,estimated_cost_pct,successful_pages,failed_pages,placement_evidence_schema_version,local_placement_artifact_hash,remote_placement_artifact_hash,migration_measurement_method,rejection_reason".split(",")
T95_ONE_SIDED = {2: 6.314, 3: 2.92, 4: 2.353, 5: 2.132, 6: 2.015, 7: 1.943, 8: 1.895, 9: 1.86, 10: 1.833, 11: 1.812, 12: 1.796, 13: 1.782, 14: 1.771, 15: 1.761, 16: 1.753, 17: 1.746, 18: 1.74, 19: 1.734, 20: 1.729}

def hash_text(h, text):
    """Byte-for-byte equivalent of calibration.c hash_text()."""
    data = text.encode()
    length = len(data)
    for shift in range(0, 64, 8):
        h = ((h ^ ((length >> shift) & 255)) * 1099511628211) & ((1 << 64) - 1)
    for byte in data:
        h = ((h ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return h
def fnv(text, domain):
    h = 1469598103934665603
    return hash_text(hash_text(h, domain), text)

def token(prefix, text): return f"{prefix}-{fnv(text, 'AWAVMA:topology:v1'):016x}"
def mean_std(values): return statistics.mean(values), statistics.stdev(values) if len(values) > 1 else 0.0
def t_bound(mean, std, count, upper=False):
    return mean + (1 if upper else -1) * T95_ONE_SIDED.get(count, 1.645) * std / math.sqrt(count)
def artifact_ref(path):
    with open(path, "rb") as handle: return "art1-" + hashlib.sha256(handle.read()).hexdigest()[:16]
def atomic_csv(path, rows):
    directory = os.path.dirname(os.path.abspath(path)); os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".calibration-", dir=directory, text=True)
    with os.fdopen(fd, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=HEADER, lineterminator="\n"); writer.writeheader(); writer.writerows(rows); handle.flush(); os.fsync(handle.fileno())
    os.replace(temporary, path)
def valid_placement(row, mode):
    required = ("placement_mode", "verification_status", "memory_policy_restored", "total_pages", "queryable_pages", "other_pages", "unknown_pages")
    return all(row.get(k, "") for k in required) and row["placement_mode"] == mode and row["verification_status"] == "PASS" and row["memory_policy_restored"] == "true" and row["total_pages"] == row["queryable_pages"] and row["other_pages"] == "0" and row["unknown_pages"] == "0"
def build(args):
    with open(args.raw, newline="", encoding="utf-8") as handle: raw = list(csv.DictReader(handle))
    with open(args.cost, newline="", encoding="utf-8") as handle: costs = list(csv.DictReader(handle))
    groups = defaultdict(list)
    for row in raw:
        if row.get("warmup") != "false" or row.get("measurement_valid") != "true": continue
        key = tuple(row.get(k, "") for k in ("benchmark_pattern", "threads", "memory_bytes", "page_size", "local_node", "remote_node", "numa_distance", "duration_seconds"))
        groups[key].append(row)
    valid_costs = [r for r in costs if r.get("warmup") == "false" and valid_cost(r)]
    if len(valid_costs) < args.minimum_cost_samples: raise SystemExit("CALIBRATION_INSUFFICIENT_COST_SAMPLES")
    cost_values = [float(r["elapsed_ms"]) for r in valid_costs]; cost_mean, cost_std = mean_std(cost_values); cost_upper = t_bound(cost_mean, cost_std, len(cost_values), True)
    records = []
    for key, rows in sorted(groups.items()):
        local = {r["pair_index"]: r for r in rows if r.get("placement_mode") == "local" and valid_placement(r, "local")}
        remote = {r["pair_index"]: r for r in rows if r.get("placement_mode") == "remote" and valid_placement(r, "remote")}
        paired = [(local[p], remote[p]) for p in sorted(set(local) & set(remote))]
        if len(paired) < args.minimum_pairs: raise SystemExit("CALIBRATION_INSUFFICIENT_PAIRS")
        local_values = [float(a["elapsed_ms"]) for a, _ in paired]; remote_values = [float(b["elapsed_ms"]) for _, b in paired]
        differences = [b - a for a, b in zip(local_values, remote_values)]
        local_mean, local_std = mean_std(local_values); remote_mean, remote_std = mean_std(remote_values); penalty, penalty_std = mean_std(differences); lower = t_bound(penalty, penalty_std, len(differences))
        if lower <= 0: raise SystemExit("CALIBRATION_WEAK_EVIDENCE")
        pattern, threads, memory_bytes, page_size, local_node, remote_node, distance, duration = key
        topology_text = "|".join((args.cpu_architecture, args.cpu_model, args.online_numa_nodes, local_node, remote_node, distance, args.local_permitted_cpu_count, page_size))
        topology = token("top1", topology_text)
        record = dict(zip(HEADER, [""] * len(HEADER)))
        record.update(schema_version="1", calibration_version="p4c-v1", created_at_utc=args.created_at_utc, collection_experiment_id=args.experiment_id, calibration_status="VALIDATED_PRODUCTION" if args.production else "VALIDATED_TEST_ONLY", cpu_architecture=args.cpu_architecture, cpu_model=args.cpu_model, online_numa_nodes=args.online_numa_nodes, topology_fingerprint=topology, local_node=local_node, remote_node=remote_node, numa_distance=distance, local_permitted_cpu_count=args.local_permitted_cpu_count, page_size_bytes=page_size, benchmark_pattern=pattern, threads=threads, memory_bytes=memory_bytes, memory_pages=str(int(memory_bytes) // int(page_size)), duration_seconds=duration, action_kind="MOVE_MEMORY", source_node=remote_node, destination_node=local_node, migration_page_bucket="4096", valid_pair_count=str(len(paired)), local_mean_ms=f"{local_mean:.9g}", local_stddev_ms=f"{local_std:.9g}", remote_mean_ms=f"{remote_mean:.9g}", remote_stddev_ms=f"{remote_std:.9g}", paired_penalty_mean_ms=f"{penalty:.9g}", paired_penalty_lower_bound_ms=f"{lower:.9g}", expected_recoverable_gain_pct=f"{lower / remote_mean * 100:.9g}", uncertainty_pct="0", safety_margin_pct=f"{args.safety_margin_pct:.9g}", cost_sample_count=str(len(cost_values)), migration_cost_mean_ms=f"{cost_mean:.9g}", migration_cost_stddev_ms=f"{cost_std:.9g}", migration_cost_conservative_ms=f"{cost_upper:.9g}", estimated_cost_pct=f"{cost_upper / remote_mean * 100:.9g}", successful_pages="4096", failed_pages="0", placement_evidence_schema_version="1", local_placement_artifact_hash=artifact_ref(paired[0][0]["placement_artifact"]), remote_placement_artifact_hash=artifact_ref(paired[0][1]["placement_artifact"]), migration_measurement_method="move_pages", rejection_reason="none")
        canonical = "|".join((record["schema_version"], record["calibration_version"], record["created_at_utc"], record["collection_experiment_id"], topology, record["benchmark_pattern"], record["action_kind"], record["threads"], record["memory_bytes"], record["memory_pages"], record["duration_seconds"], record["source_node"], record["destination_node"], record["migration_page_bucket"], record["valid_pair_count"], record["local_mean_ms"], record["local_stddev_ms"], record["remote_mean_ms"], record["remote_stddev_ms"], record["paired_penalty_mean_ms"], record["paired_penalty_lower_bound_ms"], record["expected_recoverable_gain_pct"], record["uncertainty_pct"], record["safety_margin_pct"], record["cost_sample_count"], record["migration_cost_mean_ms"], record["migration_cost_stddev_ms"], record["migration_cost_conservative_ms"], record["estimated_cost_pct"], record["successful_pages"], record["failed_pages"], record["local_placement_artifact_hash"], record["remote_placement_artifact_hash"], record["migration_measurement_method"]))
        record["calibration_id"] = f"cal1-{fnv(canonical, 'AWAVMA:calibration:record:v1'):016x}"
        records.append(record)
    atomic_csv(args.output, records)
    print(args.output)
def main():
    parser = argparse.ArgumentParser(); parser.add_argument("--raw", required=True); parser.add_argument("--cost", required=True); parser.add_argument("--output", required=True); parser.add_argument("--experiment-id", required=True); parser.add_argument("--created-at-utc", required=True); parser.add_argument("--cpu-architecture", required=True); parser.add_argument("--cpu-model", required=True); parser.add_argument("--online-numa-nodes", required=True); parser.add_argument("--local-permitted-cpu-count", required=True); parser.add_argument("--minimum-pairs", type=int, default=MIN_SAMPLES); parser.add_argument("--minimum-cost-samples", type=int, default=MIN_SAMPLES); parser.add_argument("--safety-margin-pct", type=float, default=1.0); parser.add_argument("--production", action="store_true"); args = parser.parse_args();
    if args.minimum_pairs < MIN_SAMPLES or args.minimum_cost_samples < MIN_SAMPLES or not 0 <= args.safety_margin_pct <= 100: raise SystemExit("invalid calibration policy")
    build(args)
if __name__ == "__main__": main()
