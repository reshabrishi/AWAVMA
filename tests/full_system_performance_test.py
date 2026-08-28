#!/usr/bin/env python3
"""Measure the real foreground AWAVMA runtime without changing its policy."""

from __future__ import annotations

import csv
import argparse
import ctypes.util
import math
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
LOGS = ROOT / "logs"
WORKLOADS = ("sequential", "random", "hot", "moderate", "cold", "mixed", "changing", "local")
WARMUPS = 5
MEASURED = 5
THREADS = 2
MEMORY_MB = 8
ITERATIONS = 1_000_000
SEED = 12345
EVALUATION_MS = 50
RESULT_PREFIX = "full_system_performance"
DISCOVERY_INTERVAL_MS = 0
GRAPH_OUTPUT_DIR: Path | None = None
RAW_FIELDS = (
    "timestamp", "run_id", "workload", "mode", "run_type", "run_index", "threads", "memory_mb",
    "iterations", "seed", "benchmark_pid", "app_id", "benchmark_elapsed_ms",
    "benchmark_throughput_ops_sec", "benchmark_cpu_percent", "benchmark_rss_kb",
    "runtime_cpu_percent", "runtime_rss_kb", "runtime_vms_kb", "runtime_cycle_count", "runtime_cycle_total_us",
    "discovery_us", "scheduler_or_runtime_scan_us", "queue_wait_us", "queue_depth_mean",
    "queue_depth_max", "jobs_submitted", "jobs_executed", "jobs_deferred",
    "queue_saturation_events", "worker_execution_us", "phase3_us", "pipeline_serial_wait_us",
    "phase4_us", "phase5_us", "phase6_us", "coordinator_overhead_us", "completion_latency_us",
    "pipeline_statuses", "status", "notes",
)
SUMMARY_FIELDS = ("workload", "mode", "metric", "count", "mean", "median", "min", "max", "stddev", "status")
OVERHEAD_FIELDS = ("workload", "metric", "baseline_mean", "integrated_mean", "absolute_difference", "percent_change", "status")
PROFILE_FIELDS = ("run_id", "workload", "mode", "run_type", "level", "component", "application", "pid", "job_id", "start_ns", "end_ns", "duration_us", "status", "metric_value")


def result_path(suffix: str) -> Path:
    return RESULTS / f"{RESULT_PREFIX}_{suffix}.csv"


def number(value: object) -> float | None:
    if value is None or str(value).strip().upper() in ("", "NA", "N/A", "UNKNOWN", "UNAVAILABLE"):
        return None
    try:
        result = float(str(value))
    except ValueError:
        return None
    return result if math.isfinite(result) else None


