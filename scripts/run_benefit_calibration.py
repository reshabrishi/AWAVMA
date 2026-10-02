#!/usr/bin/env python3
"""Collect a topology-specific, controlled thread-placement calibration artifact."""
import argparse
import csv
import hashlib
import math
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = 3
METHODOLOGY_VERSION = 1
SCHEDULE_VERSION = 1
PRODUCTION_WORKLOAD = "mixed"
PRODUCTION_THREADS = 2
PRODUCTION_MEMORY_MB = 1024
PRODUCTION_DURATION_SECONDS = 10.0
PRODUCTION_RUNS = 5
PRODUCTION_WARMUPS = 2


def utc_now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def command_text(command):
    return subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)


def nodes():
    result = command_text(["numactl", "--hardware"])
    if result.returncode:
        raise RuntimeError("numactl --hardware failed")
    found = [int(value) for value in re.findall(r"^node\s+(\d+)\s+cpus:", result.stdout, re.MULTILINE)]
    if len(found) < 2:
        raise RuntimeError("fewer than two NUMA nodes are visible")
    return found, result.stdout


def cpu_model():
    for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
        if line.startswith("model name"):
            return line.split(":", 1)[1].strip().replace(",", " ")
    return "unknown-cpu"


def socket_count():
    physical = {line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines()
                if line.startswith("physical id")}
    return max(1, len(physical))


def numa_distance_fingerprint(available):
    value = 1469598103934665603
    for node in sorted(available):
        distance = Path(f"/sys/devices/system/node/node{node}/distance").read_text(encoding="utf-8")
        canonical = f"node{node}:{''.join(' ' if char.isspace() else char for char in distance)};"
        for byte in canonical.encode("utf-8"):
            value ^= byte
            value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def write_metadata(directory, topology, environment):
    metadata = directory / "metadata"
    metadata.mkdir()
    (metadata / "git_revision.txt").write_text(command_text(["git", "rev-parse", "HEAD"]).stdout, encoding="utf-8")
    (metadata / "git_status.txt").write_text(command_text(["git", "status", "--short"]).stdout, encoding="utf-8")
    (metadata / "numa_topology.txt").write_text(topology, encoding="utf-8")
    (metadata / "uname.txt").write_text(platform.platform() + "\n", encoding="utf-8")
    compiler = command_text(["cc", "--version"])
    (metadata / "compiler.txt").write_text(compiler.stdout + compiler.stderr, encoding="utf-8")
    (metadata / "environment_check.txt").write_text(environment, encoding="utf-8")


def last_native_row(path):
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise RuntimeError("benchmark emitted no native CSV row")
    row = rows[-1]
    for field in ("operations", "execution_time_sec", "throughput_ops_sec"):
        if not math.isfinite(float(row[field])) or float(row[field]) <= 0:
            raise RuntimeError("benchmark emitted invalid " + field)
    return row


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def calibration_schedule(local_node, remote_node):
    schedule = []
    for condition, node in (("LOCAL", local_node), ("LOCAL", local_node),
                            ("REMOTE", remote_node), ("REMOTE", remote_node)):
        index = len(schedule) + 1
        schedule.append({"sequence_index": index, "logical_run_id": f"warmup-{condition.lower()}-{index if condition == 'LOCAL' else index - 2:02d}",
                         "role": "WARMUP", "condition": condition, "scenario": f"{condition}_WARMUP",
                         "run_index": index if condition == "LOCAL" else index - 2, "thread_node": node,
                         "memory_node": local_node})
    for run_index in range(1, PRODUCTION_RUNS + 1):
        for condition, node in (("LOCAL", local_node), ("REMOTE", remote_node)):
            sequence_index = len(schedule) + 1
            schedule.append({"sequence_index": sequence_index,
                             "logical_run_id": f"measured-{condition.lower()}-{run_index:02d}",
                             "role": "MEASURED", "condition": condition, "scenario": condition,
                             "run_index": run_index, "thread_node": node, "memory_node": local_node})
    return schedule


def canonical_evidence_manifest(args, available, local_node, remote_node, distance_fingerprint, cpu, sockets, rows, directory):
    lines = ["AWAVMA_BENEFIT_EVIDENCE_MANIFEST 1",
             f"calibration_id={args.calibration_id}",
             f"methodology_version={METHODOLOGY_VERSION}",
             f"schedule_version={SCHEDULE_VERSION}",
             f"local_node={local_node}", f"remote_node={remote_node}",
             f"numa_node_count={len(available)}", f"numa_distance_fingerprint={distance_fingerprint}",
             f"cpu_model={cpu}", f"socket_count={sockets}",
             f"workload={args.workload}", f"threads={args.threads}", f"memory_mb={args.memory_mb}",
             f"duration_seconds={args.duration_seconds:g}", f"warmup_runs={args.warmups}",
             f"measured_runs_per_scenario={args.runs}",
             f"file=calibration_runs.csv|{sha256_file(directory / 'calibration_runs.csv')}"]
    for row in rows:
        raw_path = row["native_evidence_path"]
        lines.append("run={}|{}|{}|{}|{}|{}|{}".format(
            row["sequence_index"], row["logical_run_id"], row["role"], row["condition"], raw_path,
            sha256_file(directory / raw_path), row["logical_run_id"]))
    return "\n".join(lines) + "\n"


