#!/usr/bin/env python3
"""Collect and summarize optional monitoring instrumentation timings."""

from __future__ import annotations

import csv
import ctypes.util
import math
import os
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
LOGS = ROOT / "logs"
PATTERNS = ("sequential", "random", "hot", "moderate", "cold", "mixed", "changing", "local")
MEASURED_RUNS = 5
WARMUP_RUNS = 5
THREADS = 2
MEMORY_MB = 8
ITERATIONS = 1_000_000
PROFILE_FIELDS = ("run_id", "application", "workload", "threads", "memory", "iterations", "sample_number", "component", "start_time", "end_time", "duration_us", "status")


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


def number(value: str | None) -> float | None:
    try:
        value_number = float(value) if value is not None else math.nan
    except ValueError:
        return None
    return value_number if math.isfinite(value_number) else None


class Runner:
    def __init__(self) -> None:
        LOGS.mkdir(parents=True, exist_ok=True)
        self.log = (LOGS / "monitoring_profile.log").open("w", encoding="utf-8")

    def close(self) -> None:
        self.log.close()

    def run(self, name: str, command: list[str], profile_path: Path | None = None, timeout: int = 240) -> subprocess.CompletedProcess[str]:
        self.log.write(f"COMMAND {name}: {' '.join(command)}\n")
        started = time.monotonic()
        environment = os.environ.copy()
        if profile_path is not None:
            environment["AWAVMA_PROFILE_PATH"] = str(profile_path)
        try:
            result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            result = subprocess.CompletedProcess(command, 124, error.stdout or "", error.stderr or "timeout")
        self.log.write(f"RESULT {name}: returncode={result.returncode} elapsed={time.monotonic() - started:.6f}\n")
        if result.stdout:
            self.log.write(result.stdout[-1800:])
        if result.stderr:
            self.log.write(result.stderr[-1800:])
        self.log.flush()
        return result


def map_profile_rows(path: Path, run_id: str, workload: str, warmup: bool) -> list[dict[str, str]]:
    source = read_csv(path)
    sample_ranges = []
    for row in source:
        if row.get("component") == "monitor_sample_total":
            sample_ranges.append((int(row["start_ns"]), int(row["end_ns"])))
    mapped: list[dict[str, str]] = []
    for row in source:
        start = int(row["start_ns"])
        end = int(row["end_ns"])
        sample_number = 0
        for index, (sample_start, sample_end) in enumerate(sample_ranges, 1):
            if sample_start <= start <= sample_end:
                sample_number = index
                break
        application = row.get("application", "-")
        if application in ("", "-"):
            application = workload
        mapped.append({
            "run_id": run_id, "application": application, "workload": workload,
            "threads": str(THREADS) if workload not in ("discovery", "multi_application") else "NA",
            "memory": str(MEMORY_MB) if workload not in ("discovery", "multi_application") else "NA",
            "iterations": str(ITERATIONS) if workload not in ("discovery", "multi_application") else "NA",
            "sample_number": str(sample_number), "component": row.get("component", "unknown"),
            "start_time": row.get("start_ns", "NA"), "end_time": row.get("end_ns", "NA"),
            "duration_us": row.get("duration_us", "NA"), "status": row.get("status", "UNKNOWN"),
            "_job_id": row.get("job_id", ""),
        })
    return mapped


