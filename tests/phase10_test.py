#!/usr/bin/env python3
"""Phase 10 performance measurements using the frozen Phase 2-9 binaries."""

from __future__ import annotations

import csv
import ctypes.util
import math
import os
import resource
import shutil
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
REPETITIONS = 5
THREADS = 2
MEMORY_MB = 8
ITERATIONS = 1_000_000
SEED = 12345
RAW_FIELDS = (
    "run_id", "input_type", "workload", "threads", "memory", "iterations", "run_number", "warmup",
    "execution_time", "throughput", "cpu_utilization", "rss", "page_faults", "thread_count",
    "worker_count", "monitoring_overhead", "classifier_overhead", "decision_overhead",
    "validation_overhead", "migration_overhead", "feedback_overhead", "total_overhead",
    "runtime_orchestration_overhead", "phase_pipeline_status", "status",
)
SUMMARY_FIELDS = (
    "workload", "input_type", "metric", "baseline_mean", "awavma_mean", "absolute_difference",
    "percentage_difference", "median", "minimum", "maximum", "stddev", "sample_count", "status",
)


def finite(value: object) -> float | None:
    if value is None or str(value).strip().upper() in ("", "NA", "N/A", "UNAVAILABLE", "UNKNOWN"):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


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


class Phase10:
    def __init__(self) -> None:
        RESULTS.mkdir(parents=True, exist_ok=True)
        LOGS.mkdir(parents=True, exist_ok=True)
        self.rows: list[dict[str, str]] = []
        self.failures = 0
        self.log = (LOGS / "phase10.log").open("w", encoding="utf-8")

    def close(self) -> None:
        self.log.close()

    def note(self, text: str) -> None:
        self.log.write(text.rstrip() + "\n")
        self.log.flush()

    def command(self, name: str, command: list[str], timeout: int = 120) -> tuple[subprocess.CompletedProcess[str], float]:
        self.note(f"COMMAND {name}: {' '.join(command)}")
        started = time.perf_counter()
        try:
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            result = subprocess.CompletedProcess(command, 124, error.stdout or "", error.stderr or "timeout")
        elapsed = time.perf_counter() - started
        self.note(f"RESULT {name}: returncode={result.returncode} elapsed={elapsed:.9f}")
        if result.stdout:
            self.note(result.stdout[-2000:])
        if result.stderr:
            self.note(result.stderr[-2000:])
        return result, elapsed

    def test(self, test_id: str, expected: str, actual: str, status: str, reason: str = "") -> None:
        self.note(f"TEST {test_id} expected={expected} actual={actual} status={status} reason={reason}")
        if status == "FAILED":
            self.failures += 1


def proc_sample(pid: int) -> dict[str, float] | None:
    try:
        stat_text = (Path(f"/proc/{pid}/stat")).read_text(encoding="utf-8")
        close = stat_text.rfind(")")
        fields = stat_text[close + 2:].split()
        if len(fields) < 22:
            return None
        page_size = os.sysconf("SC_PAGE_SIZE")
        ticks = os.sysconf("SC_CLK_TCK")
        return {
            "cpu": (int(fields[11]) + int(fields[12])) / ticks,
            "rss": int(fields[21]) * page_size / 1024.0,
            "faults": float(int(fields[7]) + int(fields[9])),
            "threads": float(int(fields[17])),
        }
    except (FileNotFoundError, PermissionError, ValueError, IndexError):
        return None


def launch_baseline(phase: Phase10, command: list[str]) -> tuple[subprocess.CompletedProcess[str], float, list[dict[str, float]], dict[str, float]]:
    started = time.perf_counter()
    usage_before = resource.getrusage(resource.RUSAGE_CHILDREN)
    process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    samples: list[dict[str, float]] = []
    while process.poll() is None:
        sample = proc_sample(process.pid)
        if sample is not None:
            sample["time"] = time.perf_counter() - started
            samples.append(sample)
        time.sleep(0.005)
    stdout, stderr = process.communicate()
    elapsed = time.perf_counter() - started
    usage_after = resource.getrusage(resource.RUSAGE_CHILDREN)
    result = subprocess.CompletedProcess(command, process.returncode, stdout, stderr)
    phase.note(f"RESULT baseline-process: returncode={result.returncode} elapsed={elapsed:.9f}")
    if stdout:
        phase.note(stdout[-1200:])
    if stderr:
        phase.note(stderr[-1200:])
    usage = {
        "cpu": (usage_after.ru_utime + usage_after.ru_stime) - (usage_before.ru_utime + usage_before.ru_stime),
        "rss": usage_after.ru_maxrss,
        "faults": (usage_after.ru_minflt + usage_after.ru_majflt) - (usage_before.ru_minflt + usage_before.ru_majflt),
    }
    return result, elapsed, samples, usage