def text(value: float | int | None) -> str:
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return "NA"
    return f"{value:.3f}" if isinstance(value, float) else str(value)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def write_csv(path: Path, fields: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def proc_sample(pid: int) -> dict[str, float] | None:
    try:
        stat = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
        closing = stat.rfind(")")
        fields = stat[closing + 2:].split()
        if closing < 0 or len(fields) < 22:
            return None
        page_size = os.sysconf("SC_PAGE_SIZE")
        ticks = os.sysconf("SC_CLK_TCK")
        return {
            "time": time.monotonic(),
            "cpu_seconds": (int(fields[11]) + int(fields[12])) / ticks,
            "rss_kb": int(fields[21]) * page_size / 1024.0,
            "vms_kb": int(fields[20]) / 1024.0,
        }
    except (FileNotFoundError, PermissionError, ValueError, IndexError, OSError):
        return None


def resource_metrics(samples: list[dict[str, float]]) -> dict[str, float | None]:
    if not samples:
        return {"cpu_percent": None, "cpu_peak_percent": None, "rss_kb": None, "vms_kb": None}
    first, last = samples[0], samples[-1]
    elapsed = last["time"] - first["time"]
    cpu = None
    if len(samples) > 1 and elapsed > 0:
        cpu = (last["cpu_seconds"] - first["cpu_seconds"]) / elapsed * 100.0
    peaks = [(later["cpu_seconds"] - earlier["cpu_seconds"]) / (later["time"] - earlier["time"]) * 100.0
             for earlier, later in zip(samples, samples[1:]) if later["time"] > earlier["time"]]
    return {
        "cpu_percent": cpu,
        "cpu_peak_percent": max(peaks) if peaks else None,
        "rss_kb": max(sample["rss_kb"] for sample in samples),
        "vms_kb": max(sample["vms_kb"] for sample in samples),
    }


def terminate(process: subprocess.Popen[str], name: str, notes: list[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)
        notes.append(f"forced cleanup: {name}")


def profile_values(rows: list[dict[str, str]], component: str) -> list[float]:
    return [value for row in rows if row.get("component") == component
            if (value := number(row.get("duration_us"))) is not None]


def profile_metrics(rows: list[dict[str, str]]) -> dict[str, float | int | None]:
    by_job: dict[str, dict[str, dict[str, str]]] = defaultdict(dict)
    for row in rows:
        job_id = row.get("job_id", "")
        if job_id:
            by_job[job_id][row.get("component", "")] = row
    queue_waits: list[float] = []
    completions: list[float] = []
    for events in by_job.values():
        submitted = events.get("job_submission")
        started = events.get("job_started")
        completed = events.get("job_completed")
        if submitted and started:
            queue_waits.append((int(started["start_ns"]) - int(submitted["end_ns"])) / 1000.0)
        if submitted and completed:
            completions.append((int(completed["end_ns"]) - int(submitted["start_ns"])) / 1000.0)
    depths = [number(row.get("metric_value")) for row in rows if row.get("component") == "queue_depth"]
    depths = [value for value in depths if value is not None]
    deferred = [number(row.get("metric_value")) for row in rows if row.get("component") == "queue_deferred"]
    deferred = [value for value in deferred if value is not None]
    total = profile_values(rows, "coordinator_application_total")
    phases = sum(sum(profile_values(rows, name)) for name in ("phase4_execution", "phase5_execution", "phase6_execution"))
    return {
        "discovery_us": statistics.mean(profile_values(rows, "application_discovery_scan")) if profile_values(rows, "application_discovery_scan") else None,
        "scheduler_or_runtime_scan_us": statistics.mean(profile_values(rows, "runtime_schedule_active")) if profile_values(rows, "runtime_schedule_active") else None,
        "queue_wait_us": statistics.mean(queue_waits) if queue_waits else None,
        "queue_depth_mean": statistics.mean(depths) if depths else None,
        "queue_depth_max": max(depths) if depths else None,
        "jobs_submitted": sum(1 for row in rows if row.get("component") == "job_submission" and row.get("status") == "OK"),
        "jobs_executed": sum(1 for row in rows if row.get("component") == "worker_execution" and row.get("status") == "OK"),
        "jobs_deferred": int(sum(deferred)),
        "queue_saturation_events": len(deferred),
        "worker_execution_us": statistics.mean(profile_values(rows, "worker_execution")) if profile_values(rows, "worker_execution") else None,
        "phase3_us": statistics.mean(profile_values(rows, "monitor_execution")) if profile_values(rows, "monitor_execution") else None,
        "pipeline_serial_wait_us": statistics.mean(profile_values(rows, "pipeline_serial_wait")) if profile_values(rows, "pipeline_serial_wait") else None,
        "phase4_us": statistics.mean(profile_values(rows, "phase4_execution")) if profile_values(rows, "phase4_execution") else None,
        "phase5_us": statistics.mean(profile_values(rows, "phase5_execution")) if profile_values(rows, "phase5_execution") else None,
        "phase6_us": statistics.mean(profile_values(rows, "phase6_execution")) if profile_values(rows, "phase6_execution") else None,
        "coordinator_overhead_us": max(0.0, sum(total) - phases) if total else None,
        "completion_latency_us": statistics.mean(completions) if completions else None,
        "runtime_cycle_count": sum(1 for row in rows if row.get("component") == "runtime_cycle_total"),
        "runtime_cycle_total_us": statistics.mean(profile_values(rows, "runtime_cycle_total")) if profile_values(rows, "runtime_cycle_total") else None,
    }


def benchmark_metrics(path: Path) -> tuple[float | None, float | None]:
    rows = read_csv(path)
    if not rows:
        return None, None
    row = rows[-1]
    elapsed = number(row.get("execution_time_sec"))
    return (elapsed * 1000.0 if elapsed is not None else None,
            number(row.get("throughput_ops_sec")))


class Evaluation:
    def __init__(self) -> None:
        RESULTS.mkdir(parents=True, exist_ok=True)
        LOGS.mkdir(parents=True, exist_ok=True)
        self.log = (LOGS / "full_system_performance.log").open("w", encoding="utf-8")
        self.profile_rows: list[dict[str, str]] = []

    def close(self) -> None:
        self.log.close()

    def note(self, message: str) -> None:
        self.log.write(message.rstrip() + "\n")
        self.log.flush()

    def build(self) -> bool:
        result = subprocess.run(["make", "profile-awavma-runtime"], cwd=ROOT, capture_output=True, text=True)
        self.note(f"BUILD profile-awavma-runtime returncode={result.returncode}")
        self.note(result.stdout)
        self.note(result.stderr)
        required = (ROOT / "bin/benchmark", ROOT / "bin/profile-awavma-runtime", ROOT / "bin/classifier", ROOT / "bin/decision", ROOT / "bin/validation")
        if result.returncode != 0 or not all(path.is_file() and os.access(path, os.X_OK) for path in required):
            return False
        probe = subprocess.run([str(ROOT / "bin/benchmark"), "--help"], cwd=ROOT, capture_output=True, text=True)
        runtime_probe = subprocess.run([str(ROOT / "bin/profile-awavma-runtime"), "--help"], cwd=ROOT, capture_output=True, text=True)
        return probe.returncode == 0 and runtime_probe.returncode == 0

    def baseline(self, workload: str, root: Path) -> tuple[dict[str, float | None], str, str]:
        output = root / "benchmark.csv"
        command = [str(ROOT / "bin/benchmark"), "--threads", str(THREADS), "--memory", str(MEMORY_MB),
                   "--iterations", str(ITERATIONS), "--pattern", workload, "--seed", str(SEED), "--output", str(output)]
        process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        samples: list[dict[str, float]] = []
        deadline = time.monotonic() + 30
        while process.poll() is None and time.monotonic() < deadline:
            if (sample := proc_sample(process.pid)) is not None:
                samples.append(sample)
            time.sleep(0.002)
        notes: list[str] = []
        if process.poll() is None:
            terminate(process, "benchmark", notes)
        stdout, stderr = process.communicate()
        elapsed, throughput = benchmark_metrics(output)
        resource = resource_metrics(samples)
        metrics = {"benchmark_pid": process.pid, "benchmark_elapsed_ms": elapsed,
                   "benchmark_throughput_ops_sec": throughput,
                   "benchmark_cpu_percent": resource["cpu_percent"],
                   "benchmark_rss_kb": resource["rss_kb"]}
        return metrics, "MEASURED" if process.returncode == 0 and elapsed is not None else "FAILED_BENCHMARK", "; ".join(notes + [stderr.strip()])

    def integrated(self, workload: str, root: Path, run_id: str, run_type: str) -> tuple[dict[str, float | int | None], str, str, str, str]:
        benchmark_output = root / "benchmark.csv"
        runtime_root = root / "runtime"
        profile_path = root / "profile.csv"
        benchmark_command = [str(ROOT / "bin/benchmark"), "--threads", str(THREADS), "--memory", str(MEMORY_MB),
                             "--iterations", str(ITERATIONS), "--pattern", workload, "--seed", str(SEED), "--output", str(benchmark_output)]
        benchmark = subprocess.Popen(benchmark_command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        environment = os.environ.copy()
        environment["AWAVMA_PROFILE_PATH"] = str(profile_path)
        runtime_command = [str(ROOT / "bin/profile-awavma-runtime"), "--duration-ms", str(EVALUATION_MS),
                           "--evaluation-ms", str(EVALUATION_MS), "--pid", str(benchmark.pid),
                           "--root-dir", str(runtime_root)]
        if DISCOVERY_INTERVAL_MS > 0:
            runtime_command.extend(("--discovery-interval-ms", str(DISCOVERY_INTERVAL_MS)))
        runtime = subprocess.Popen(runtime_command, cwd=ROOT, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        benchmark_samples: list[dict[str, float]] = []
        runtime_samples: list[dict[str, float]] = []
        notes: list[str] = []
        deadline = time.monotonic() + 30
        try:
            while (benchmark.poll() is None or runtime.poll() is None) and time.monotonic() < deadline:
                if benchmark.poll() is None and (sample := proc_sample(benchmark.pid)) is not None:
                    benchmark_samples.append(sample)
                if runtime.poll() is None and (sample := proc_sample(runtime.pid)) is not None:
                    runtime_samples.append(sample)
                time.sleep(0.002)
            if benchmark.poll() is None:
                terminate(benchmark, "benchmark", notes)
            if runtime.poll() is None:
                terminate(runtime, "awavma-runtime", notes)
        finally:
            if benchmark.poll() is None:
                terminate(benchmark, "benchmark", notes)
            if runtime.poll() is None:
                terminate(runtime, "awavma-runtime", notes)
        benchmark_stdout, benchmark_stderr = benchmark.communicate()
        runtime_stdout, runtime_stderr = runtime.communicate()
        profile = read_csv(profile_path)
        for row in profile:
            self.profile_rows.append({"run_id": run_id, "workload": workload, "mode": "INTEGRATED", "run_type": run_type,
                                      **{field: row.get(field, "") for field in PROFILE_FIELDS[4:]}})
        metrics: dict[str, float | int | None] = profile_metrics(profile)
        benchmark_resource = resource_metrics(benchmark_samples)
        runtime_resource = resource_metrics(runtime_samples)
        elapsed, throughput = benchmark_metrics(benchmark_output)
        metrics.update({"benchmark_elapsed_ms": elapsed, "benchmark_throughput_ops_sec": throughput,
                        "benchmark_pid": benchmark.pid,
                        "benchmark_cpu_percent": benchmark_resource["cpu_percent"],
                        "benchmark_rss_kb": benchmark_resource["rss_kb"],
                         "runtime_cpu_percent": runtime_resource["cpu_percent"],
                         "runtime_cpu_peak_percent": runtime_resource["cpu_peak_percent"],
                        "runtime_rss_kb": runtime_resource["rss_kb"], "runtime_vms_kb": runtime_resource["vms_kb"]})
        runtime_rows = read_csv(runtime_root / "runtime_results.csv")
        statuses = Counter(row.get("status", "UNKNOWN") for row in runtime_rows)
        status_text = ";".join(f"{name}:{count}" for name, count in sorted(statuses.items())) or "NO_RUNTIME_RECORD"
        app_id = runtime_rows[0].get("app_id", "NA") if runtime_rows else "NA"
        status = "MEASURED"
        if benchmark.returncode != 0 or elapsed is None:
            status = "FAILED_BENCHMARK"
        elif runtime.returncode != 0:
            status = "FAILED_RUNTIME"
        elif not runtime_rows or not profile_values(profile, "monitor_execution"):
            status = "TARGET_NOT_DISCOVERED"
        detail = "; ".join(part for part in ("; ".join(notes), benchmark_stderr.strip(), runtime_stderr.strip()) if part)
        return metrics, status, detail, app_id, status_text


def summarize(rows: list[dict[str, str]]) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    metrics = ("benchmark_elapsed_ms", "benchmark_throughput_ops_sec", "runtime_cpu_percent", "runtime_rss_kb",
               "runtime_vms_kb", "benchmark_cpu_percent", "benchmark_rss_kb", "discovery_us", "queue_wait_us", "worker_execution_us", "phase3_us",
               "pipeline_serial_wait_us", "phase4_us", "phase5_us", "phase6_us", "coordinator_overhead_us",
               "completion_latency_us", "queue_depth_max", "runtime_cycle_total_us", "runtime_cycle_count",
               "jobs_submitted", "jobs_executed", "jobs_deferred", "queue_saturation_events")
    summary: list[dict[str, str]] = []
    means: dict[tuple[str, str, str], float] = {}
    for workload in WORKLOADS:
        for mode in ("BASELINE", "INTEGRATED"):
            for metric in metrics:
                values = [number(row.get(metric)) for row in rows if row["workload"] == workload and row["mode"] == mode and row["run_type"] == "MEASURED" and row["status"] == "MEASURED"]
                values = [value for value in values if value is not None]
                if values:
                    means[(workload, mode, metric)] = statistics.mean(values)
                summary.append({"workload": workload, "mode": mode, "metric": metric, "count": str(len(values)),
                                "mean": text(statistics.mean(values) if values else None),
                                "median": text(statistics.median(values) if values else None),
                                "min": text(min(values) if values else None), "max": text(max(values) if values else None),
                                "stddev": text(statistics.stdev(values) if len(values) > 1 else None),
                                "status": "MEASURED" if values else "UNAVAILABLE"})
    overhead: list[dict[str, str]] = []
    for workload in WORKLOADS:
        for metric in ("benchmark_elapsed_ms", "benchmark_throughput_ops_sec"):
            baseline = means.get((workload, "BASELINE", metric))
            integrated = means.get((workload, "INTEGRATED", metric))
            if baseline is None or integrated is None or baseline == 0:
                overhead.append({"workload": workload, "metric": metric, "baseline_mean": "NA", "integrated_mean": "NA", "absolute_difference": "NA", "percent_change": "NA", "status": "UNAVAILABLE"})
                continue
            overhead.append({"workload": workload, "metric": metric, "baseline_mean": text(baseline),
                             "integrated_mean": text(integrated), "absolute_difference": text(integrated - baseline),
                             "percent_change": text((integrated - baseline) / baseline * 100.0), "status": "MEASURED"})
    return summary, overhead


def environment_rows() -> list[dict[str, str]]:
    nodes = sorted(path.name for path in Path("/sys/devices/system/node").glob("node[0-9]*"))
    return [
        {"key": "cpus", "value": str(os.cpu_count() or 0), "status": "MEASURED"},
        {"key": "numa_nodes", "value": ";".join(nodes), "status": "MEASURED"},
        {"key": "libnuma", "value": "available" if ctypes.util.find_library("numa") else "unavailable", "status": "MEASURED"},
        {"key": "numa.h", "value": "available" if Path("/usr/include/numa.h").is_file() else "unavailable", "status": "NOT TESTED — ENVIRONMENT LIMITATION"},
        {"key": "numaif.h", "value": "available" if Path("/usr/include/numaif.h").is_file() else "unavailable", "status": "NOT TESTED — ENVIRONMENT LIMITATION"},
        {"key": "remote_numa_migration", "value": "not demonstrated", "status": "NOT TESTED — ENVIRONMENT LIMITATION"},
        {"key": "perf_cache_counters", "value": "permission limited", "status": "NOT TESTED — ENVIRONMENT LIMITATION"},
        {"key": "benchmark_build", "value": "frozen existing binary; source rebuild requires unavailable numa.h", "status": "NOT TESTED — ENVIRONMENT LIMITATION"},
        {"key": "worker_count", "value": "2", "status": "MEASURED"},
        {"key": "monitor_interval_ms", "value": "100", "status": "MEASURED"},
        {"key": "evaluation_interval_ms", "value": str(EVALUATION_MS), "status": "MEASURED"},
    ]


def main() -> int:
    global RESULT_PREFIX, DISCOVERY_INTERVAL_MS, GRAPH_OUTPUT_DIR
    parser = argparse.ArgumentParser()
    parser.add_argument("--discovery-interval-ms", type=int, default=250)
    parser.add_argument("--prefix", default="full_system_performance")
    parser.add_argument("--graph-output-dir", type=Path)
    parser.add_argument("--summarize-only", action="store_true")
    args = parser.parse_args()
    if args.discovery_interval_ms < 0:
        return 1
    RESULT_PREFIX = args.prefix
    DISCOVERY_INTERVAL_MS = args.discovery_interval_ms
    GRAPH_OUTPUT_DIR = args.graph_output_dir
    if args.summarize_only:
        rows = read_csv(result_path("raw"))
        summary, overhead = summarize(rows)
        write_csv(result_path("summary"), SUMMARY_FIELDS, summary)
        write_csv(result_path("overhead"), OVERHEAD_FIELDS, overhead)
        graph_command = [sys.executable, str(ROOT / "scripts/generate_full_system_performance_graphs.py"), "--root", str(ROOT), "--prefix", RESULT_PREFIX]
        if GRAPH_OUTPUT_DIR is not None:
            graph_command.extend(("--output-dir", str(GRAPH_OUTPUT_DIR)))
        graph = subprocess.run(graph_command, cwd=ROOT)
        print(f"Full-system performance summaries regenerated from {len(rows)} raw rows.")
        return graph.returncode
    evaluation = Evaluation()
    rows: list[dict[str, str]] = []
    try:
        if not evaluation.build():
            print("Full-system performance prerequisites failed; no measurements were generated.")
            return 1
        write_csv(result_path("environment"), ("key", "value", "status"), environment_rows())
        evaluation.note("ORDER: per workload, five BASELINE/INTEGRATED warm-up pairs followed by five measured pairs")
        evaluation.note(f"PARAMETERS: threads={THREADS} memory_mb={MEMORY_MB} iterations={ITERATIONS} seed={SEED} worker_count=2 monitor_interval_ms=100 evaluation_interval_ms={EVALUATION_MS}")
        with tempfile.TemporaryDirectory(prefix="awavma-full-system-") as temporary:
            root = Path(temporary)
            for workload in WORKLOADS:
                for run_type, repetitions in (("WARMUP", WARMUPS), ("MEASURED", MEASURED)):
                    for run_index in range(1, repetitions + 1):
                        for mode in ("BASELINE", "INTEGRATED"):
                            run_id = f"{workload.lower()}-{run_type.lower()}-{run_index}-{mode.lower()}"
                            run_root = root / run_id
                            run_root.mkdir()
                            if mode == "BASELINE":
                                metrics, status, notes = evaluation.baseline(workload, run_root)
                                app_id = "NA"
                                statuses = "NOT_RUN"
                            else:
                                metrics, status, notes, app_id, statuses = evaluation.integrated(workload, run_root, run_id, run_type)
                            if run_type == "WARMUP" and status == "MEASURED":
                                status = "WARMUP"
                            row = {field: "NA" for field in RAW_FIELDS}
                            row.update({"timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "run_id": run_id,
                                        "workload": workload, "mode": mode, "run_type": run_type, "run_index": str(run_index),
                                        "threads": str(THREADS), "memory_mb": str(MEMORY_MB), "iterations": str(ITERATIONS),
                                        "seed": str(SEED), "benchmark_pid": "NA", "app_id": app_id,
                                        "pipeline_statuses": statuses, "status": status, "notes": notes or ""})
                            for key, value in metrics.items():
                                if key in row:
                                    row[key] = text(value if isinstance(value, (int, float)) else None)
                            rows.append(row)
                            evaluation.note(f"RUN {run_id} status={status} elapsed_ms={row['benchmark_elapsed_ms']} pipeline={statuses}")
        write_csv(result_path("raw"), RAW_FIELDS, rows)
        write_csv(result_path("profile_events"), PROFILE_FIELDS, evaluation.profile_rows)
        summary, overhead = summarize(rows)
        write_csv(result_path("summary"), SUMMARY_FIELDS, summary)
        write_csv(result_path("overhead"), OVERHEAD_FIELDS, overhead)
        status_counts = Counter()
        for row in rows:
            if row["mode"] == "INTEGRATED":
                for item in row["pipeline_statuses"].split(";"):
                    if ":" in item:
                        name, count = item.split(":", 1)
                        status_counts[name] += int(count)
        status_rows = [{"status": name, "count": str(count), "meaning": "actual runtime status"} for name, count in sorted(status_counts.items())]
        status_rows.extend([
            {"status": "PHASE7", "count": "0", "meaning": "NOT EXECUTED — INSUFFICIENT REAL MIGRATION METADATA"},
            {"status": "PHASE8", "count": "0", "meaning": "NOT EXECUTED — no valid later before/after observation"},
        ])
        write_csv(result_path("status_counts"), ("status", "count", "meaning"), status_rows)
        graph_command = [sys.executable, str(ROOT / "scripts/generate_full_system_performance_graphs.py"), "--root", str(ROOT), "--prefix", RESULT_PREFIX]
        if GRAPH_OUTPUT_DIR is not None:
            graph_command.extend(("--output-dir", str(GRAPH_OUTPUT_DIR)))
        graph = subprocess.run(graph_command, cwd=ROOT, capture_output=True, text=True)
        evaluation.note(graph.stdout)
        evaluation.note(graph.stderr)
        valid = sum(row["status"] == "MEASURED" and row["run_type"] == "MEASURED" for row in rows)
        failures = sum(row["status"].startswith("FAILED") or row["status"] == "TARGET_NOT_DISCOVERED" for row in rows)
        print(f"Full-system performance: measured_valid={valid}/80 failures={failures} graphs_returncode={graph.returncode}")
        return 0 if graph.returncode == 0 else 1
    finally:
        write_csv(result_path("raw"), RAW_FIELDS, rows)
        evaluation.close()


if __name__ == "__main__":
    raise SystemExit(main())
