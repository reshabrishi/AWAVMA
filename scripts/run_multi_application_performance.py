#!/usr/bin/env python3
"""Measure bounded FIFO AWAVMA behavior with simultaneous benchmark processes."""

from __future__ import annotations

import argparse
import csv
import ctypes.util
import math
import os
import signal
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
APP_COUNTS = (1, 2, 4, 8)
WORKER_COUNTS = (1, 2, 4)
PRIMARY_WORKLOAD = "mixed"
HETEROGENEOUS_WORKLOADS = ("sequential", "random", "hot", "mixed")
THREADS = 1
MEMORY_MB = 8
MONITOR_MS = 100
EVALUATION_MS = 1000
DISCOVERY_MS = 250
QUEUE_CAPACITY = 64
SAMPLE_PERIOD_SECONDS = 0.01
TARGET_SECONDS = 3.5

RAW_FIELDS = (
    "timestamp", "run_id", "run_type", "mode", "application_count", "worker_count",
    "application_index", "app_id", "pid", "start_time_ticks", "workload", "threads",
    "memory_mb", "iterations", "seed", "launch_monotonic_ns", "launch_skew_us",
    "benchmark_elapsed_ms", "benchmark_throughput", "benchmark_cpu_percent",
    "benchmark_rss_kb", "benchmark_vms_kb", "aggregate_throughput",
     "group_makespan_ms", "runtime_cpu_percent", "runtime_cpu_peak_percent", "runtime_rss_kb", "runtime_vms_kb",
    "runtime_tree_cpu_percent", "runtime_tree_rss_kb", "runtime_tree_vms_kb",
    "monitor_submissions", "monitor_completions", "pipeline_completions",
    "mean_queue_wait_us", "median_queue_wait_us", "max_queue_wait_us",
    "mean_eligibility_wait_us", "max_eligibility_wait_us", "mean_worker_execution_us",
    "mean_phase3_us", "mean_pipeline_serial_wait_us", "max_pipeline_serial_wait_us",
    "mean_phase4_us", "mean_phase5_us", "mean_phase6_us", "mean_completion_latency_us",
    "max_completion_latency_us", "max_queue_depth", "deferred_jobs", "saturation_events",
    "discovery_scan_count", "discovery_total_us", "discovery_mean_us", "runtime_cycle_total_us",
    "evaluation_interval_observed_ms", "max_evaluation_interval_observed_ms", "eligible_opportunities",
    "status", "outcome", "notes",
)
SUMMARY_FIELDS = (
    "scope", "application_count", "worker_count", "mode", "metric", "count", "mean",
    "median", "min", "max", "stddev", "status",
)
SCALING_FIELDS = (
    "mode", "application_count", "worker_count", "aggregate_throughput",
    "mean_application_elapsed_ms", "runtime_cpu_percent", "runtime_rss_kb", "runtime_vms_kb",
    "mean_queue_wait_us", "max_queue_wait_us", "max_queue_depth", "deferred_jobs",
    "saturation_events", "mean_serial_wait_us", "max_serial_wait_us",
    "mean_completion_latency_us", "fairness_index", "status",
)
FAIRNESS_FIELDS = (
    "configuration", "run_id", "application_count", "worker_count", "application_index", "app_id",
    "completed_cycles", "mean_queue_wait_us", "max_queue_wait_us",
    "mean_evaluation_interval_ms", "max_evaluation_interval_ms", "starvation_evidence",
    "jain_fairness_index", "status",
)
RUN_FIELDS = (
    "run_id", "run_type", "mode", "application_count", "worker_count", "iterations",
     "group_makespan_ms", "aggregate_throughput", "runtime_cpu_percent", "runtime_cpu_peak_percent", "runtime_rss_kb",
    "runtime_vms_kb", "runtime_tree_cpu_percent", "runtime_tree_rss_kb", "runtime_tree_vms_kb",
    "max_queue_depth", "deferred_jobs", "saturation_events", "discovery_scan_count",
    "discovery_total_us", "status", "notes",
)
PROFILE_FIELDS = (
    "run_id", "run_type", "mode", "application_count", "worker_count", "level", "component",
    "application", "pid", "job_id", "start_ns", "end_ns", "duration_us", "status", "metric_value",
)


def number(value: object) -> float | None:
    if value is None or str(value).strip().upper() in ("", "NA", "N/A", "UNKNOWN", "UNAVAILABLE"):
        return None
    try:
        parsed = float(str(value))
    except ValueError:
        return None
    return parsed if math.isfinite(parsed) else None


def text(value: float | int | None) -> str:
    if value is None or (isinstance(value, float) and not math.isfinite(value)):
        return "NA"
    return f"{value:.3f}" if isinstance(value, float) else str(value)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def write_csv(path: Path, fields: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
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
            "start_time_ticks": float(fields[19]),
        }
    except (FileNotFoundError, PermissionError, ValueError, IndexError, OSError):
        return None


def metrics(samples: list[dict[str, float]]) -> dict[str, float | None]:
    if not samples:
        return {"cpu_percent": None, "cpu_peak_percent": None, "rss_kb": None, "vms_kb": None}
    first, last = samples[0], samples[-1]
    elapsed = last["time"] - first["time"]
    cpu = (last["cpu_seconds"] - first["cpu_seconds"]) / elapsed * 100.0 if elapsed > 0 else None
    peaks = [(later["cpu_seconds"] - earlier["cpu_seconds"]) / (later["time"] - earlier["time"]) * 100.0
             for earlier, later in zip(samples, samples[1:]) if later["time"] > earlier["time"]]
    return {"cpu_percent": cpu, "cpu_peak_percent": max(peaks) if peaks else None, "rss_kb": max(row["rss_kb"] for row in samples),
            "vms_kb": max(row["vms_kb"] for row in samples)}


