#!/usr/bin/env python3
"""Profile the serialized Phase 4-6 subprocess pipeline without changing policy."""

from __future__ import annotations

import argparse
import csv
import os
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict
from pathlib import Path

from run_multi_application_performance import (DISCOVERY_MS, EVALUATION_MS, MEMORY_MB, MONITOR_MS,
                                                PRIMARY_WORKLOAD, QUEUE_CAPACITY, ROOT, SAMPLE_PERIOD_SECONDS,
                                                THREADS, benchmark_result, group_metrics, mean, median, metrics,
                                                number, process_children, proc_sample, read_csv, terminate, text,
                                                write_csv)


RESULTS = ROOT / "results"
LOGS = ROOT / "logs"
APPLICATION_COUNTS = (1, 2, 4, 8)
WORKERS = 2
ITERATIONS = 17_841_231
WARMUPS = 3
MEASURED = 5
VARIANT = "SUBPROCESS_SERIAL_BASELINE"
PHASE4_MODE = "subprocess"
RAW_FIELDS = (
    "run_id", "variant", "run_type", "run_index", "application_count", "worker_count", "app_id", "pid",
    "start_time_ticks", "generation", "phase", "serial_wait_us", "artifact_prepare_us", "phase3_artifact_scan_us",
    "parent_prepare_us", "fork_wait_wall_us", "child_profiled_total_us", "startup_uninstrumented_residual_us",
    "direct_api_us",
    "file_io_us", "result_parse_us", "total_phase_us", "pipeline_total_us", "pipeline_service_rate_apps_sec",
    "aggregate_throughput", "benchmark_elapsed_ms", "runtime_cpu_percent", "runtime_rss_kb", "runtime_vms_kb",
    "completion_latency_us", "status", "notes",
)
SUMMARY_FIELDS = ("variant", "application_count", "phase", "metric", "count", "mean", "median", "min", "max", "stddev", "status")
EVENT_FIELDS = ("run_id", "source", "application_count", "run_type", "level", "component", "application", "pid", "job_id", "start_ns", "end_ns", "duration_us", "status", "metric_value")
RUN_FIELDS = ("run_id", "variant", "run_type", "run_index", "application_count", "worker_count", "pipeline_total_us", "pipeline_service_rate_apps_sec", "aggregate_throughput", "runtime_cpu_percent", "runtime_rss_kb", "runtime_vms_kb", "status", "notes")


def profile_duration(rows: list[dict[str, str]], component: str, pid: int | None = None, generation: str | None = None) -> float | None:
    values = []
    for row in rows:
        if row.get("component") != component:
            continue
        if pid is not None and row.get("pid") != str(pid):
            continue
        if generation is not None and row.get("job_id") != generation:
            continue
        value = number(row.get("duration_us"))
        if value is not None:
            values.append(value)
    return sum(values) if values else None


def child_profiles(runtime_root: Path, records: dict[str, dict[str, str]]) -> tuple[list[dict[str, str]], dict[tuple[str, str, str], float]]:
    events: list[dict[str, str]] = []
    totals: dict[tuple[str, str, str], float] = {}
    for path in runtime_root.glob("apps/*/cycles/*/*.profile.csv"):
        phase = path.name.removesuffix(".profile.csv")
        generation = path.parent.name
        app_id = path.parents[2].name
        record = records.get(app_id, {})
        for row in read_csv(path):
            row = dict(row)
            row["application"] = app_id
            row["pid"] = record.get("pid", row.get("pid", "-1"))
            events.append({"source": "child", **row})
            child_component = f"{phase}_child_api_total" if phase != "phase6" else "phase6_child_validate_log"
            if row.get("component") == child_component:
                value = number(row.get("duration_us"))
                if value is not None:
                    key = (app_id, generation, phase)
                    totals[key] = totals.get(key, 0.0) + value
    return events, totals


def first_value(rows: list[dict[str, str]], component: str, pid: int, generation: str) -> float | None:
    return profile_duration(rows, component, pid, generation)


def runtime_record_map(rows: list[dict[str, str]]) -> dict[str, dict[str, str]]:
    return {row["app_id"]: row for row in rows if row.get("app_id")}