def run_benchmark(args, output, entry):
    scenario, run_index = entry["scenario"], entry["run_index"]
    thread_node, memory_node = entry["thread_node"], entry["memory_node"]
    command = [str(ROOT / "bin" / "benchmark"), "--threads", str(args.threads), "--memory", str(args.memory_mb),
               "--duration", str(args.duration_seconds), "--pattern", args.workload, "--seed", str(args.seed),
               "--thread-node", str(thread_node), "--memory-node", str(memory_node), "--output", str(output)]
    completed = command_text(command)
    row = {"calibration_id": args.calibration_id, "timestamp_utc": utc_now(), "sequence_index": entry["sequence_index"],
           "logical_run_id": entry["logical_run_id"], "role": entry["role"], "condition": entry["condition"],
           "native_evidence_path": entry["native_evidence_path"], "run_index": run_index, "scenario": scenario,
           "workload": args.workload, "seed": args.seed, "thread_node": thread_node,
           "memory_node": memory_node, "threads": args.threads, "memory_mb": args.memory_mb,
           "duration_seconds": args.duration_seconds, "operations": "", "execution_time_sec": "",
           "throughput_ops_sec": "", "status": "FAILED"}
    if completed.returncode == 0:
        try:
            native = last_native_row(output)
            if int(native["thread_node"]) != thread_node or int(native["memory_node"]) != memory_node:
                raise RuntimeError("benchmark placement differs from requested placement")
            row.update({"operations": native["operations"], "execution_time_sec": native["execution_time_sec"],
                        "throughput_ops_sec": native["throughput_ops_sec"], "status": "MEASURED"})
        except (KeyError, ValueError, RuntimeError) as error:
            row["status"] = "INVALID: " + str(error)
    return row, completed


def metric_summary(rows, scenario, field):
    values = [float(row[field]) for row in rows if row["scenario"] == scenario and row["status"] == "MEASURED"]
    return {"mean": statistics.mean(values), "median": statistics.median(values),
            "sample_stdev": statistics.stdev(values) if len(values) > 1 else 0.0}


def successful(rows, scenario):
    return [row for row in rows if row["scenario"] == scenario and row["status"] == "MEASURED"]


def valid_summary(metrics):
    return (math.isfinite(metrics["mean"]) and metrics["mean"] > 0 and
            math.isfinite(metrics["median"]) and metrics["median"] > 0 and
            math.isfinite(metrics["sample_stdev"]) and metrics["sample_stdev"] >= 0)


def is_production_methodology(args):
    return (args.workload == PRODUCTION_WORKLOAD and args.threads == PRODUCTION_THREADS and
            args.memory_mb == PRODUCTION_MEMORY_MB and
            args.duration_seconds == PRODUCTION_DURATION_SECONDS and args.runs == PRODUCTION_RUNS and
            args.warmups == PRODUCTION_WARMUPS)