def group_metrics(samples: list[dict[int, dict[str, float]]]) -> dict[str, float | None]:
    complete = [row for row in samples if row]
    if len(complete) < 2:
        return {"cpu_percent": None, "rss_kb": None, "vms_kb": None}
    first, last = complete[0], complete[-1]
    elapsed = max(sample["time"] for sample in last.values()) - min(sample["time"] for sample in first.values())
    shared = set(first) & set(last)
    cpu = sum(last[pid]["cpu_seconds"] - first[pid]["cpu_seconds"] for pid in shared) / elapsed * 100.0 if elapsed > 0 else None
    return {"cpu_percent": cpu,
            "rss_kb": max(sum(sample["rss_kb"] for sample in row.values()) for row in complete),
            "vms_kb": max(sum(sample["vms_kb"] for sample in row.values()) for row in complete)}


def process_children(pid: int) -> set[int]:
    children: set[int] = set()
    queue = [pid]
    while queue:
        parent = queue.pop()
        try:
            values = Path(f"/proc/{parent}/task/{parent}/children").read_text(encoding="utf-8").split()
        except (FileNotFoundError, PermissionError, OSError):
            continue
        for value in values:
            try:
                child = int(value)
            except ValueError:
                continue
            if child not in children:
                children.add(child)
                queue.append(child)
    return children


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


def benchmark_result(path: Path) -> tuple[float | None, float | None, int | None]:
    rows = read_csv(path)
    if not rows:
        return None, None, None
    row = rows[-1]
    elapsed = number(row.get("execution_time_sec"))
    operations = number(row.get("operations"))
    return (elapsed * 1000.0 if elapsed is not None else None,
            number(row.get("throughput_ops_sec")), int(operations) if operations is not None else None)


def profile_values(rows: list[dict[str, str]], component: str, pid: int | None = None) -> list[float]:
    values = []
    for row in rows:
        if row.get("component") != component or (pid is not None and row.get("pid") != str(pid)):
            continue
        value = number(row.get("duration_us"))
        if value is not None:
            values.append(value)
    return values


def profile_counter(rows: list[dict[str, str]], component: str, pid: int | None = None,
                    status: str | None = None) -> list[float]:
    values = []
    for row in rows:
        if row.get("component") != component or (pid is not None and row.get("pid") != str(pid)):
            continue
        if status is not None and row.get("status") != status:
            continue
        value = number(row.get("metric_value"))
        if value is not None:
            values.append(value)
    return values


def job_metrics(rows: list[dict[str, str]], pid: int) -> dict[str, object]:
    jobs: dict[str, dict[str, dict[str, str]]] = defaultdict(dict)
    for row in rows:
        if row.get("pid") == str(pid) and row.get("job_id"):
            jobs[row["job_id"]][row.get("component", "")] = row
    queue_waits = profile_values(rows, "queue_wait", pid)
    if not queue_waits:
        for events in jobs.values():
            submitted, started = events.get("job_submission"), events.get("job_started")
            if submitted and started:
                queue_waits.append((int(started["start_ns"]) - int(submitted["start_ns"])) / 1000.0)
    completion = []
    for events in jobs.values():
        submitted, completed = events.get("job_submission"), events.get("job_completed")
        if submitted and completed:
            completion.append((int(completed["end_ns"]) - int(submitted["start_ns"])) / 1000.0)
    waits = profile_values(rows, "pipeline_serial_wait", pid)
    completions = profile_counter(rows, "pipeline_completion", pid)
    pipeline_outcomes = [row.get("status", "UNKNOWN") for row in rows
                         if row.get("component") == "pipeline_completion" and row.get("pid") == str(pid)]
    entries = sorted(profile_values(rows, "coordinator_application_total", pid))
    coordinator_events = sorted((int(row["end_ns"]) for row in rows if row.get("component") == "coordinator_application_total" and row.get("pid") == str(pid) and row.get("status") == "OK"))
    intervals = [(later - earlier) / 1_000_000.0 for earlier, later in zip(coordinator_events, coordinator_events[1:])]
    return {
        "monitor_submissions": sum(1 for events in jobs.values() if events.get("job_submission", {}).get("status") == "OK"),
        "monitor_completions": sum(1 for events in jobs.values() if events.get("job_completed", {}).get("status") == "OK"),
        "pipeline_completions": int(sum(completions)),
        "mean_queue_wait_us": mean(queue_waits), "median_queue_wait_us": median(queue_waits), "max_queue_wait_us": maximum(queue_waits),
        "mean_eligibility_wait_us": mean(profile_values(rows, "monitor_eligibility_wait", pid)),
        "max_eligibility_wait_us": maximum(profile_values(rows, "monitor_eligibility_wait", pid)),
        "mean_worker_execution_us": mean(profile_values(rows, "worker_execution", pid)),
        "mean_phase3_us": mean(profile_values(rows, "monitor_execution", pid)),
        "mean_pipeline_serial_wait_us": mean(waits), "max_pipeline_serial_wait_us": maximum(waits),
        "mean_phase4_us": mean(profile_values(rows, "phase4_execution", pid)),
        "mean_phase5_us": mean(profile_values(rows, "phase5_execution", pid)),
        "mean_phase6_us": mean(profile_values(rows, "phase6_execution", pid)),
        "mean_completion_latency_us": mean(completion), "max_completion_latency_us": maximum(completion),
        "evaluation_interval_observed_ms": mean(intervals), "max_evaluation_interval_ms": maximum(intervals),
        "eligible_opportunities": len(profile_values(rows, "monitor_eligibility_wait", pid)),
        "coordinator_event_count": len(entries), "pipeline_outcome": pipeline_outcomes[-1] if pipeline_outcomes else None,
    }