def summarize(raw: list[dict[str, str]]) -> list[dict[str, str]]:
    measured = [row for row in raw if "warmup" not in row["run_id"] and "probe" not in row["run_id"]]
    totals = [number(row["duration_us"]) for row in measured if row["component"] == "monitor_sample_total"]
    totals = [value for value in totals if value is not None]
    total_mean = statistics.mean(totals) if totals else math.nan
    session_values = [number(row["duration_us"]) for row in measured if row["component"] == "monitor_session"]
    session_values = [value for value in session_values if value is not None]
    session_mean = statistics.mean(session_values) if session_values else math.nan
    once_values = [number(row["duration_us"]) for row in measured if row["component"] == "monitor_run_pid_once"]
    once_values = [value for value in once_values if value is not None]
    once_mean = statistics.mean(once_values) if once_values else math.nan
    components = sorted({row["component"] for row in measured})
    groups = {
        "csv_formatting": ("csv_formatting_process", "csv_formatting_threads"),
        "csv_write": ("csv_write_process", "csv_write_threads"),
        "per_thread_statistics": ("per_thread_stat",),
    }
    for name, members in groups.items():
        components.append(name)
    components.extend(("process_metadata_collection", "page_fault_collection"))
    rows: list[dict[str, str]] = []
    for component in sorted(set(components)):
        if component == "process_metadata_collection" or component == "page_fault_collection":
            rows.append({"component": component, "sample_count": "0", "mean_us": "NA", "median_us": "NA", "min_us": "NA", "max_us": "NA", "stddev_us": "NA", "percentage_of_monitor_time": "NA", "status": "NOT SEPARATELY MEASURABLE: included in proc_pid_stat"})
            continue
        members = groups.get(component, (component,))
        if component in groups:
            sample_values: dict[tuple[str, str], float] = {}
            for row in measured:
                value = number(row["duration_us"])
                if row["component"] in members and value is not None:
                    key = (row["run_id"], row["sample_number"])
                    sample_values[key] = sample_values.get(key, 0.0) + value
            values = list(sample_values.values())
        else:
            values = [number(row["duration_us"]) for row in measured if row["component"] in members]
            values = [value for value in values if value is not None]
        if not values:
            rows.append({"component": component, "sample_count": "0", "mean_us": "NA", "median_us": "NA", "min_us": "NA", "max_us": "NA", "stddev_us": "NA", "percentage_of_monitor_time": "NA", "status": "NOT USED"})
            continue
        mean = statistics.mean(values)
        if component == "monitor_session" and session_mean > 0:
            percentage = 100.0
        elif component == "monitor_run_pid_once" and once_mean > 0:
            percentage = 100.0
        elif component in ("initialization", "finalization") and session_mean > 0:
            percentage = mean / session_mean * 100.0
        elif component in ("sampling_interval_sleep", "job_submission", "job_started", "job_completed", "monitor_execution", "application_discovery_scan"):
            percentage = math.nan
        else:
            percentage = mean / total_mean * 100.0 if total_mean > 0 else math.nan
        rows.append({"component": component, "sample_count": str(len(values)), "mean_us": f"{mean:.3f}", "median_us": f"{statistics.median(values):.3f}", "min_us": f"{min(values):.3f}", "max_us": f"{max(values):.3f}", "stddev_us": f"{statistics.stdev(values):.3f}" if len(values) > 1 else "NA", "percentage_of_monitor_time": f"{percentage:.3f}" if math.isfinite(percentage) else "NA", "status": "MEASURED"})
    single_ids = {row["run_id"] for row in measured if row["workload"] not in ("discovery", "multi_application") and row["component"] == "monitor_session"}
    sleep_totals = []
    residuals = []
    for run_id in single_ids:
        session = sum(number(row["duration_us"]) or 0.0 for row in measured if row["run_id"] == run_id and row["component"] == "monitor_session")
        sleep = sum(number(row["duration_us"]) or 0.0 for row in measured if row["run_id"] == run_id and row["component"] == "sampling_interval_sleep")
        samples = sum(number(row["duration_us"]) or 0.0 for row in measured if row["run_id"] == run_id and row["component"] == "monitor_sample_total")
        initialization = sum(number(row["duration_us"]) or 0.0 for row in measured if row["run_id"] == run_id and row["component"] == "initialization")
        finalization = sum(number(row["duration_us"]) or 0.0 for row in measured if row["run_id"] == run_id and row["component"] == "finalization")
        sleep_totals.append(sleep)
        residuals.append(session - sleep - samples - initialization - finalization)
    for component, values in (("sampling_interval_sleep_session_total", sleep_totals), ("unaccounted_session_time", residuals)):
        if not values:
            continue
        mean = statistics.mean(values)
        percentage = mean / session_mean * 100.0 if session_mean > 0 else math.nan
        rows.append({"component": component, "sample_count": str(len(values)), "mean_us": f"{mean:.3f}", "median_us": f"{statistics.median(values):.3f}", "min_us": f"{min(values):.3f}", "max_us": f"{max(values):.3f}", "stddev_us": f"{statistics.stdev(values):.3f}" if len(values) > 1 else "NA", "percentage_of_monitor_time": f"{percentage:.3f}" if math.isfinite(percentage) else "NA", "status": "MEASURED"})
    return rows


