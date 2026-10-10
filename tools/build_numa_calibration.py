#!/usr/bin/env python3
"""Build strict P4-B calibration CSVs from safe, retained CloudLab raw rows."""
import argparse, csv, ctypes, errno, hashlib, json, math, os, shutil, statistics, sys, tempfile
from collections import defaultdict
from pathlib import Path
from calibration_manifest_trust import (AUTHORITY_KIND, CALIBRATION_VERSION,
    SCHEMA_VERSION as MANIFEST_SCHEMA, VALIDATION_FIELDS, read_manifest,
    validate_collection_manifest, verify_raw_artifacts)
from validate_numa_calibration import timing_metrics, valid_cost

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
def write_csv(path, rows):
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=HEADER, lineterminator="\n"); writer.writeheader(); writer.writerows(rows); handle.flush(); os.fsync(handle.fileno())
def publish_bundle(staging, bundle):
    # renameat2's NOREPLACE flag makes the directory publication atomic without replacing a collision.
    renameat2 = getattr(ctypes.CDLL(None, use_errno=True), "renameat2", None)
    if renameat2 is None: raise SystemExit("CALIBRATION_BUNDLE_ATOMIC_PUBLISH_UNAVAILABLE")
    renameat2.argtypes = (ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint)
    renameat2.restype = ctypes.c_int
    if renameat2(-100, os.fsencode(staging), -100, os.fsencode(bundle), 1):
        error = ctypes.get_errno()
        if error == errno.EEXIST: raise SystemExit("CALIBRATION_BUNDLE_DESTINATION_NOT_NEW")
        raise OSError(error, os.strerror(error), bundle)
    directory = os.open(os.path.dirname(bundle), os.O_RDONLY)
    try: os.fsync(directory)
    finally: os.close(directory)
def valid_placement(row, mode):
    required = ("placement_mode", "verification_status", "memory_policy_restored", "total_pages", "queryable_pages", "other_pages", "unknown_pages")
    return all(row.get(k, "") for k in required) and row["placement_mode"] == mode and row["verification_status"] == "PASS" and row["memory_policy_restored"] == "true" and row["total_pages"] == row["queryable_pages"] and row["other_pages"] == "0" and row["unknown_pages"] == "0"
def valid_timing_row(row, root):
    try:
        artifact = Path(row["benchmark_artifact"]).resolve()
        artifact.relative_to(root)
        metrics = timing_metrics(artifact)
        if not metrics:
            return False
        operations, execution, throughput = metrics
        return (int(row["operations"]) == operations and
                math.isclose(float(row["execution_time_sec"]), execution, rel_tol=1e-12) and
                math.isclose(float(row["throughput_ops_sec"]), throughput, rel_tol=1e-12))
    except (KeyError, ValueError, OSError):
        return False
