#!/usr/bin/env python3
"""Collect a topology-specific, controlled thread-placement calibration artifact."""
import argparse
import csv
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
SCHEMA = 1


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


def run_benchmark(args, output, scenario, run_index, thread_node, memory_node):
    command = [str(ROOT / "bin" / "benchmark"), "--threads", str(args.threads), "--memory", str(args.memory_mb),
               "--duration", str(args.duration_seconds), "--pattern", args.workload, "--seed", str(args.seed),
               "--thread-node", str(thread_node), "--memory-node", str(memory_node), "--output", str(output)]
    completed = command_text(command)
    row = {"calibration_id": args.calibration_id, "timestamp_utc": utc_now(), "run_index": run_index,
           "scenario": scenario, "workload": args.workload, "seed": args.seed, "thread_node": thread_node,
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
    if args.threads != 2 or args.memory_mb <= 0 or args.duration_seconds <= 0 or args.runs != 5 or args.warmups != 2:
        parser.error("controlled calibration requires threads=2, positive memory/duration, runs=5, warmups=2")
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
    local_csv, remote_csv = directory / "benchmark_local.csv", directory / "benchmark_remote.csv"
    rows = []
    logs = directory / "logs"
    logs.mkdir()
    for scenario, output, thread_node in (("LOCAL_WARMUP", local_csv, local), ("REMOTE_WARMUP", remote_csv, remote)):
        for index in range(1, args.warmups + 1):
            row, completed = run_benchmark(args, output, scenario, index, thread_node, local)
            rows.append(row)
            (logs / f"{scenario.lower()}-{index}.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
    for index in range(1, args.runs + 1):
        for scenario, output, thread_node in (("LOCAL", local_csv, local), ("REMOTE", remote_csv, remote)):
            row, completed = run_benchmark(args, output, scenario, index, thread_node, local)
            rows.append(row)
            (logs / f"{scenario.lower()}-{index}.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")
    fields = list(rows[0])
    with (directory / "calibration_runs.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerows(rows)
    measured = [row for row in rows if row["scenario"] in ("LOCAL", "REMOTE")]
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
    artifact_fields = ["schema_version", "calibration_id", "state", "provenance", "git_revision", "cpu_model", "socket_count", "numa_node_count", "numa_distance_fingerprint", "kernel_release", "source_node", "target_node", "workload", "threads", "memory_mb", "duration_seconds", "measured_runs_per_scenario", "local_mean_throughput", "remote_mean_throughput", "throughput_gain_percent", "local_mean_execution_time", "remote_mean_execution_time", "execution_time_improvement_percent", "environment_check_status", "validation_status", "created_at_utc"]
    artifact = {"schema_version": SCHEMA, "calibration_id": args.calibration_id,
                "state": "VALIDATED_PRODUCTION" if valid else "CONFIGURED_UNVALIDATED",
                "provenance": "controlled_cloudlab_thread_placement", "git_revision": (directory / "metadata/git_revision.txt").read_text().strip(),
                "cpu_model": cpu_model(), "socket_count": socket_count(), "numa_node_count": len(available),
                "numa_distance_fingerprint": numa_distance_fingerprint(available), "kernel_release": platform.release(),
                "source_node": local, "target_node": remote, "workload": args.workload, "threads": args.threads,
                "memory_mb": args.memory_mb, "duration_seconds": args.duration_seconds, "measured_runs_per_scenario": args.runs,
                "local_mean_throughput": local_t["mean"], "remote_mean_throughput": remote_t["mean"], "throughput_gain_percent": gain,
                "local_mean_execution_time": local_e["mean"], "remote_mean_execution_time": remote_e["mean"], "execution_time_improvement_percent": improvement,
                "environment_check_status": "READY" if environment.returncode == 0 and "READY" in environment_text else "NOT_READY",
                "validation_status": "PASS" if valid else "FAIL", "created_at_utc": utc_now()}
    with (directory / "benefit_calibration.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=artifact_fields)
        writer.writeheader(); writer.writerow(artifact)
    print(directory / "benefit_calibration.csv")
    return 0 if valid else 1


if __name__ == "__main__":
    sys.exit(main())