def monitor_metrics(path: Path) -> dict[str, float]:
    rows = read_csv(path)
    cpu = [finite(row.get("process_cpu_utilization_percent")) for row in rows]
    cpu = [value for value in cpu if value is not None]
    rss = [finite(row.get("rss_kb")) for row in rows]
    rss = [value for value in rss if value is not None]
    faults = [finite(row.get("minor_page_faults")) for row in rows]
    faults_major = [finite(row.get("major_page_faults")) for row in rows]
    faults = [value for value in faults if value is not None]
    faults_major = [value for value in faults_major if value is not None]
    threads = [finite(row.get("thread_count")) for row in rows]
    threads = [value for value in threads if value is not None]
    return {
        "cpu": statistics.mean(cpu) if cpu else math.nan,
        "rss": max(rss) if rss else math.nan,
        "faults": (max(faults) if faults else 0.0) + (max(faults_major) if faults_major else 0.0),
        "threads": max(threads) if threads else math.nan,
        "samples": float(len(rows)),
    }


def benchmark_row(path: Path) -> dict[str, str] | None:
    rows = read_csv(path)
    return rows[-1] if rows else None


def phase_input_from_migration(migration: Path, output: Path) -> None:
    fields = ("timestamp", "feedback_id", "migration_id", "app_id", "pid", "entity_id", "action", "phase6_validation", "migration_result", "before_throughput", "after_throughput", "before_execution_time", "after_execution_time", "before_latency", "after_latency", "before_page_faults", "after_page_faults", "before_samples", "after_samples")
    with output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for index, row in enumerate(read_csv(migration)):
            writer.writerow({
                "timestamp": row.get("timestamp", "UNKNOWN"), "feedback_id": f"phase10-{index}",
                "migration_id": row.get("migration_id", f"phase10-{index}"), "app_id": row.get("app_id", "UNKNOWN"),
                "pid": row.get("pid", "0"), "entity_id": row.get("entity_id", "workload"),
                "action": row.get("action", "NO_MIGRATION"), "phase6_validation": row.get("phase6_validation", "REJECTED"),
                "migration_result": row.get("result", "MIGRATION_SYSTEM_ERROR"),
                "before_throughput": "NA", "after_throughput": "NA", "before_execution_time": "NA", "after_execution_time": "NA",
                "before_latency": "NA", "after_latency": "NA", "before_page_faults": "NA", "after_page_faults": "NA",
                "before_samples": "NA", "after_samples": "NA",
            })


def run_phase_pipeline(phase: Phase10, workload: str, monitor_path: Path, root: Path) -> dict[str, float | str]:
    results = root / "results"
    history = root / "history"
    state = root / "state"
    logs = root / "logs"
    for path in (results, history, state, logs):
        path.mkdir(parents=True, exist_ok=True)
    classification = results / "classification.csv"
    decision = results / "decision.csv"
    validation = results / "validation.csv"
    migration = results / "migration.csv"
    feedback_input = root / "feedback_input.csv"
    feedback = results / "feedback.csv"
    commands = (
        ("classifier", [str(ROOT / "bin/classifier"), "--input", str(monitor_path), "--output", str(classification)]),
        ("decision", [str(ROOT / "bin/decision"), "--input", str(classification), "--output", str(decision), "--app-id", f"P10_{workload.upper()}", "--state-dir", str(state), "--history-dir", str(history), "--log", str(logs / "decision.log")]),
        ("validation", [str(ROOT / "bin/validation"), "--input", str(decision), "--output", str(validation), "--history", str(history / "validation.csv"), "--log", str(logs / "validation.log"), "--config", str(ROOT / "config/awavma.conf")]),
        ("migration", [str(ROOT / "bin/migration"), "--input", str(validation), "--output", str(migration), "--history", str(history / "migration.csv"), "--log", str(logs / "migration.log"), "--state", str(state / "migration.csv"), "--config", str(ROOT / "config/awavma.conf")]),
    )
    timings: dict[str, float | str] = {}
    success = True
    for name, command in commands:
        result, elapsed = phase.command(f"{workload}-{name}", command)
        timings[f"{name}_overhead"] = elapsed
        success = success and result.returncode == 0
    phase_input_from_migration(migration, feedback_input)
    result, elapsed = phase.command(f"{workload}-feedback", [str(ROOT / "bin/feedback"), "--input", str(feedback_input), "--output", str(feedback), "--history", str(history / "feedback.csv"), "--log", str(logs / "feedback.log"), "--state-dir", str(state), "--history-dir", str(history)])
    timings["feedback_overhead"] = elapsed
    success = success and result.returncode == 0
    timings["phase_pipeline_status"] = "PASS" if success else "FAILED"
    return timings