def phase_rows(run_id: str, run_type: str, run_index: int, app_count: int, parent: list[dict[str, str]],
               child_totals: dict[tuple[str, str, str], float], records: dict[str, dict[str, str]],
               aggregate: float | None, elapsed_by_pid: dict[int, float | None], runtime_resource: dict[str, float | None],
               notes: str) -> tuple[list[dict[str, str]], float | None, float | None]:
    pipeline_total = profile_duration(parent, "pipeline_serial_total")
    completion_events = [row for row in parent if row.get("component") == "pipeline_completion"]
    service_rate = len(completion_events) / (pipeline_total / 1_000_000.0) if pipeline_total and pipeline_total > 0 else None
    rows: list[dict[str, str]] = []
    parent_apps = sorted({row.get("application") for row in parent if row.get("application") not in (None, "-")})
    for app_id in parent_apps:
        record = records.get(app_id, {})
        pid_text = record.get("pid", "-1")
        try:
            pid = int(pid_text)
        except ValueError:
            continue
        generations = sorted({row.get("job_id", "") for row in parent if row.get("application") == app_id and row.get("component") == "phase4_execution"}, key=lambda item: int(item) if item.isdigit() else -1)
        for generation in generations:
            serial_wait = first_value(parent, "pipeline_serial_wait", pid, generation)
            artifact = first_value(parent, "coordinator_artifact_prepare", pid, generation)
            scan = first_value(parent, "phase3_artifact_scan", pid, generation)
            normalize = first_value(parent, "monitoring_normalize_io", pid, generation)
            delta = first_value(parent, "classification_delta_io", pid, generation)
            adapter = first_value(parent, "validation_input_adapt_io", pid, generation)
            result_parse = first_value(parent, "pipeline_result_parse_io", pid, generation)
            completion_latency = first_value(parent, "completion_latency", pid, generation)
            for phase in ("phase4", "phase5", "phase6"):
                total = first_value(parent, f"{phase}_execution", pid, generation)
                prepare = first_value(parent, f"{phase}_parent_prepare", pid, generation)
                fork_wait = first_value(parent, f"{phase}_fork_wait_wall", pid, generation)
                child_total = child_totals.get((app_id, generation, phase))
                direct_api = first_value(parent, "phase4_direct_api_total", pid, generation) if phase == "phase4" else None
                residual = fork_wait - child_total if fork_wait is not None and child_total is not None else None
                file_io = (normalize or 0.0) + (delta or 0.0) if phase == "phase4" and normalize is not None and delta is not None else (adapter if phase == "phase6" else None)
                status = next((row.get("status", "UNKNOWN") for row in parent if row.get("application") == app_id and row.get("component") == f"{phase}_execution" and row.get("job_id") == generation), "UNKNOWN")
                rows.append({"run_id": run_id, "variant": VARIANT, "run_type": run_type,
                             "run_index": str(run_index), "application_count": str(app_count), "worker_count": str(WORKERS),
                             "app_id": app_id, "pid": str(pid), "start_time_ticks": record.get("start_time_ticks", "NA"),
                             "generation": generation, "phase": phase, "serial_wait_us": text(serial_wait),
                             "artifact_prepare_us": text(artifact), "phase3_artifact_scan_us": text(scan),
                             "parent_prepare_us": text(prepare), "fork_wait_wall_us": text(fork_wait),
                             "child_profiled_total_us": text(child_total), "startup_uninstrumented_residual_us": text(residual),
                             "direct_api_us": text(direct_api),
                             "file_io_us": text(file_io), "result_parse_us": text(result_parse if phase == "phase6" else None),
                             "total_phase_us": text(total), "pipeline_total_us": text(pipeline_total),
                             "pipeline_service_rate_apps_sec": text(service_rate), "aggregate_throughput": text(aggregate),
                             "benchmark_elapsed_ms": text(elapsed_by_pid.get(pid)), "runtime_cpu_percent": text(runtime_resource["cpu_percent"]),
                             "runtime_rss_kb": text(runtime_resource["rss_kb"]), "runtime_vms_kb": text(runtime_resource["vms_kb"]),
                             "completion_latency_us": text(completion_latency), "status": status, "notes": notes})
    return rows, pipeline_total, service_rate