def build(args):
    collection = read_manifest(args.collection_manifest)
    validate_collection_manifest(collection, args.experiment_id)
    collection_root = Path(args.collection_manifest).resolve().parent
    try:
        raw_relative = Path(args.raw).resolve().relative_to(collection_root).as_posix()
        cost_relative = Path(args.cost).resolve().relative_to(collection_root).as_posix()
    except ValueError as error:
        raise SystemExit("CALIBRATION_RAW_INPUT_OUTSIDE_COLLECTION") from error
    with open(args.raw, newline="", encoding="utf-8") as handle: raw = list(csv.DictReader(handle))
    with open(args.cost, newline="", encoding="utf-8") as handle: costs = list(csv.DictReader(handle))
    if not raw or any(row.get("run_id") != args.experiment_id for row in raw):
        raise SystemExit("CALIBRATION_RAW_EXPERIMENT_MISMATCH")
    if not costs or any(not row.get("run_id", "").startswith(args.experiment_id + "-cost-") for row in costs):
        raise SystemExit("CALIBRATION_COST_EXPERIMENT_MISMATCH")
    if any(row.get("measurement_valid") == "true" and not valid_timing_row(row, collection_root) for row in raw):
        raise SystemExit("CALIBRATION_TIMING_METRICS_INVALID")
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
        if any(not valid_timing_row(row, collection_root) for row in rows):
            raise SystemExit("CALIBRATION_TIMING_METRICS_INVALID")
        local = {r["pair_index"]: r for r in rows if r.get("placement_mode") == "local" and valid_placement(r, "local")}
        remote = {r["pair_index"]: r for r in rows if r.get("placement_mode") == "remote" and valid_placement(r, "remote")}
        paired = [(local[p], remote[p]) for p in sorted(set(local) & set(remote))]
        if len(paired) < args.minimum_pairs: raise SystemExit("CALIBRATION_INSUFFICIENT_PAIRS")
        local_values = [float(a["execution_time_sec"]) * 1000 for a, _ in paired]; remote_values = [float(b["execution_time_sec"]) * 1000 for _, b in paired]
        local_throughput = [float(a["throughput_ops_sec"]) for a, _ in paired]; remote_throughput = [float(b["throughput_ops_sec"]) for _, b in paired]
        differences = [b - a for a, b in zip(local_values, remote_values)]
        gains = [(local - remote) / remote * 100 for local, remote in zip(local_throughput, remote_throughput)]
        local_mean, local_std = mean_std(local_values); remote_mean, remote_std = mean_std(remote_values); penalty, penalty_std = mean_std(differences); lower = t_bound(penalty, penalty_std, len(differences)); gain_mean, gain_std = mean_std(gains); gain_lower = t_bound(gain_mean, gain_std, len(gains))
        if gain_lower <= 0: raise SystemExit("CALIBRATION_WEAK_EVIDENCE")
        pattern, threads, memory_bytes, page_size, local_node, remote_node, distance, duration = key
        if any((r["source_node"], r["destination_node"], r["distance"], r["page_size"]) !=
               (remote_node, local_node, distance, page_size) for r in valid_costs):
            raise SystemExit("CALIBRATION_COST_DIRECTION_MISMATCH")
        topology_text = "|".join((args.cpu_architecture, args.cpu_model, args.online_numa_nodes, local_node, remote_node, distance, args.local_permitted_cpu_count, page_size))
        topology = token("top1", topology_text)
        record = dict(zip(HEADER, [""] * len(HEADER)))
        record.update(schema_version="2", calibration_version=CALIBRATION_VERSION, created_at_utc=args.created_at_utc, collection_experiment_id=args.experiment_id, calibration_status="VALIDATED_PRODUCTION", cpu_architecture=args.cpu_architecture, cpu_model=args.cpu_model, online_numa_nodes=args.online_numa_nodes, topology_fingerprint=topology, local_node=local_node, remote_node=remote_node, numa_distance=distance, local_permitted_cpu_count=args.local_permitted_cpu_count, page_size_bytes=page_size, benchmark_pattern=pattern, threads=threads, memory_bytes=memory_bytes, memory_pages=str(int(memory_bytes) // int(page_size)), duration_seconds=duration, action_kind="MOVE_MEMORY", source_node=remote_node, destination_node=local_node, migration_page_bucket="4096", valid_pair_count=str(len(paired)), local_mean_ms=f"{local_mean:.9g}", local_stddev_ms=f"{local_std:.9g}", remote_mean_ms=f"{remote_mean:.9g}", remote_stddev_ms=f"{remote_std:.9g}", paired_penalty_mean_ms=f"{penalty:.9g}", paired_penalty_lower_bound_ms=f"{lower:.9g}", expected_recoverable_gain_pct=f"{gain_lower:.9g}", uncertainty_pct="0", safety_margin_pct=f"{args.safety_margin_pct:.9g}", cost_sample_count=str(len(cost_values)), migration_cost_mean_ms=f"{cost_mean:.9g}", migration_cost_stddev_ms=f"{cost_std:.9g}", migration_cost_conservative_ms=f"{cost_upper:.9g}", estimated_cost_pct=f"{cost_upper / 1000 / (remote_mean / 1000) * 100:.9g}", successful_pages="4096", failed_pages="0", placement_evidence_schema_version="1", local_placement_artifact_hash=artifact_ref(paired[0][0]["placement_artifact"]), remote_placement_artifact_hash=artifact_ref(paired[0][1]["placement_artifact"]), migration_measurement_method="move_pages", rejection_reason="none")
        canonical = "|".join((record["schema_version"], record["calibration_version"], record["created_at_utc"], record["collection_experiment_id"], topology, record["benchmark_pattern"], record["action_kind"], record["threads"], record["memory_bytes"], record["memory_pages"], record["duration_seconds"], record["source_node"], record["destination_node"], record["migration_page_bucket"], record["valid_pair_count"], record["local_mean_ms"], record["local_stddev_ms"], record["remote_mean_ms"], record["remote_stddev_ms"], record["paired_penalty_mean_ms"], record["paired_penalty_lower_bound_ms"], record["expected_recoverable_gain_pct"], record["uncertainty_pct"], record["safety_margin_pct"], record["cost_sample_count"], record["migration_cost_mean_ms"], record["migration_cost_stddev_ms"], record["migration_cost_conservative_ms"], record["estimated_cost_pct"], record["successful_pages"], record["failed_pages"], record["local_placement_artifact_hash"], record["remote_placement_artifact_hash"], record["migration_measurement_method"]))
        record["calibration_id"] = f"cal1-{fnv(canonical, 'AWAVMA:calibration:record:v1'):016x}"
        records.append(record)
    if not records: raise SystemExit("CALIBRATION_EMPTY")
    required_raw = {raw_relative: "TIMING", cost_relative: "COST"}
    for rows in groups.values():
        for row in rows:
            if row.get("warmup") == "false" and row.get("measurement_valid") == "true" and row.get("placement_artifact"):
                try:
                    relative = Path(row["placement_artifact"]).resolve().relative_to(collection_root).as_posix()
                except ValueError as error:
                    raise SystemExit("CALIBRATION_PLACEMENT_OUTSIDE_COLLECTION") from error
                required_raw[relative] = "PLACEMENT"
                try:
                    relative = Path(row["benchmark_artifact"]).resolve().relative_to(collection_root).as_posix()
                except (KeyError, ValueError) as error:
                    raise SystemExit("CALIBRATION_BENCHMARK_OUTSIDE_COLLECTION") from error
                required_raw[relative] = "BENCHMARK"
    try:
        verify_raw_artifacts(collection, collection_root, required_raw)
    except Exception as error:
        raise SystemExit(str(error)) from error
    output_path = os.path.abspath(args.output)
    manifest_path = os.path.abspath(args.calibration_manifest)
    bundle = os.path.dirname(output_path)
    if bundle != os.path.dirname(manifest_path) or os.path.basename(output_path) == os.path.basename(manifest_path):
        raise SystemExit("CALIBRATION_MANIFEST_DESTINATION_NOT_COLOCATED")
    parent = os.path.dirname(bundle)
    if not os.path.isdir(parent): raise SystemExit("CALIBRATION_BUNDLE_PARENT_MISSING")
    staging = tempfile.mkdtemp(prefix=f".{os.path.basename(bundle)}.staging-", dir=parent)
    try:
        staged_csv = os.path.join(staging, os.path.basename(output_path))
        staged_manifest = os.path.join(staging, os.path.basename(manifest_path))
        write_csv(staged_csv, records)
        content = Path(staged_csv).read_bytes()
        authority = {key: collection[key] for key in VALIDATION_FIELDS}
        authority.update(schema_version=MANIFEST_SCHEMA, kind=AUTHORITY_KIND, mode="full",
            status="VALIDATED_PRODUCTION", production_authority=True,
            collection_experiment_id=args.experiment_id,
            numa_balancing_restore_status=collection["numa_balancing_restore_status"],
            timing_metric="throughput_ops_sec", calibration_artifact_basename=os.path.basename(output_path),
            calibration_artifact_sha256=hashlib.sha256(content).hexdigest(),
            calibration_record_count=len(records), calibration_version=CALIBRATION_VERSION,
            topology_fingerprints=sorted({row["topology_fingerprint"] for row in records}),
            raw_artifacts=collection["raw_artifacts"])
        with open(staged_manifest, "w", encoding="utf-8") as handle:
            json.dump(authority, handle, sort_keys=True, separators=(",", ":")); handle.write("\n"); handle.flush(); os.fsync(handle.fileno())
        directory = os.open(staging, os.O_RDONLY)
        try: os.fsync(directory)
        finally: os.close(directory)
        publish_bundle(staging, bundle)
    finally:
        if os.path.exists(staging): shutil.rmtree(staging)
    print(output_path)
def main():
    parser = argparse.ArgumentParser(); parser.add_argument("--raw", required=True); parser.add_argument("--cost", required=True); parser.add_argument("--output", required=True); parser.add_argument("--collection-manifest", required=True); parser.add_argument("--calibration-manifest", required=True); parser.add_argument("--experiment-id", required=True); parser.add_argument("--created-at-utc", required=True); parser.add_argument("--cpu-architecture", required=True); parser.add_argument("--cpu-model", required=True); parser.add_argument("--online-numa-nodes", required=True); parser.add_argument("--local-permitted-cpu-count", required=True); parser.add_argument("--minimum-pairs", type=int, default=MIN_SAMPLES); parser.add_argument("--minimum-cost-samples", type=int, default=MIN_SAMPLES); parser.add_argument("--safety-margin-pct", type=float, default=1.0); args = parser.parse_args();
    if args.minimum_pairs < MIN_SAMPLES or args.minimum_cost_samples < MIN_SAMPLES or not 0 <= args.safety_margin_pct <= 100: raise SystemExit("invalid calibration policy")
    build(args)
if __name__ == "__main__": main()