def make_row(run_id: str, workload: str, run_number: int, warmup: bool, mode: str,
             wall: float | None, operations: float | None, metrics: dict[str, float],
             overhead: dict[str, float | str], internal_time: float | None) -> dict[str, str]:
    throughput = operations / wall if operations is not None and wall and wall > 0 else math.nan
    monitor_overhead = (wall - internal_time) if mode == "AWAVMA" and wall is not None and internal_time is not None else math.nan
    phase_values = {name: finite(overhead.get(name)) for name in ("classifier_overhead", "decision_overhead", "validation_overhead", "migration_overhead", "feedback_overhead")}
    total_values = [value for value in (monitor_overhead, *phase_values.values()) if value is not None]
    total = sum(total_values) if mode == "AWAVMA" and len(total_values) == 6 else math.nan
    def text(value: object) -> str:
        return "NA" if value is None or (isinstance(value, float) and not math.isfinite(value)) else f"{value:.9f}" if isinstance(value, float) else str(value)
    status = "WARMUP" if warmup else "MEASURED"
    return {
        "run_id": run_id, "input_type": "REAL", "workload": workload, "threads": str(THREADS), "memory": str(MEMORY_MB),
        "iterations": str(ITERATIONS), "run_number": str(run_number), "warmup": "true" if warmup else "false",
        "execution_time": text(wall), "throughput": text(throughput), "cpu_utilization": text(metrics.get("cpu")),
        "rss": text(metrics.get("rss")), "page_faults": text(metrics.get("faults")), "thread_count": text(metrics.get("threads")),
        "worker_count": "UNAVAILABLE", "monitoring_overhead": text(monitor_overhead),
        "classifier_overhead": text(phase_values["classifier_overhead"]), "decision_overhead": text(phase_values["decision_overhead"]),
        "validation_overhead": text(phase_values["validation_overhead"]), "migration_overhead": text(phase_values["migration_overhead"]),
        "feedback_overhead": text(phase_values["feedback_overhead"]), "total_overhead": text(total),
        "runtime_orchestration_overhead": "UNAVAILABLE", "phase_pipeline_status": str(overhead.get("phase_pipeline_status", "NOT_RUN")), "status": status,
    }


def run_case(phase: Phase10, mode: str, workload: str, run_number: int, warmup: bool, root: Path) -> dict[str, str]:
    output = root / f"{mode.lower()}_{workload}_{run_number}_benchmark.csv"
    command_base = ["--threads", str(THREADS), "--memory", str(MEMORY_MB), "--iterations", str(ITERATIONS), "--pattern", workload, "--seed", str(SEED), "--output", str(output)]
    if mode == "BASELINE":
        command = [str(ROOT / "bin/benchmark"), *command_base]
        result, wall, samples, usage = launch_baseline(phase, command)
        metrics = {
            "cpu": usage["cpu"] / wall * 100.0 if wall > 0 else math.nan,
            "rss": max([sample["rss"] for sample in samples], default=usage["rss"]),
            "faults": usage["faults"] if usage["faults"] >= 0 else max([sample["faults"] for sample in samples], default=math.nan),
            "threads": max([sample["threads"] for sample in samples], default=math.nan),
        }
        overhead: dict[str, float | str] = {name: "NA" for name in ("classifier_overhead", "decision_overhead", "validation_overhead", "migration_overhead", "feedback_overhead")}
        overhead["phase_pipeline_status"] = "NOT_RUN"
    else:
        monitor = root / f"{mode.lower()}_{workload}_{run_number}_monitor.csv"
        threads = root / f"{mode.lower()}_{workload}_{run_number}_threads.csv"
        log = root / f"{mode.lower()}_{workload}_{run_number}.log"
        command = [str(ROOT / "bin/monitor"), "--interval", "50", "--output", str(monitor), "--thread-output", str(threads), "--log", str(log), "--benchmark-path", str(ROOT / "bin/benchmark"), "--benchmark", *command_base]
        result, wall = phase.command(f"{mode.lower()}-{workload}-{run_number}", command, timeout=180)
        metrics = monitor_metrics(monitor)
        overhead = {name: "NA" for name in ("classifier_overhead", "decision_overhead", "validation_overhead", "migration_overhead", "feedback_overhead")}
        overhead["phase_pipeline_status"] = "NOT_RUN"
        benchmark = benchmark_row(output)
        if not warmup and result.returncode == 0 and monitor.is_file():
            with tempfile.TemporaryDirectory(prefix=f"awavma-phase10-{workload}-") as directory:
                overhead.update(run_phase_pipeline(phase, workload, monitor, Path(directory)))
    benchmark = benchmark_row(output)
    operations = finite(benchmark.get("operations")) if benchmark else None
    internal_time = finite(benchmark.get("execution_time_sec")) if benchmark else None
    if result.returncode != 0 or benchmark is None:
        phase.test(f"P10-{mode}-{workload}-{run_number}", "benchmark completes", f"returncode={result.returncode}", "FAILED", "measurement command failed")
    return make_row(f"{mode.lower()}-{workload}-{run_number}", workload, run_number, warmup, mode, wall if result.returncode == 0 else None, operations, metrics, overhead, internal_time)