def launch(root: Path, app_count: int) -> tuple[list[subprocess.Popen[str]], list[Path], list[int]]:
    processes, outputs, launched = [], [], []
    for index in range(app_count):
        output = root / f"benchmark-{index}.csv"
        command = [str(ROOT / "bin/benchmark"), "--threads", str(THREADS), "--memory", str(MEMORY_MB),
                   "--iterations", str(ITERATIONS), "--pattern", PRIMARY_WORKLOAD, "--seed", str(12345 + index), "--output", str(output)]
        launched.append(time.monotonic_ns())
        processes.append(subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True))
        outputs.append(output)
    return processes, outputs, launched


def run_one(run_id: str, run_type: str, run_index: int, app_count: int, timeout: float) -> tuple[list[dict[str, str]], list[dict[str, str]], dict[str, str]]:
    notes: list[str] = []
    with tempfile.TemporaryDirectory(prefix=f"awavma-phase46-{run_id}-") as temporary:
        root = Path(temporary)
        processes, outputs, launched = launch(root, app_count)
        pids = [process.pid for process in processes]
        identities = {pid: int(sample["start_time_ticks"]) for pid in pids if (sample := proc_sample(pid)) is not None}
        parent_profile = root / "parent.profile.csv"
        command = [str(ROOT / "bin/profile-awavma-runtime"), "--duration-ms", "10000", "--evaluation-ms", str(EVALUATION_MS),
                   "--monitor-ms", str(MONITOR_MS), "--discovery-interval-ms", str(DISCOVERY_MS), "--workers", str(WORKERS),
                   "--queue-capacity", str(QUEUE_CAPACITY), "--root-dir", str(root / "runtime"), "--bin-dir", str(ROOT / "bin/phase46-profile"),
                   "--phase4-mode", PHASE4_MODE]
        for pid in pids:
            command.extend(("--pid", str(pid)))
        environment = os.environ.copy()
        environment["AWAVMA_PROFILE_PATH"] = str(parent_profile)
        runtime_stdout_path = root / "runtime.stdout.log"
        runtime_stderr_path = root / "runtime.stderr.log"
        runtime_stdout = runtime_stdout_path.open("w", encoding="utf-8")
        runtime_stderr = runtime_stderr_path.open("w", encoding="utf-8")
        runtime = subprocess.Popen(command, cwd=ROOT, env=environment, stdout=runtime_stdout, stderr=runtime_stderr, text=True)
        runtime_samples: list[dict[str, float]] = []
        group_samples: list[dict[int, dict[str, float]]] = []
        exited: set[int] = set()
        stop_sent = False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and (len(exited) < len(processes) or runtime.poll() is None):
            group = {pid: sample for pid in pids if pid not in exited if (sample := proc_sample(pid)) is not None}
            if group:
                group_samples.append(group)
            if (sample := proc_sample(runtime.pid)) is not None:
                runtime_samples.append(sample)
            for process in processes:
                if process.poll() is not None:
                    exited.add(process.pid)
            if len(exited) == len(processes) and runtime.poll() is None and not stop_sent:
                runtime.send_signal(signal.SIGTERM)
                stop_sent = True
            time.sleep(SAMPLE_PERIOD_SECONDS)
        for index, process in enumerate(processes):
            terminate(process, f"benchmark-{index}", notes)
        terminate(runtime, "runtime", notes)
        runtime_stdout.close()
        runtime_stderr.close()
        benchmark_errors = []
        for process in processes:
            _, error = process.communicate()
            if error.strip():
                benchmark_errors.append(error.strip())
        runtime.communicate()
        if benchmark_errors:
            notes.extend(benchmark_errors)
        if (runtime_error := runtime_stderr_path.read_text(encoding="utf-8")).strip():
            notes.append(runtime_error.strip())
        parent = read_csv(parent_profile)
        runtime_rows = read_csv(root / "runtime/runtime_results.csv")
        records = runtime_record_map(runtime_rows)
        child_events, child_totals = child_profiles(root / "runtime", records)
        events = [{"source": "parent", **row} for row in parent] + child_events
        elapsed_by_pid, operations = {}, []
        for process, output in zip(processes, outputs):
            elapsed, _, operation_count = benchmark_result(output)
            elapsed_by_pid[process.pid] = elapsed
            if operation_count is not None:
                operations.append(operation_count)
        group_resource = group_metrics(group_samples)
        makespan = max((row["time"] for sample in group_samples for row in sample.values()), default=0.0) - min((row["time"] for sample in group_samples for row in sample.values()), default=0.0)
        aggregate = sum(operations) / makespan if operations and makespan > 0 else None
        runtime_resource = metrics(runtime_samples)
        run_status = "MEASURED"
        if any(process.returncode != 0 for process in processes):
            run_status = "FAILED_BENCHMARK"
        elif runtime.returncode != 0:
            run_status = "FAILED_RUNTIME"
        elif len(records) != app_count:
            run_status = "TARGET_NOT_DISCOVERED"
        rows, pipeline_total, service_rate = phase_rows(run_id, run_type, run_index, app_count, parent, child_totals, records, aggregate, elapsed_by_pid, runtime_resource, "; ".join(notes))
        for row in rows:
            if run_status != "MEASURED":
                row["status"] = run_status
            if row["pid"] in {str(pid) for pid in identities}:
                row["start_time_ticks"] = str(identities[int(row["pid"])] )
        run = {"run_id": run_id, "variant": VARIANT, "run_type": run_type,
               "run_index": str(run_index), "application_count": str(app_count), "worker_count": str(WORKERS),
               "pipeline_total_us": text(pipeline_total), "pipeline_service_rate_apps_sec": text(service_rate),
               "aggregate_throughput": text(aggregate), "runtime_cpu_percent": text(runtime_resource["cpu_percent"]),
               "runtime_rss_kb": text(runtime_resource["rss_kb"]), "runtime_vms_kb": text(runtime_resource["vms_kb"]),
               "status": run_status, "notes": "; ".join(notes)}
        return rows, events, run