def supported_migration_route(local_node, remote_node):
    """Positive LOCAL-minus-REMOTE gain supports moving the remote thread back local."""
    return remote_node, local_node


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", default=None)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--memory-mb", type=int, default=1024)
    parser.add_argument("--duration-seconds", type=float, default=10.0)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--seed", type=int, default=20260930)
    parser.add_argument("--workload", default="mixed")
    parser.add_argument("--local-node", type=int)
    parser.add_argument("--remote-node", type=int)
    args = parser.parse_args()
    if not is_production_methodology(args):
        parser.error("production calibration requires workload=mixed, threads=2, memory-mb=1024, "
                     "duration-seconds=10, runs=5, warmups=2")
    available, topology = nodes()
    local = available[0] if args.local_node is None else args.local_node
    remote = available[1] if args.remote_node is None else args.remote_node
    if local == remote or local not in available or remote not in available:
        parser.error("local and remote nodes must be distinct available NUMA nodes")
    args.calibration_id = "benefit-calibration-{}-{}".format(datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"), os.getpid())
    directory = Path(args.output_dir) if args.output_dir else ROOT / "results" / "cloudlab" / "calibration" / args.calibration_id
    if directory.exists():
        parser.error("output directory already exists")
    directory.mkdir(parents=True)
    environment = command_text([str(ROOT / "bin" / "environment-check")])
    environment_text = environment.stdout + environment.stderr
    write_metadata(directory, topology, environment_text)
    rows = []
    raw = directory / "raw"
    raw.mkdir()
    logs = directory / "logs"
    logs.mkdir()
    for entry in calibration_schedule(local, remote):
        entry["native_evidence_path"] = f"raw/{entry['logical_run_id']}.csv"
        row, completed = run_benchmark(args, directory / entry["native_evidence_path"], entry)
        rows.append(row)
        (logs / f"{entry['logical_run_id']}.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
    fields = list(rows[0])
    with (directory / "calibration_runs.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerows(rows)
    measured = [row for row in rows if row["role"] == "MEASURED"]
    valid = (environment.returncode == 0 and "READY" in environment_text and
             bool((directory / "metadata/git_revision.txt").read_text().strip()) and
             len(successful(measured, "LOCAL")) == args.runs and
             len(successful(measured, "REMOTE")) == args.runs)
    summary_rows = []
    if valid:
        local_t, remote_t = metric_summary(measured, "LOCAL", "throughput_ops_sec"), metric_summary(measured, "REMOTE", "throughput_ops_sec")
        local_e, remote_e = metric_summary(measured, "LOCAL", "execution_time_sec"), metric_summary(measured, "REMOTE", "execution_time_sec")
        gain = (local_t["mean"] - remote_t["mean"]) / remote_t["mean"] * 100.0
        improvement = (remote_e["mean"] - local_e["mean"]) / remote_e["mean"] * 100.0
        valid = all(valid_summary(metrics) for metrics in (local_t, remote_t, local_e, remote_e)) and math.isfinite(gain) and math.isfinite(improvement)
        for scenario, metrics in (("LOCAL", local_t), ("REMOTE", remote_t)):
            summary_rows.append({"scenario": scenario, "metric": "throughput_ops_sec", **metrics})
        for scenario, metrics in (("LOCAL", local_e), ("REMOTE", remote_e)):
            summary_rows.append({"scenario": scenario, "metric": "execution_time_sec", **metrics})
    else:
        local_t = remote_t = local_e = remote_e = {"mean": "", "median": "", "sample_stdev": ""}; gain = improvement = ""
    with (directory / "calibration_summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=["scenario", "metric", "mean", "median", "sample_stdev"])
        writer.writeheader(); writer.writerows(summary_rows)
    distance_fingerprint = numa_distance_fingerprint(available)
    model, sockets = cpu_model(), socket_count()
    manifest = canonical_evidence_manifest(args, available, local, remote, distance_fingerprint, model, sockets, rows, directory)
    manifest_path = directory / "benefit_evidence.manifest"
    manifest_path.write_text(manifest, encoding="ascii", newline="\n")
    artifact_fields = ["schema_version", "calibration_id", "state", "provenance", "git_revision", "cpu_model", "socket_count", "numa_node_count", "numa_distance_fingerprint", "kernel_release", "source_node", "target_node", "workload", "threads", "memory_mb", "duration_seconds", "measured_runs_per_scenario", "local_mean_throughput", "remote_mean_throughput", "throughput_gain_percent", "local_mean_execution_time", "remote_mean_execution_time", "execution_time_improvement_percent", "environment_check_status", "validation_status", "created_at_utc", "warmup_runs", "methodology_version", "schedule_version", "evidence_manifest_path", "evidence_manifest_sha256"]
    source_node, target_node = supported_migration_route(local, remote)
    artifact = {"schema_version": SCHEMA, "calibration_id": args.calibration_id,
                "state": "VALIDATED_PRODUCTION" if valid else "CONFIGURED_UNVALIDATED",
                "provenance": "controlled_cloudlab_thread_placement_remote_to_local", "git_revision": (directory / "metadata/git_revision.txt").read_text().strip(),
                "cpu_model": model, "socket_count": sockets, "numa_node_count": len(available),
                "numa_distance_fingerprint": distance_fingerprint, "kernel_release": platform.release(),
                "source_node": source_node, "target_node": target_node, "workload": args.workload, "threads": args.threads,
                "memory_mb": args.memory_mb, "duration_seconds": args.duration_seconds, "measured_runs_per_scenario": args.runs,
                "local_mean_throughput": local_t["mean"], "remote_mean_throughput": remote_t["mean"], "throughput_gain_percent": gain,
                "local_mean_execution_time": local_e["mean"], "remote_mean_execution_time": remote_e["mean"], "execution_time_improvement_percent": improvement,
                "environment_check_status": "READY" if environment.returncode == 0 and "READY" in environment_text else "NOT_READY",
                "validation_status": "PASS" if valid else "FAIL", "created_at_utc": utc_now(),
                "warmup_runs": args.warmups, "methodology_version": METHODOLOGY_VERSION,
                "schedule_version": SCHEDULE_VERSION, "evidence_manifest_path": "benefit_evidence.manifest",
                "evidence_manifest_sha256": hashlib.sha256(manifest.encode("ascii")).hexdigest()}
    with (directory / "benefit_calibration.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=artifact_fields)
        writer.writeheader(); writer.writerow(artifact)
    print(directory / "benefit_calibration.csv")
    return 0 if valid else 1


if __name__ == "__main__":
    sys.exit(main())