def statistics_summary(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    metrics = ("execution_time", "throughput", "cpu_utilization", "rss", "page_faults", "monitoring_overhead", "classifier_overhead", "decision_overhead", "validation_overhead", "migration_overhead", "feedback_overhead", "total_overhead")
    summary: list[dict[str, str]] = []
    for workload in PATTERNS:
        for metric in metrics:
            baseline = [finite(row.get(metric)) for row in rows if row["workload"] == workload and row["status"] == "MEASURED" and row["input_type"] == "REAL" and row["run_id"].startswith("baseline-")]
            awavma = [finite(row.get(metric)) for row in rows if row["workload"] == workload and row["status"] == "MEASURED" and row["input_type"] == "REAL" and row["run_id"].startswith("awavma-")]
            baseline = [value for value in baseline if value is not None]
            awavma = [value for value in awavma if value is not None]
            comparable = baseline and awavma and metric in ("execution_time", "throughput", "cpu_utilization", "rss", "page_faults")
            values = awavma if awavma else []
            bmean = statistics.mean(baseline) if comparable else math.nan
            amean = statistics.mean(awavma) if awavma else math.nan
            difference = amean - bmean if comparable else math.nan
            percentage = difference / bmean * 100.0 if comparable and bmean != 0 else math.nan
            def text(value: float) -> str:
                return "NA" if not math.isfinite(value) else f"{value:.9f}"
            summary.append({"workload": workload, "input_type": "REAL", "metric": metric, "baseline_mean": text(bmean), "awavma_mean": text(amean), "absolute_difference": text(difference), "percentage_difference": text(percentage), "median": text(statistics.median(values) if values else math.nan), "minimum": text(min(values) if values else math.nan), "maximum": text(max(values) if values else math.nan), "stddev": text(statistics.stdev(values) if len(values) > 1 else math.nan), "sample_count": str(len(values)), "status": "MEASURED" if values and (comparable or metric not in ("execution_time", "throughput", "cpu_utilization", "rss", "page_faults")) else "UNAVAILABLE"})
    return summary


def environment_rows() -> list[dict[str, str]]:
    nodes = sorted(path.name for path in Path("/sys/devices/system/node").glob("node[0-9]*"))
    gcc = subprocess.run(["gcc", "--version"], capture_output=True, text=True) if shutil.which("gcc") else None
    return [{"key": "cpu_count", "value": str(os.cpu_count() or 0), "status": "MEASURED"},
            {"key": "numa_nodes", "value": ";".join(nodes), "status": "MEASURED"},
            {"key": "numa_availability", "value": "single-node-only" if len(nodes) < 2 else "multi-node", "status": "NOT TESTED — ENVIRONMENT LIMITATION" if len(nodes) < 2 else "MEASURED"},
            {"key": "compiler", "value": gcc.stdout.splitlines()[0] if gcc and gcc.stdout else "UNAVAILABLE", "status": "MEASURED" if gcc else "UNAVAILABLE"},
            {"key": "python", "value": sys.version.split()[0], "status": "MEASURED"},
            {"key": "libnuma", "value": "available" if ctypes.util.find_library("numa") else "unavailable", "status": "MEASURED"},
            {"key": "missing_headers", "value": ";".join(name for name, path in (("numa.h", "/usr/include/numa.h"), ("numaif.h", "/usr/include/numaif.h")) if not Path(path).is_file()) or "none", "status": "MEASURED"},
            {"key": "graph_backend", "value": "dependency-free SVG via graph_utils", "status": "MEASURED"}]


def main() -> int:
    phase = Phase10()
    rows: list[dict[str, str]] = []
    try:
        write_csv(RESULTS / "phase10_environment.csv", ("key", "value", "status"), environment_rows())
        phase.note("WARMUP_POLICY: one baseline and one AWAVMA warm-up per workload; warm-ups excluded from statistics")
        phase.note(f"PARAMETERS: threads={THREADS} memory_mb={MEMORY_MB} iterations_per_thread={ITERATIONS} repetitions={REPETITIONS} seed={SEED}")
        phase.note("BASELINE: direct bin/benchmark only; no AWAVMA runtime components launched")
        phase.note("AWAVMA: frozen bin/monitor benchmark launcher plus Phase 4-8 post-processing; full in-process runtime CLI is unavailable and is not claimed")
        with tempfile.TemporaryDirectory(prefix="awavma-phase10-raw-") as directory:
            root = Path(directory)
            for workload in PATTERNS:
                rows.append(run_case(phase, "BASELINE", workload, 0, True, root))
                rows.append(run_case(phase, "AWAVMA", workload, 0, True, root))
                for run_number in range(1, REPETITIONS + 1):
                    rows.append(run_case(phase, "BASELINE", workload, run_number, False, root))
                    rows.append(run_case(phase, "AWAVMA", workload, run_number, False, root))
        write_csv(RESULTS / "phase10_raw_results.csv", RAW_FIELDS, rows)
        summary = statistics_summary(rows)
        write_csv(RESULTS / "phase10_summary.csv", SUMMARY_FIELDS, summary)
        finite_rows = [row for row in rows if row["status"] == "MEASURED" and finite(row.get("execution_time")) is not None and finite(row.get("throughput")) is not None]
        for index, workload in enumerate(PATTERNS):
            baseline_count = sum(row["status"] == "MEASURED" and row["run_id"].startswith(f"baseline-{workload}-") for row in rows)
            awavma_count = sum(row["status"] == "MEASURED" and row["run_id"].startswith(f"awavma-{workload}-") for row in rows)
            phase.test(f"P10-{index * 2 + 1:02d}", f"five baseline {workload} repetitions", str(baseline_count), "PASS" if baseline_count == REPETITIONS else "FAILED")
            phase.test(f"P10-{index * 2 + 2:02d}", f"five AWAVMA {workload} repetitions", str(awavma_count), "PASS" if awavma_count == REPETITIONS else "FAILED")
        phase.test("P10-23", "five finite repetitions per mode/workload", f"{len(finite_rows)} measured rows", "PASS" if len(finite_rows) == len(PATTERNS) * REPETITIONS * 2 else "FAILED")
        parameters_match = all(row["threads"] == str(THREADS) and row["memory"] == str(MEMORY_MB) and row["iterations"] == str(ITERATIONS) for row in rows)
        phase.test("P10-24", "baseline/AWAVMA parameters match", str(parameters_match), "PASS" if parameters_match else "FAILED")
        phase.test("P10-19", "monitoring overhead measured separately", "AWAVMA monitoring delta", "PASS" if any(finite(row.get("monitoring_overhead")) is not None for row in rows if row["status"] == "MEASURED") else "UNAVAILABLE")
        phase.test("P10-20", "decision/validation overhead measured separately", "phase timing columns", "PASS" if any(finite(row.get("decision_overhead")) is not None for row in rows if row["status"] == "MEASURED") else "UNAVAILABLE")
        phase.test("P10-21", "feedback overhead measured separately", "feedback timing column", "PASS" if any(finite(row.get("feedback_overhead")) is not None for row in rows if row["status"] == "MEASURED") else "UNAVAILABLE")
        phase.test("P10-22", "measured overhead total excludes unavailable orchestration", "serialized measured components", "PASS")
        phase.test("P10-17", "multi-application baseline", "full runtime comparison CLI unavailable", "UNAVAILABLE", "no production command exposes the complete multi-application runtime for timing")
        phase.test("P10-18", "multi-application AWAVMA", "full runtime comparison CLI unavailable", "UNAVAILABLE", "no production command exposes the complete multi-application runtime for timing")
        phase.test("P10-25", "remote NUMA capability", "single NUMA node", "NOT TESTED — ENVIRONMENT LIMITATION")
        graph_command = [sys.executable, str(ROOT / "scripts/generate_phase10_graphs.py"), "--root", str(ROOT), "--output-dir", str(ROOT / "graphs/phase10"), "--summary", str(RESULTS / "phase10_graph_summary.csv"), "--log", str(LOGS / "phase10_graphs.log")]
        graph_result, _ = phase.command("phase10-graphs", graph_command)
        if graph_result.returncode != 0:
            return 1
        return 1 if phase.failures else 0
    finally:
        write_csv(RESULTS / "phase10_raw_results.csv", RAW_FIELDS, rows)
        phase.close()


if __name__ == "__main__":
    raise SystemExit(main())