def summary(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    metrics_to_summarize = ("serial_wait_us", "parent_prepare_us", "fork_wait_wall_us", "child_profiled_total_us", "startup_uninstrumented_residual_us", "direct_api_us", "file_io_us", "result_parse_us", "total_phase_us", "pipeline_total_us", "pipeline_service_rate_apps_sec", "aggregate_throughput", "runtime_cpu_percent", "completion_latency_us")
    output = []
    groups = {(row["variant"], row["application_count"], row["phase"]) for row in rows}
    for variant, app_count, phase in sorted(groups, key=lambda value: (value[0], int(value[1]), value[2])):
        selected = [row for row in rows if row["variant"] == variant and row["application_count"] == app_count and row["phase"] == phase and row["run_type"] == "MEASURED" and row["status"] == "OK"]
        for metric in metrics_to_summarize:
            values = [number(row.get(metric)) for row in selected]
            values = [value for value in values if value is not None]
            output.append({"variant": variant, "application_count": app_count, "phase": phase,
                           "metric": metric, "count": str(len(values)), "mean": text(mean(values)), "median": text(median(values)),
                           "min": text(min(values) if values else None), "max": text(max(values) if values else None),
                           "stddev": text(statistics.stdev(values) if len(values) > 1 else None),
                           "status": "MEASURED" if values else "UNAVAILABLE"})
    return output


def audit_rows() -> list[dict[str, str]]:
    return [
        {"component": "Coordinator", "current_invocation": "foreground synchronous loop", "state_model": "runtime records", "file_io": "per-app cycle artifacts and runtime_results.csv", "thread_safety": "SERIALIZED", "serialization_reason": "single loop plus blocking waitpid", "optimization_chosen": "profiling only"},
        {"component": "Phase 4", "current_invocation": "classifier subprocess; in-process experiment retained", "state_model": "run-local", "file_io": "distinct input/output CSV", "thread_safety": "PER_APPLICATION_SAFE", "serialization_reason": "current coordinator loop; shared output unsafe", "optimization_chosen": "candidate rejected by reprofile"},
        {"component": "Phase 5", "current_invocation": "decision subprocess", "state_model": "run-local plus per-app persistence", "file_io": "state/history/log/output", "thread_safety": "PER_APPLICATION_SAFE", "serialization_reason": "same persistence tree unsafe", "optimization_chosen": "none before profile gate"},
        {"component": "Phase 6", "current_invocation": "validation subprocess", "state_model": "process-global config/logger", "file_io": "result/history/log", "thread_safety": "SERIALIZED_REQUIRED in-process", "serialization_reason": "Validation_Init/logger globals", "optimization_chosen": "retain subprocess isolation"},
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--warmups", type=int, default=WARMUPS)
    parser.add_argument("--measured", type=int, default=MEASURED)
    parser.add_argument("--timeout-seconds", type=float, default=30.0)
    parser.add_argument("--variant", default="SUBPROCESS_SERIAL_BASELINE")
    parser.add_argument("--phase4-mode", choices=("subprocess", "in-process"), default="subprocess")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--summarize-only", action="store_true")
    args = parser.parse_args()
    global VARIANT, PHASE4_MODE
    VARIANT, PHASE4_MODE = args.variant, args.phase4_mode
    RESULTS.mkdir(parents=True, exist_ok=True)
    LOGS.mkdir(parents=True, exist_ok=True)
    if args.quick:
        args.warmups, args.measured = 0, 1
    if args.summarize_only:
        rows = read_csv(RESULTS / "phase46_pipeline_profile_raw.csv")
        write_csv(RESULTS / "phase46_pipeline_profile_summary.csv", SUMMARY_FIELDS, summary(rows))
        return 0
    build = subprocess.run(["make", "profile-awavma-runtime", "profile-phase46-binaries"], cwd=ROOT)
    if build.returncode != 0:
        return build.returncode
    all_rows = [row for row in read_csv(RESULTS / "phase46_pipeline_profile_raw.csv") if row.get("variant") != VARIANT]
    all_events = [row for row in read_csv(RESULTS / "phase46_pipeline_profile_events.csv") if row.get("run_id", "").find(f"-{VARIANT}-") < 0]
    runs = [row for row in read_csv(RESULTS / "phase46_pipeline_profile_runs.csv") if row.get("variant") != VARIANT]
    for app_count in APPLICATION_COUNTS:
        for index in range(1, args.warmups + args.measured + 1):
            run_type = "WARMUP" if index <= args.warmups else "MEASURED"
            run_id = f"phase46-{VARIANT}-a{app_count}-{run_type.lower()}-{index}"
            print(f"Phase46 run {run_id}", flush=True)
            rows, events, run = run_one(run_id, run_type, index, app_count, args.timeout_seconds)
            all_rows.extend(rows)
            for event in events:
                all_events.append({"run_id": run_id, "application_count": str(app_count), "run_type": run_type, **event})
            runs.append(run)
    write_csv(RESULTS / "phase46_pipeline_profile_raw.csv", RAW_FIELDS, all_rows)
    write_csv(RESULTS / "phase46_pipeline_profile_events.csv", EVENT_FIELDS, all_events)
    write_csv(RESULTS / "phase46_pipeline_profile_runs.csv", RUN_FIELDS, runs)
    write_csv(RESULTS / "phase46_pipeline_profile_summary.csv", SUMMARY_FIELDS, summary(all_rows))
    write_csv(RESULTS / "phase46_pipeline_profile_concurrency_audit.csv", ("component", "current_invocation", "state_model", "file_io", "thread_safety", "serialization_reason", "optimization_chosen"), audit_rows())
    counts = Counter(row.get("status", "UNKNOWN") for row in all_rows if row.get("run_type") == "MEASURED")
    write_csv(RESULTS / "phase46_pipeline_status_counts.csv", ("status", "count"), [{"status": status, "count": str(count)} for status, count in sorted(counts.items())])
    print(f"Phase46 pipeline profile: raw_rows={len(all_rows)} runs={len(runs)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