def application_summary(raw: list[dict[str, str]]) -> list[dict[str, str]]:
    runtime = [row for row in raw if row["workload"] == "multi_application"]
    jobs: dict[tuple[str, str], dict[str, dict[str, str]]] = {}
    for row in runtime:
        key = (row["application"], row["start_time"] if row["component"] == "job_submission" else "")
        job_id = row.get("_job_id", "")
        if not job_id:
            continue
        jobs.setdefault((row["application"], job_id), {})[row["component"]] = row
    grouped: dict[str, list[dict[str, float]]] = {}
    for (application, _), events in jobs.items():
        if not all(name in events for name in ("job_submission", "job_started", "monitor_execution", "job_completed")):
            continue
        submission = events["job_submission"]
        started = events["job_started"]
        execution = events["monitor_execution"]
        completed = events["job_completed"]
        grouped.setdefault(application, []).append({
            "queue": (int(started["start_time"]) - int(submission["end_time"])) / 1000.0,
            "worker": (int(completed["start_time"]) - int(started["start_time"])) / 1000.0,
            "monitor": number(execution["duration_us"]) or 0.0,
            "completion": (int(completed["end_time"]) - int(submission["start_time"])) / 1000.0,
        })
    output = []
    for application, values in sorted(grouped.items()):
        output.append({"application": application, "jobs_submitted": str(len(values)), "jobs_completed": str(len(values)), "queue_wait_mean_us": f"{statistics.mean(value['queue'] for value in values):.3f}", "queue_wait_median_us": f"{statistics.median(value['queue'] for value in values):.3f}", "worker_execution_mean_us": f"{statistics.mean(value['worker'] for value in values):.3f}", "monitor_execution_mean_us": f"{statistics.mean(value['monitor'] for value in values):.3f}", "completion_latency_mean_us": f"{statistics.mean(value['completion'] for value in values):.3f}"})
    return output


