#!/usr/bin/env python3
"""Measure discovery interval cost and select a safe candidate interval."""

from __future__ import annotations

import csv
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
INTERVALS = (50, 100, 250, 500, 1000)
WARMUPS = 5
MEASURED = 5
DURATION_SECONDS = "1.2"
RAW_FIELDS = ("timestamp", "interval_ms", "mode", "run_type", "run_index", "benchmark_elapsed_ms",
              "benchmark_throughput_ops_sec", "runtime_cpu_percent", "runtime_rss_kb", "discovery_scans",
              "discovery_total_us", "discovery_mean_us", "runtime_cycle_total_us", "status", "notes")
SUMMARY_FIELDS = ("interval_ms", "mode", "metric", "count", "mean", "median", "min", "max", "stddev", "status")
DETECTION_FIELDS = ("interval_ms", "run_index", "latency_ms", "discovery_scans", "schedule_passes", "status")


def number(value: object) -> float | None:
    try:
        result = float(str(value))
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def text(value: float | None) -> str:
    return "NA" if value is None else f"{value:.3f}"


def read(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def write(path: Path, fields: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def proc_sample(pid: int) -> tuple[float, float, float] | None:
    try:
        stat = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
        fields = stat[stat.rfind(")") + 2:].split()
        ticks = os.sysconf("SC_CLK_TCK")
        page = os.sysconf("SC_PAGE_SIZE")
        return time.monotonic(), (int(fields[11]) + int(fields[12])) / ticks, int(fields[21]) * page / 1024.0
    except (FileNotFoundError, OSError, ValueError, IndexError):
        return None


def profile_metrics(path: Path) -> dict[str, float | None]:
    rows = read(path)
    values = lambda component: [number(row.get("duration_us")) for row in rows if row.get("component") == component]
    discovery = [value for value in values("application_discovery_scan") if value is not None]
    cycles = [value for value in values("runtime_cycle_total") if value is not None]
    return {"discovery_scans": float(len(discovery)), "discovery_total_us": sum(discovery) if discovery else None,
            "discovery_mean_us": statistics.mean(discovery) if discovery else None,
            "runtime_cycle_total_us": statistics.mean(cycles) if cycles else None}


def benchmark_metrics(path: Path) -> tuple[float | None, float | None]:
    rows = read(path)
    if not rows:
        return None, None
    row = rows[-1]
    elapsed = number(row.get("execution_time_sec"))
    return (elapsed * 1000.0 if elapsed is not None else None, number(row.get("throughput_ops_sec")))


def run_case(interval: int, mode: str, directory: Path) -> dict[str, str]:
    directory.mkdir(parents=True, exist_ok=True)
    benchmark_csv = directory / "benchmark.csv"
    benchmark_command = [str(ROOT / "bin/benchmark"), "--threads", "2", "--memory", "8", "--duration", DURATION_SECONDS,
                         "--pattern", "sequential", "--seed", "12345", "--output", str(benchmark_csv)]
    benchmark = subprocess.Popen(benchmark_command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    runtime = None
    profile = directory / "profile.csv"
    environment = os.environ.copy()
    if mode == "INTEGRATED":
        environment["AWAVMA_PROFILE_PATH"] = str(profile)
        runtime = subprocess.Popen([str(ROOT / "bin/profile-awavma-runtime"), "--duration-ms", "1200", "--evaluation-ms", "1200",
                                    "--discovery-interval-ms", str(interval), "--pid", str(benchmark.pid),
                                    "--root-dir", str(directory / "runtime")], cwd=ROOT, env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    samples: list[tuple[float, float, float]] = []
    deadline = time.monotonic() + 15
    while (benchmark.poll() is None or (runtime is not None and runtime.poll() is None)) and time.monotonic() < deadline:
        if runtime is not None and runtime.poll() is None:
            sample = proc_sample(runtime.pid)
            if sample is not None:
                samples.append(sample)
        time.sleep(0.005)
    notes = []
    for process, name in ((benchmark, "benchmark"), (runtime, "runtime")):
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
                notes.append(f"forced cleanup: {name}")
    _, benchmark_error = benchmark.communicate()
    runtime_error = ""
    if runtime is not None:
        _, runtime_error = runtime.communicate()
    elapsed, throughput = benchmark_metrics(benchmark_csv)
    profile_data = profile_metrics(profile) if mode == "INTEGRATED" else {}
    runtime_cpu = runtime_rss = None
    if len(samples) > 1:
        runtime_cpu = (samples[-1][1] - samples[0][1]) / (samples[-1][0] - samples[0][0]) * 100.0
        runtime_rss = max(sample[2] for sample in samples)
    status = "MEASURED" if benchmark.returncode == 0 and elapsed is not None and (runtime is None or runtime.returncode == 0) else "FAILED"
    return {"benchmark_elapsed_ms": text(elapsed), "benchmark_throughput_ops_sec": text(throughput),
            "runtime_cpu_percent": text(runtime_cpu), "runtime_rss_kb": text(runtime_rss),
            "discovery_scans": text(profile_data.get("discovery_scans")),
            "discovery_total_us": text(profile_data.get("discovery_total_us")),
            "discovery_mean_us": text(profile_data.get("discovery_mean_us")),
            "runtime_cycle_total_us": text(profile_data.get("runtime_cycle_total_us")), "status": status,
            "notes": "; ".join(value for value in ("; ".join(notes), benchmark_error.strip(), runtime_error.strip()) if value)}


def detection(interval: int, run_index: int) -> dict[str, str]:
    result = subprocess.run([str(ROOT / "bin/discovery-cadence-probe"), str(interval)], cwd=ROOT, capture_output=True, text=True)
    rows = list(csv.DictReader(result.stdout.splitlines())) if result.stdout else []
    row = rows[-1] if rows else {}
    return {"interval_ms": str(interval), "run_index": str(run_index), "latency_ms": row.get("latency_ms", "NA"),
            "discovery_scans": row.get("discovery_scans", "NA"), "schedule_passes": row.get("schedule_passes", "NA"),
            "status": row.get("status", "FAILED") if result.returncode == 0 else "FAILED"}


def main() -> int:
    RESULTS.mkdir(parents=True, exist_ok=True)
    build = subprocess.run(["make", "profile-awavma-runtime", "discovery-cadence-probe"], cwd=ROOT)
    if build.returncode != 0 or not (ROOT / "bin/benchmark").is_file():
        return 1
    raw: list[dict[str, str]] = []
    detection_rows: list[dict[str, str]] = []
    with tempfile.TemporaryDirectory(prefix="awavma-discovery-cadence-") as temporary:
        root = Path(temporary)
        for interval in INTERVALS:
            for run_type, count in (("WARMUP", WARMUPS), ("MEASURED", MEASURED)):
                for index in range(1, count + 1):
                    for mode in ("BASELINE", "INTEGRATED"):
                        values = run_case(interval, mode, root / f"{interval}-{run_type}-{index}-{mode}")
                        raw.append({"timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "interval_ms": str(interval),
                                    "mode": mode, "run_type": run_type, "run_index": str(index), **values})
            for index in range(1, MEASURED + 1):
                detection_rows.append(detection(interval, index))
    write(RESULTS / "discovery_cadence_raw.csv", RAW_FIELDS, raw)
    write(RESULTS / "discovery_cadence_detection_latency.csv", DETECTION_FIELDS, detection_rows)
    summary: list[dict[str, str]] = []
    for interval in INTERVALS:
        for mode in ("BASELINE", "INTEGRATED"):
            for metric in ("benchmark_elapsed_ms", "benchmark_throughput_ops_sec", "runtime_cpu_percent",
                           "runtime_rss_kb", "discovery_scans", "discovery_total_us", "discovery_mean_us",
                           "runtime_cycle_total_us"):
                values = [number(row.get(metric)) for row in raw if row["interval_ms"] == str(interval) and row["mode"] == mode and row["run_type"] == "MEASURED" and row["status"] == "MEASURED"]
                values = [value for value in values if value is not None]
                summary.append({"interval_ms": str(interval), "mode": mode, "metric": metric, "count": str(len(values)),
                                "mean": text(statistics.mean(values) if values else None), "median": text(statistics.median(values) if values else None),
                                "min": text(min(values) if values else None), "max": text(max(values) if values else None),
                                "stddev": text(statistics.stdev(values) if len(values) > 1 else None), "status": "MEASURED" if values else "UNAVAILABLE"})
    write(RESULTS / "discovery_cadence_summary.csv", SUMMARY_FIELDS, summary)
    candidates = []
    for interval in INTERVALS:
        latency = [number(row["latency_ms"]) for row in detection_rows if row["interval_ms"] == str(interval) and row["status"] == "MEASURED"]
        latency = [value for value in latency if value is not None]
        discovery = next((number(row["mean"]) for row in summary if row["interval_ms"] == str(interval) and row["mode"] == "INTEGRATED" and row["metric"] == "discovery_total_us"), None)
        if latency and discovery is not None and statistics.mean(latency) <= 350:
            candidates.append((interval, discovery, statistics.mean(latency)))
    selected = max(candidates, key=lambda value: value[0])[0] if candidates else 0
    comparison = [{"metric": "selected_interval_ms", "before": "legacy every scheduler pass", "after": str(selected), "change": "candidate requires mean detection latency <= 350 ms"}]
    for interval, discovery, latency in candidates:
        comparison.append({"metric": f"candidate_{interval}", "before": text(discovery), "after": text(latency), "change": "discovery_total_us; detection_latency_ms"})
    write(RESULTS / "discovery_cadence_comparison.csv", ("metric", "before", "after", "change"), comparison)
    graph = subprocess.run([sys.executable, str(ROOT / "scripts/generate_discovery_cadence_graphs.py"), "--root", str(ROOT)], cwd=ROOT)
    valid = sum(row["run_type"] == "MEASURED" and row["status"] == "MEASURED" for row in raw)
    print(f"Discovery cadence: valid={valid}/50 selected_interval_ms={selected} graphs_returncode={graph.returncode}")
    return 0 if valid == 50 and selected > 0 and graph.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