def mean(values: list[float]) -> float | None:
    return statistics.mean(values) if values else None


def median(values: list[float]) -> float | None:
    return statistics.median(values) if values else None


def maximum(values: list[float]) -> float | None:
    return max(values) if values else None


def status_outcome(runtime_status: str) -> str:
    return {
        "INSUFFICIENT": "INSUFFICIENT",
        "REJECTED": "VALIDATION_REJECTED",
        "MIGRATION_METADATA_UNAVAILABLE": "VALIDATION_APPROVED",
        "FEEDBACK_PENDING": "FEEDBACK_PENDING",
    }.get(runtime_status, runtime_status or "UNKNOWN")


class Evaluation:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.raw: list[dict[str, str]] = []
        self.runs: list[dict[str, str]] = []
        self.events: list[dict[str, str]] = []
        self.calibration: list[dict[str, str]] = []
        self.log_path = LOGS / "multi_application_performance.log"
        RESULTS.mkdir(parents=True, exist_ok=True)
        LOGS.mkdir(parents=True, exist_ok=True)
        self.log = self.log_path.open("w", encoding="utf-8")

    def note(self, message: str) -> None:
        self.log.write(message.rstrip() + "\n")
        self.log.flush()

    def close(self) -> None:
        self.log.close()

    def build(self) -> bool:
        result = subprocess.run(["make", "profile-awavma-runtime"], cwd=ROOT, capture_output=True, text=True)
        self.note(f"BUILD profile-awavma-runtime returncode={result.returncode}")
        self.note(result.stdout)
        self.note(result.stderr)
        required = (ROOT / "bin/benchmark", ROOT / "bin/profile-awavma-runtime", ROOT / "bin/classifier", ROOT / "bin/decision", ROOT / "bin/validation")
        return result.returncode == 0 and all(path.is_file() and os.access(path, os.X_OK) for path in required)

    def calibrate(self) -> int:
        if self.args.iterations is not None:
            self.calibration.append({"attempt": "override", "iterations": str(self.args.iterations), "elapsed_ms": "NA", "status": "FIXED"})
            return self.args.iterations
        with tempfile.TemporaryDirectory(prefix="awavma-multi-calibration-") as temporary:
            root = Path(temporary)
            iterations = 5_000_000
            for attempt in range(1, 4):
                output = root / f"calibration-{attempt}.csv"
                command = [str(ROOT / "bin/benchmark"), "--threads", str(THREADS), "--memory", str(MEMORY_MB),
                           "--iterations", str(iterations), "--pattern", PRIMARY_WORKLOAD, "--seed", "12345", "--output", str(output)]
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=30)
                elapsed, _, _ = benchmark_result(output)
                self.calibration.append({"attempt": str(attempt), "iterations": str(iterations),
                                         "elapsed_ms": text(elapsed), "status": "MEASURED" if result.returncode == 0 and elapsed else "FAILED"})
                if result.returncode != 0 or elapsed is None or elapsed <= 0:
                    raise RuntimeError("calibration benchmark failed")
                scaled = max(1, int(round(iterations * TARGET_SECONDS * 1000.0 / elapsed)))
                if 2000.0 <= elapsed <= 5000.0:
                    return iterations
                iterations = scaled
            return iterations

    def launch_benchmarks(self, root: Path, workloads: list[str], iterations: int) -> tuple[list[subprocess.Popen[str]], list[Path], list[int]]:
        processes, outputs, launch_ns = [], [], []
        for index, workload in enumerate(workloads):
            output = root / f"benchmark-{index}.csv"
            command = [str(ROOT / "bin/benchmark"), "--threads", str(THREADS), "--memory", str(MEMORY_MB),
                       "--iterations", str(iterations), "--pattern", workload, "--seed", str(12345 + index), "--output", str(output)]
            launch_ns.append(time.monotonic_ns())
            processes.append(subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
            outputs.append(output)
        return processes, outputs, launch_ns

    def run_group(self, run_id: str, run_type: str, mode: str, app_count: int, workers: int,
                  workloads: list[str], iterations: int) -> None:
        notes: list[str] = []
        with tempfile.TemporaryDirectory(prefix=f"awavma-multi-{run_id}-") as temporary:
            root = Path(temporary)
            processes, outputs, launched = self.launch_benchmarks(root, workloads, iterations)
            pids = [process.pid for process in processes]
            identities: dict[int, int] = {}
            for pid in pids:
                sample = proc_sample(pid)
                if sample is not None:
                    identities[pid] = int(sample["start_time_ticks"])
            runtime: subprocess.Popen[str] | None = None
            profile_path = root / "profile.csv"
            if mode == "INTEGRATED":
                runtime_command = [str(ROOT / "bin/profile-awavma-runtime"), "--duration-ms", str(self.args.runtime_duration_ms),
                                   "--evaluation-ms", str(EVALUATION_MS), "--monitor-ms", str(MONITOR_MS),
                                   "--discovery-interval-ms", str(DISCOVERY_MS), "--workers", str(workers),
                                   "--queue-capacity", str(QUEUE_CAPACITY), "--root-dir", str(root / "runtime")]
                for pid in pids:
                    runtime_command.extend(("--pid", str(pid)))
                environment = os.environ.copy()
                environment["AWAVMA_PROFILE_PATH"] = str(profile_path)
                runtime = subprocess.Popen(runtime_command, cwd=ROOT, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            benchmark_samples: dict[int, list[dict[str, float]]] = {pid: [] for pid in pids}
            group_samples: list[dict[int, dict[str, float]]] = []
            runtime_samples: list[dict[str, float]] = []
            tree_samples: list[dict[int, dict[str, float]]] = []
            benchmark_exited_ns: dict[int, int] = {}
            runtime_stop_sent = False
            deadline = time.monotonic() + self.args.timeout_seconds
            while time.monotonic() < deadline and (any(process.poll() is None for process in processes) or (runtime is not None and runtime.poll() is None)):
                group: dict[int, dict[str, float]] = {}
                for pid in pids:
                    if (sample := proc_sample(pid)) is not None:
                        benchmark_samples[pid].append(sample)
                        group[pid] = sample
                if group:
                    group_samples.append(group)
                if runtime is not None and (sample := proc_sample(runtime.pid)) is not None:
                    runtime_samples.append(sample)
                    tree = {runtime.pid: sample}
                    for child in process_children(runtime.pid):
                        if (child_sample := proc_sample(child)) is not None:
                            tree[child] = child_sample
                    tree_samples.append(tree)
                for process in processes:
                    if process.poll() is not None and process.pid not in benchmark_exited_ns:
                        benchmark_exited_ns[process.pid] = time.monotonic_ns()
                if len(benchmark_exited_ns) == len(processes) and runtime is not None and runtime.poll() is None and not runtime_stop_sent:
                    runtime.send_signal(signal.SIGTERM)
                    runtime_stop_sent = True
                time.sleep(SAMPLE_PERIOD_SECONDS)
            for index, process in enumerate(processes):
                terminate(process, f"benchmark-{index}", notes)
                benchmark_exited_ns.setdefault(process.pid, time.monotonic_ns())
            if runtime is not None:
                terminate(runtime, "runtime", notes)
            stderr = []
            for process in processes:
                _, error = process.communicate()
                if error.strip():
                    stderr.append(error.strip())
            runtime_error = ""
            if runtime is not None:
                _, runtime_error = runtime.communicate()
            event_rows = read_csv(profile_path) if mode == "INTEGRATED" else []
            for event in event_rows:
                self.events.append({"run_id": run_id, "run_type": run_type, "mode": mode,
                                    "application_count": str(app_count), "worker_count": str(workers), **event})
            runtime_rows = read_csv(root / "runtime" / "runtime_results.csv") if mode == "INTEGRATED" else []
            runtime_by_pid = {int(row["pid"]): row for row in runtime_rows if row.get("pid", "").isdigit()}
            group_resource = group_metrics(group_samples)
            runtime_resource = metrics(runtime_samples)
            tree_resource = group_metrics(tree_samples)
            makespan_ms = (max(benchmark_exited_ns.values()) - min(launched)) / 1_000_000.0 if launched and benchmark_exited_ns else None
            operations = []
            benchmark_data = []
            for output in outputs:
                elapsed, throughput, operation_count = benchmark_result(output)
                benchmark_data.append((elapsed, throughput, operation_count))
                if operation_count is not None:
                    operations.append(operation_count)
            aggregate = sum(operations) / (makespan_ms / 1000.0) if operations and makespan_ms and makespan_ms > 0 else None
            discovery = profile_values(event_rows, "application_discovery_scan")
            queue_depth = profile_counter(event_rows, "queue_depth")
            deferred = profile_counter(event_rows, "queue_deferred")
            saturation = profile_counter(event_rows, "queue_saturation")
            run_status = "MEASURED"
            if any(process.returncode != 0 for process in processes):
                run_status = "FAILED_BENCHMARK"
            elif mode == "INTEGRATED" and (runtime is None or runtime.returncode != 0):
                run_status = "FAILED_RUNTIME"
            elif mode == "INTEGRATED" and any(pid not in runtime_by_pid for pid in pids):
                run_status = "TARGET_NOT_DISCOVERED"
            if stderr:
                notes.extend(stderr)
            if runtime_error.strip():
                notes.append(runtime_error.strip())
            self.runs.append({"run_id": run_id, "run_type": run_type, "mode": mode,
                              "application_count": str(app_count), "worker_count": str(workers),
                               "iterations": str(iterations), "group_makespan_ms": text(makespan_ms),
                               "aggregate_throughput": text(aggregate), "runtime_cpu_percent": text(runtime_resource["cpu_percent"] if mode == "INTEGRATED" else None),
                               "runtime_cpu_peak_percent": text(runtime_resource["cpu_peak_percent"] if mode == "INTEGRATED" else None),
                              "runtime_rss_kb": text(runtime_resource["rss_kb"] if mode == "INTEGRATED" else None),
                              "runtime_vms_kb": text(runtime_resource["vms_kb"] if mode == "INTEGRATED" else None),
                              "runtime_tree_cpu_percent": text(tree_resource["cpu_percent"] if mode == "INTEGRATED" else None),
                              "runtime_tree_rss_kb": text(tree_resource["rss_kb"] if mode == "INTEGRATED" else None),
                              "runtime_tree_vms_kb": text(tree_resource["vms_kb"] if mode == "INTEGRATED" else None),
                              "max_queue_depth": text(maximum(queue_depth)), "deferred_jobs": text(sum(deferred) if deferred else 0),
                              "saturation_events": text(sum(saturation) if saturation else 0),
                              "discovery_scan_count": text(len(discovery)), "discovery_total_us": text(sum(discovery) if discovery else None),
                              "status": run_status, "notes": "; ".join(notes)})
            skew_us = (max(launched) - min(launched)) / 1000.0 if launched else None
            for index, pid in enumerate(pids):
                elapsed, throughput, _ = benchmark_data[index]
                runtime_row = runtime_by_pid.get(pid, {})
                app_metrics = job_metrics(event_rows, pid) if mode == "INTEGRATED" else {}
                runtime_status = runtime_row.get("status", "NOT_RUN") if mode == "INTEGRATED" else "NOT_RUN"
                status = run_status if run_status != "MEASURED" else ("MEASURED" if mode == "BASELINE" or runtime_status not in ("ERROR", "AMBIGUOUS") else runtime_status)
                self.raw.append({
                    "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "run_id": run_id,
                    "run_type": run_type, "mode": mode, "application_count": str(app_count),
                    "worker_count": str(workers), "application_index": str(index),
                    "app_id": runtime_row.get("app_id", "NA"), "pid": str(pid),
                    "start_time_ticks": str(identities.get(pid, "NA")), "workload": workloads[index],
                    "threads": str(THREADS), "memory_mb": str(MEMORY_MB), "iterations": str(iterations),
                    "seed": str(12345 + index), "launch_monotonic_ns": str(launched[index]), "launch_skew_us": text(skew_us),
                    "benchmark_elapsed_ms": text(elapsed), "benchmark_throughput": text(throughput),
                    "benchmark_cpu_percent": text(metrics(benchmark_samples[pid])["cpu_percent"]),
                    "benchmark_rss_kb": text(metrics(benchmark_samples[pid])["rss_kb"]),
                    "benchmark_vms_kb": text(metrics(benchmark_samples[pid])["vms_kb"]),
                    "aggregate_throughput": text(aggregate), "group_makespan_ms": text(makespan_ms),
                     "runtime_cpu_percent": text(runtime_resource["cpu_percent"] if mode == "INTEGRATED" else None),
                     "runtime_cpu_peak_percent": text(runtime_resource["cpu_peak_percent"] if mode == "INTEGRATED" else None),
                    "runtime_rss_kb": text(runtime_resource["rss_kb"] if mode == "INTEGRATED" else None),
                    "runtime_vms_kb": text(runtime_resource["vms_kb"] if mode == "INTEGRATED" else None),
                    "runtime_tree_cpu_percent": text(tree_resource["cpu_percent"] if mode == "INTEGRATED" else None),
                    "runtime_tree_rss_kb": text(tree_resource["rss_kb"] if mode == "INTEGRATED" else None),
                    "runtime_tree_vms_kb": text(tree_resource["vms_kb"] if mode == "INTEGRATED" else None),
                    **{key: text(value) for key, value in app_metrics.items() if key not in ("coordinator_event_count", "pipeline_outcome")},
                    "max_evaluation_interval_observed_ms": text(app_metrics.get("max_evaluation_interval_ms")),
                    "max_queue_depth": text(maximum(queue_depth)), "deferred_jobs": text(sum(deferred) if deferred else 0),
                    "saturation_events": text(sum(saturation) if saturation else 0),
                    "discovery_scan_count": text(len(discovery)), "discovery_total_us": text(sum(discovery) if discovery else None),
                    "discovery_mean_us": text(mean(discovery)), "runtime_cycle_total_us": text(mean(profile_values(event_rows, "runtime_cycle_total"))),
                    "status": status, "outcome": app_metrics.get("pipeline_outcome") or status_outcome(runtime_status), "notes": "; ".join(notes),
                })

    def run_primary(self, iterations: int) -> None:
        for app_count in APP_COUNTS:
            for workers in WORKER_COUNTS:
                workloads = [PRIMARY_WORKLOAD] * app_count
                for index in range(1, self.args.warmups + self.args.measured + 1):
                    run_type = "WARMUP" if index <= self.args.warmups else "MEASURED"
                    order = ("BASELINE", "INTEGRATED") if index % 2 else ("INTEGRATED", "BASELINE")
                    for mode in order:
                        run_id = f"primary-a{app_count}-w{workers}-{run_type.lower()}-{index}-{mode.lower()}"
                        self.note(f"RUN {run_id}")
                        self.run_group(run_id, run_type, mode, app_count, workers, workloads, iterations)

    def run_heterogeneous(self, iterations: int) -> None:
        if self.args.skip_heterogeneous:
            return
        for index in range(1, self.args.warmups + self.args.measured + 1):
            run_type = "WARMUP" if index <= self.args.warmups else "MEASURED"
            workloads = list(HETEROGENEOUS_WORKLOADS[index % len(HETEROGENEOUS_WORKLOADS):] + HETEROGENEOUS_WORKLOADS[:index % len(HETEROGENEOUS_WORKLOADS)])
            for mode in (("BASELINE", "INTEGRATED") if index % 2 else ("INTEGRATED", "BASELINE")):
                run_id = f"heterogeneous-a4-w2-{run_type.lower()}-{index}-{mode.lower()}"
                self.note(f"RUN {run_id}")
                self.run_group(run_id, run_type, mode, 4, 2, workloads, iterations)


def measured(rows: list[dict[str, str]], **filters: str) -> list[dict[str, str]]:
    return [row for row in rows if row.get("run_type") == "MEASURED" and row.get("status") == "MEASURED" and all(row.get(key) == value for key, value in filters.items())]


def summarize(raw: list[dict[str, str]], runs: list[dict[str, str]]) -> tuple[list[dict[str, str]], list[dict[str, str]], list[dict[str, str]], list[dict[str, str]]]:
    summary: list[dict[str, str]] = []
    for scope, rows, metrics_for_scope in (("APPLICATION", raw, ("benchmark_elapsed_ms", "benchmark_throughput", "mean_queue_wait_us", "mean_completion_latency_us", "mean_pipeline_serial_wait_us", "mean_phase3_us", "mean_phase4_us", "mean_phase5_us", "mean_phase6_us")),
                                           ("RUN", runs, ("aggregate_throughput", "group_makespan_ms", "runtime_cpu_percent", "runtime_rss_kb", "runtime_vms_kb", "max_queue_depth", "deferred_jobs", "saturation_events", "discovery_total_us"))):
        groups = {(row["application_count"], row["worker_count"], row["mode"]) for row in rows}
        for app_count, workers, mode in sorted(groups, key=lambda item: tuple(map(int, item[:2])) + (item[2],)):
            subset = measured(rows, application_count=app_count, worker_count=workers, mode=mode)
            for metric in metrics_for_scope:
                values = [number(row.get(metric)) for row in subset]
                values = [value for value in values if value is not None]
                summary.append({"scope": scope, "application_count": app_count, "worker_count": workers, "mode": mode, "metric": metric,
                                "count": str(len(values)), "mean": text(mean(values)), "median": text(median(values)), "min": text(min(values) if values else None),
                                "max": text(max(values) if values else None), "stddev": text(statistics.stdev(values) if len(values) > 1 else None),
                                "status": "MEASURED" if values else "UNAVAILABLE"})
    fairness = []
    scaling = []
    for app_count in APP_COUNTS:
        for workers in WORKER_COUNTS:
            for mode in ("BASELINE", "INTEGRATED"):
                run_rows = measured(runs, application_count=str(app_count), worker_count=str(workers), mode=mode)
                app_rows = measured(raw, application_count=str(app_count), worker_count=str(workers), mode=mode)
                fairness_values = []
                for run_id in sorted({row["run_id"] for row in app_rows}):
                    per_app = sorted([row for row in app_rows if row["run_id"] == run_id], key=lambda row: int(row["application_index"]))
                    cycles = [number(row.get("pipeline_completions")) or 0.0 for row in per_app]
                    jain = sum(cycles) ** 2 / (len(cycles) * sum(value * value for value in cycles)) if cycles and sum(value * value for value in cycles) > 0 else None
                    if jain is not None:
                        fairness_values.append(jain)
                    completed = [value for value in cycles if value > 0]
                    peers_complete = max(cycles, default=0) > 0
                    for row, cycle in zip(per_app, cycles):
                        eligible = number(row.get("eligible_opportunities")) or 0.0
                        submitted = number(row.get("monitor_submissions")) or 0.0
                        starvation = "POTENTIAL" if peers_complete and cycle == 0 and eligible - submitted >= 2 else "NONE"
                        fairness.append({"configuration": f"apps={app_count};workers={workers};mode={mode}", "run_id": run_id,
                                         "application_count": str(app_count), "worker_count": str(workers), "application_index": row["application_index"],
                                         "app_id": row["app_id"], "completed_cycles": text(cycle), "mean_queue_wait_us": row.get("mean_queue_wait_us", "NA"),
                                         "max_queue_wait_us": row.get("max_queue_wait_us", "NA"), "mean_evaluation_interval_ms": row.get("evaluation_interval_observed_ms", "NA"),
                                         "max_evaluation_interval_ms": row.get("max_evaluation_interval_observed_ms", "NA"), "starvation_evidence": starvation,
                                         "jain_fairness_index": text(jain), "status": "MEASURED" if completed else "UNAVAILABLE"})
                scaling.append({"mode": mode, "application_count": str(app_count), "worker_count": str(workers),
                                "aggregate_throughput": text(mean([number(row.get("aggregate_throughput")) for row in run_rows if number(row.get("aggregate_throughput")) is not None])),
                                "mean_application_elapsed_ms": text(mean([number(row.get("benchmark_elapsed_ms")) for row in app_rows if number(row.get("benchmark_elapsed_ms")) is not None])),
                                "runtime_cpu_percent": text(mean([number(row.get("runtime_cpu_percent")) for row in run_rows if number(row.get("runtime_cpu_percent")) is not None])),
                                "runtime_rss_kb": text(mean([number(row.get("runtime_rss_kb")) for row in run_rows if number(row.get("runtime_rss_kb")) is not None])),
                                "runtime_vms_kb": text(mean([number(row.get("runtime_vms_kb")) for row in run_rows if number(row.get("runtime_vms_kb")) is not None])),
                                "mean_queue_wait_us": text(mean([number(row.get("mean_queue_wait_us")) for row in app_rows if number(row.get("mean_queue_wait_us")) is not None])),
                                "max_queue_wait_us": text(max([number(row.get("max_queue_wait_us")) for row in app_rows if number(row.get("max_queue_wait_us")) is not None], default=None)),
                                "max_queue_depth": text(max([number(row.get("max_queue_depth")) for row in run_rows if number(row.get("max_queue_depth")) is not None], default=None)),
                                "deferred_jobs": text(mean([number(row.get("deferred_jobs")) for row in run_rows if number(row.get("deferred_jobs")) is not None])),
                                "saturation_events": text(mean([number(row.get("saturation_events")) for row in run_rows if number(row.get("saturation_events")) is not None])),
                                "mean_serial_wait_us": text(mean([number(row.get("mean_pipeline_serial_wait_us")) for row in app_rows if number(row.get("mean_pipeline_serial_wait_us")) is not None])),
                                "max_serial_wait_us": text(max([number(row.get("max_pipeline_serial_wait_us")) for row in app_rows if number(row.get("max_pipeline_serial_wait_us")) is not None], default=None)),
                                "mean_completion_latency_us": text(mean([number(row.get("mean_completion_latency_us")) for row in app_rows if number(row.get("mean_completion_latency_us")) is not None])),
                                "fairness_index": text(mean(fairness_values)), "status": "MEASURED" if run_rows else "UNAVAILABLE"})
    status_counts = []
    counts = Counter(row.get("outcome", "UNKNOWN") for row in raw if row.get("run_type") == "MEASURED" and row.get("mode") == "INTEGRATED")
    for name in ("INSUFFICIENT", "NO_MIGRATION", "VALIDATION_REJECTED", "VALIDATION_APPROVED", "MIGRATION_EXECUTED", "FEEDBACK_PENDING", "FEEDBACK_APPLIED"):
        status_counts.append({"outcome": name, "count": str(counts.get(name, 0)), "status": "MEASURED" if counts.get(name, 0) else "NOT EXECUTED"})
    return summary, scaling, fairness, status_counts


def report(iterations: int, scaling: list[dict[str, str]], fairness: list[dict[str, str]], runs: list[dict[str, str]], raw: list[dict[str, str]]) -> None:
    integrated = [row for row in scaling if row["mode"] == "INTEGRATED"]
    by_config = {(row["application_count"], row["worker_count"]): row for row in integrated}
    lines = ["# Multi-Application Performance Evaluation", "", "## Methodology", "",
             f"- CPUs: {os.cpu_count() or 0}", "- Primary workload: mixed benchmark-generation pattern.",
             f"- Threads/application: {THREADS}; memory/application: {MEMORY_MB} MiB; iterations/thread: {iterations}.",
             f"- Discovery interval: {DISCOVERY_MS} ms; monitor interval: {MONITOR_MS} ms; evaluation interval: {EVALUATION_MS} ms.",
             "- Warm-ups and measured repetitions are recorded in the raw CSV. Launches are sequential with measured skew because the frozen benchmark has no external pre-execution barrier.", "",
             "## Application Scaling", "", "| Apps | Workers | Aggregate Throughput | AWAVMA CPU | Queue Wait | Serial Wait | Completion Latency |", "|---:|---:|---:|---:|---:|---:|---:|"]
    for apps in APP_COUNTS:
        for workers in WORKER_COUNTS:
            row = by_config.get((str(apps), str(workers)), {})
            lines.append(f"| {apps} | {workers} | {row.get('aggregate_throughput', 'NA')} | {row.get('runtime_cpu_percent', 'NA')} | {row.get('mean_queue_wait_us', 'NA')} | {row.get('mean_serial_wait_us', 'NA')} | {row.get('mean_completion_latency_us', 'NA')} |")
    lines.extend(["", "## Queue Behavior", "", "| Apps | Workers | Max Queue Depth | Deferred | Saturation Events | Mean Wait | Max Wait |", "|---:|---:|---:|---:|---:|---:|---:|"])
    for apps in APP_COUNTS:
        for workers in WORKER_COUNTS:
            row = by_config.get((str(apps), str(workers)), {})
            lines.append(f"| {apps} | {workers} | {row.get('max_queue_depth', 'NA')} | {row.get('deferred_jobs', 'NA')} | {row.get('saturation_events', 'NA')} | {row.get('mean_queue_wait_us', 'NA')} | {row.get('max_queue_wait_us', 'NA')} |")
    lines.extend(["", "## Fairness", "", "| Apps | Workers | Jain Index | Min Cycles/App | Max Cycles/App | Worst Evaluation Gap |", "|---:|---:|---:|---:|---:|---:|"])
    for apps in APP_COUNTS:
        for workers in WORKER_COUNTS:
            rows = [row for row in fairness if row["application_count"] == str(apps) and row["worker_count"] == str(workers) and "mode=INTEGRATED" in row["configuration"]]
            cycles = [number(row.get("completed_cycles")) for row in rows]
            gaps = [number(row.get("max_evaluation_interval_ms")) for row in rows]
            scale = by_config.get((str(apps), str(workers)), {})
            lines.append(f"| {apps} | {workers} | {scale.get('fairness_index', 'NA')} | {text(min([v for v in cycles if v is not None], default=None))} | {text(max([v for v in cycles if v is not None], default=None))} | {text(max([v for v in gaps if v is not None], default=None))} |")
    lines.extend(["", "## AWAVMA Overhead", "", "| Apps | Workers | Baseline Aggregate Throughput | AWAVMA Aggregate Throughput | Change % |", "|---:|---:|---:|---:|---:|"])
    baseline = {(row["application_count"], row["worker_count"]): row for row in scaling if row["mode"] == "BASELINE"}
    for apps in APP_COUNTS:
        for workers in WORKER_COUNTS:
            before = number(baseline.get((str(apps), str(workers)), {}).get("aggregate_throughput"))
            after = number(by_config.get((str(apps), str(workers)), {}).get("aggregate_throughput"))
            change = (after - before) / before * 100.0 if before and after is not None else None
            lines.append(f"| {apps} | {workers} | {text(before)} | {text(after)} | {text(change)} |")
    lines.extend(["", "## Heterogeneous Follow-Up", "", "| Workload | Baseline Throughput | AWAVMA Throughput | Change % |", "|---|---:|---:|---:|"])
    for workload in HETEROGENEOUS_WORKLOADS:
        baseline_values = [number(row.get("benchmark_throughput")) for row in raw if row.get("run_type") == "MEASURED" and row.get("run_id", "").startswith("heterogeneous") and row.get("mode") == "BASELINE" and row.get("workload") == workload]
        integrated_values = [number(row.get("benchmark_throughput")) for row in raw if row.get("run_type") == "MEASURED" and row.get("run_id", "").startswith("heterogeneous") and row.get("mode") == "INTEGRATED" and row.get("workload") == workload]
        baseline_mean = mean([value for value in baseline_values if value is not None])
        integrated_mean = mean([value for value in integrated_values if value is not None])
        change = (integrated_mean - baseline_mean) / baseline_mean * 100.0 if baseline_mean and integrated_mean is not None else None
        lines.append(f"| {workload} | {text(baseline_mean)} | {text(integrated_mean)} | {text(change)} |")
    (RESULTS / "multi_application_performance_report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def environment(iterations: int) -> list[dict[str, str]]:
    nodes = sorted(path.name for path in Path("/sys/devices/system/node").glob("node[0-9]*"))
    return [{"key": "cpus", "value": str(os.cpu_count() or 0), "status": "MEASURED"},
            {"key": "numa_nodes", "value": ";".join(nodes), "status": "MEASURED"},
            {"key": "libnuma", "value": "available" if ctypes.util.find_library("numa") else "unavailable", "status": "MEASURED"},
            {"key": "benchmark", "value": "frozen existing binary; source rebuild requires unavailable numa.h", "status": "NOT TESTED - ENVIRONMENT LIMITATION"},
            {"key": "iterations_per_thread", "value": str(iterations), "status": "MEASURED"},
            {"key": "discovery_interval_ms", "value": str(DISCOVERY_MS), "status": "MEASURED"}]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--measured", type=int, default=5)
    parser.add_argument("--runtime-duration-ms", type=int, default=5000)
    parser.add_argument("--timeout-seconds", type=float, default=30.0)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--skip-heterogeneous", action="store_true")
    parser.add_argument("--summarize-only", action="store_true")
    args = parser.parse_args()
    if args.iterations is not None and args.iterations <= 0 or args.warmups < 0 or args.measured <= 0:
        return 1
    if args.quick:
        args.warmups, args.measured = 0, 1
        args.skip_heterogeneous = True
    if args.summarize_only:
        raw, runs = read_csv(RESULTS / "multi_application_performance_raw.csv"), read_csv(RESULTS / "multi_application_runs.csv")
        iterations = int(raw[0]["iterations"]) if raw else 0
        summary, scaling, fairness, statuses = summarize(raw, runs)
        write_csv(RESULTS / "multi_application_performance_summary.csv", SUMMARY_FIELDS, summary)
        write_csv(RESULTS / "multi_application_scaling.csv", SCALING_FIELDS, scaling)
        write_csv(RESULTS / "multi_application_fairness.csv", FAIRNESS_FIELDS, fairness)
        write_csv(RESULTS / "multi_application_status_counts.csv", ("outcome", "count", "status"), statuses)
        report(iterations, scaling, fairness, runs, raw)
    else:
        evaluation = Evaluation(args)
        try:
            if not evaluation.build():
                print("Multi-application performance: required binaries unavailable", file=sys.stderr)
                return 1
            iterations = evaluation.calibrate()
            evaluation.run_primary(iterations)
            evaluation.run_heterogeneous(iterations)
            summary, scaling, fairness, statuses = summarize(evaluation.raw, evaluation.runs)
            write_csv(RESULTS / "multi_application_performance_raw.csv", RAW_FIELDS, evaluation.raw)
            write_csv(RESULTS / "multi_application_runs.csv", RUN_FIELDS, evaluation.runs)
            write_csv(RESULTS / "multi_application_profile_events.csv", PROFILE_FIELDS, evaluation.events)
            write_csv(RESULTS / "multi_application_calibration.csv", ("attempt", "iterations", "elapsed_ms", "status"), evaluation.calibration)
            write_csv(RESULTS / "multi_application_performance_summary.csv", SUMMARY_FIELDS, summary)
            write_csv(RESULTS / "multi_application_scaling.csv", SCALING_FIELDS, scaling)
            write_csv(RESULTS / "multi_application_fairness.csv", FAIRNESS_FIELDS, fairness)
            write_csv(RESULTS / "multi_application_status_counts.csv", ("outcome", "count", "status"), statuses)
            write_csv(RESULTS / "multi_application_environment.csv", ("key", "value", "status"), environment(iterations))
            report(iterations, scaling, fairness, evaluation.runs, evaluation.raw)
        finally:
            evaluation.close()
    graph = subprocess.run([sys.executable, str(ROOT / "scripts/generate_multi_application_performance_graphs.py"), "--root", str(ROOT)], cwd=ROOT)
    print(f"Multi-application performance: raw_rows={len(read_csv(RESULTS / 'multi_application_performance_raw.csv'))} graph_returncode={graph.returncode}")
    return graph.returncode


if __name__ == "__main__":
    raise SystemExit(main())