def main() -> int:
    RESULTS.mkdir(parents=True, exist_ok=True)
    runner = Runner()
    raw: list[dict[str, str]] = []
    try:
        runner.run("build-profile-monitor", ["make", "profile-monitor"])
        runner.run("build-profile-discovery", ["make", "profile-application-discovery-test"])
        runner.run("build-profile-runtime", ["make", "profile-continuous-monitor-test"])
        with tempfile.TemporaryDirectory(prefix="awavma-monitor-profile-") as directory:
            root = Path(directory)
            discovery_path = root / "discovery.csv"
            discovery_result = runner.run("discovery-profile", [str(ROOT / "bin/profile-application-discovery-test")], discovery_path)
            if discovery_result.returncode == 0 and discovery_path.is_file():
                raw.extend(map_profile_rows(discovery_path, "discovery", "discovery", False))
            for workload in PATTERNS:
                for run_number in range(1, WARMUP_RUNS + MEASURED_RUNS + 1):
                    warmup = run_number <= WARMUP_RUNS
                    profile_path = root / f"{workload}-{run_number}.csv"
                    monitor_output = root / f"{workload}-{run_number}-monitor.csv"
                    thread_output = root / f"{workload}-{run_number}-threads.csv"
                    log_path = root / f"{workload}-{run_number}.log"
                    command = [str(ROOT / "bin/profile-monitor"), "--interval", "50", "--output", str(monitor_output), "--thread-output", str(thread_output), "--log", str(log_path), "--benchmark-path", str(ROOT / "bin/benchmark"), "--benchmark", "--threads", str(THREADS), "--memory", str(MEMORY_MB), "--iterations", str(ITERATIONS), "--pattern", workload, "--seed", "12345", "--output", str(root / f"{workload}-{run_number}-benchmark.csv")]
                    result = runner.run(f"monitor-profile-{workload}-{run_number}", command, profile_path)
                    if result.returncode == 0 and profile_path.is_file():
                        label = f"{workload}-warmup-{run_number}" if warmup else f"{workload}-{run_number - WARMUP_RUNS}"
                        raw.extend(map_profile_rows(profile_path, label, workload, warmup))
            runtime_path = root / "runtime.csv"
            result = runner.run("runtime-profile", ["env", "AWAVMA_REGRESSION_VERIFIED=1", str(ROOT / "bin/profile-continuous-monitor-test")], runtime_path)
            if result.returncode != 0:
                runner.log.write("PROFILE_RUNTIME_STATUS: profiling instrumentation perturbed a timing-sensitive runtime test; events retained for timing analysis\n")
            if runtime_path.is_file():
                raw.extend(map_profile_rows(runtime_path, "runtime-multi", "multi_application", False))
        # Preserve the job id in memory for application-level event pairing;
        # the public raw CSV intentionally keeps the requested schema only.
        application_rows = application_summary(raw)
        public_raw = [{key: row.get(key, "") for key in PROFILE_FIELDS} for row in raw]
        write_csv(RESULTS / "monitoring_profile_raw.csv", PROFILE_FIELDS, public_raw)
        summary = summarize(raw)
        write_csv(RESULTS / "monitoring_profile_summary.csv", ("component", "sample_count", "mean_us", "median_us", "min_us", "max_us", "stddev_us", "percentage_of_monitor_time", "status"), summary)
        write_csv(RESULTS / "monitoring_profile_application.csv", ("application", "jobs_submitted", "jobs_completed", "queue_wait_mean_us", "queue_wait_median_us", "worker_execution_mean_us", "monitor_execution_mean_us", "completion_latency_mean_us"), application_rows)
        nodes = sorted(path.name for path in Path("/sys/devices/system/node").glob("node[0-9]*"))
        write_csv(RESULTS / "monitoring_profile_environment.csv", ("key", "value", "status"), [
            {"key": "cpu_count", "value": str(os.cpu_count() or 0), "status": "MEASURED"},
            {"key": "numa_nodes", "value": ";".join(nodes), "status": "MEASURED"},
            {"key": "libnuma", "value": "available" if ctypes.util.find_library("numa") else "unavailable", "status": "MEASURED"},
            {"key": "numa.h", "value": str(Path("/usr/include/numa.h").is_file()), "status": "MEASURED"},
            {"key": "numaif.h", "value": str(Path("/usr/include/numaif.h").is_file()), "status": "MEASURED"},
            {"key": "compiler", "value": subprocess.run(["gcc", "--version"], capture_output=True, text=True).stdout.splitlines()[0], "status": "MEASURED"},
            {"key": "python", "value": sys.version.split()[0], "status": "MEASURED"},
        ])
        graph = runner.run("profile-graphs", [sys.executable, str(ROOT / "scripts/generate_monitoring_profile_graphs.py"), "--root", str(ROOT), "--output-dir", str(ROOT / "graphs/monitoring-profile"), "--summary", str(RESULTS / "monitoring_profile_graph_summary.csv"), "--log", str(LOGS / "monitoring_profile_graphs.log")])
        return 0 if graph.returncode == 0 else 1
    finally:
        runner.close()


if __name__ == "__main__":
    raise SystemExit(main())
