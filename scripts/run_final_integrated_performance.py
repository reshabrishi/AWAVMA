#!/usr/bin/env python3
"""Re-profile only the accepted serialized subprocess AWAVMA production path."""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
LOGS = ROOT / "logs"
sys.path.insert(0, str(ROOT / "tests"))
import full_system_performance_test as single  # noqa: E402
import run_multi_application_performance as multi  # noqa: E402


WORKLOADS = single.WORKLOADS
SINGLE_FIELDS = single.RAW_FIELDS + ("runtime_cpu_peak_percent", "discovery_scan_count", "discovery_total_us")
SINGLE_SUMMARY_FIELDS = ("scope", "workload", "mode", "metric", "count", "mean", "median", "min", "max", "stddev", "status")
OVERHEAD_FIELDS = ("workload", "metric", "baseline_mean", "integrated_mean", "absolute_difference", "percent_change", "status")


class FinalSingle(single.Evaluation):
    def __init__(self) -> None:
        RESULTS.mkdir(parents=True, exist_ok=True)
        LOGS.mkdir(parents=True, exist_ok=True)
        self.log = (LOGS / "final_integrated_performance.log").open("w", encoding="utf-8")
        self.profile_rows: list[dict[str, str]] = []


class FinalMulti(multi.Evaluation):
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.raw: list[dict[str, str]] = []
        self.runs: list[dict[str, str]] = []
        self.events: list[dict[str, str]] = []
        self.calibration: list[dict[str, str]] = []
        RESULTS.mkdir(parents=True, exist_ok=True)
        LOGS.mkdir(parents=True, exist_ok=True)
        self.log = (LOGS / "final_integrated_multiapp.log").open("w", encoding="utf-8")


def write(path: Path, fields: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def text(value: float | int | None) -> str:
    return single.text(value)


def values(rows: list[dict[str, str]], metric: str, **filters: str) -> list[float]:
    output = []
    for row in rows:
        if not all(row.get(key) == value for key, value in filters.items()):
            continue
        if (value := single.number(row.get(metric))) is not None:
            output.append(value)
    return output


def stats(values_for_metric: list[float]) -> dict[str, str]:
    return {"count": str(len(values_for_metric)), "mean": text(statistics.mean(values_for_metric) if values_for_metric else None),
            "median": text(statistics.median(values_for_metric) if values_for_metric else None),
            "min": text(min(values_for_metric) if values_for_metric else None),
            "max": text(max(values_for_metric) if values_for_metric else None),
            "stddev": text(statistics.stdev(values_for_metric) if len(values_for_metric) > 1 else None),
            "status": "MEASURED" if values_for_metric else "UNAVAILABLE"}


def single_summary(rows: list[dict[str, str]]) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    metrics = ("benchmark_elapsed_ms", "benchmark_throughput_ops_sec", "runtime_cpu_percent", "runtime_cpu_peak_percent",
               "runtime_rss_kb", "runtime_vms_kb", "discovery_scan_count", "discovery_total_us", "discovery_us",
               "queue_wait_us", "worker_execution_us", "phase3_us", "pipeline_serial_wait_us", "phase4_us",
               "phase5_us", "phase6_us", "coordinator_overhead_us", "runtime_cycle_total_us", "completion_latency_us")
    summary, overhead = [], []
    for workload in WORKLOADS:
        for mode in ("BASELINE", "INTEGRATED"):
            for metric in metrics:
                entry = {"scope": "SINGLE_APPLICATION", "workload": workload, "mode": mode, "metric": metric}
                entry.update(stats(values(rows, metric, workload=workload, mode=mode, run_type="MEASURED", status="MEASURED")))
                summary.append(entry)
        for metric in ("benchmark_elapsed_ms", "benchmark_throughput_ops_sec"):
            before = values(rows, metric, workload=workload, mode="BASELINE", run_type="MEASURED", status="MEASURED")
            after = values(rows, metric, workload=workload, mode="INTEGRATED", run_type="MEASURED", status="MEASURED")
            baseline, integrated = (statistics.mean(before) if before else None), (statistics.mean(after) if after else None)
            overhead.append({"workload": workload, "metric": metric, "baseline_mean": text(baseline), "integrated_mean": text(integrated),
                             "absolute_difference": text(integrated - baseline if baseline is not None and integrated is not None else None),
                             "percent_change": text((integrated - baseline) / baseline * 100.0 if baseline else None),
                             "status": "MEASURED" if baseline is not None and integrated is not None else "UNAVAILABLE"})
    return summary, overhead


def run_single() -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    single.EVALUATION_MS, single.DISCOVERY_INTERVAL_MS = 1000, 250
    evaluation, rows = FinalSingle(), []
    try:
        if not evaluation.build():
            raise RuntimeError("final single-app prerequisites unavailable")
        evaluation.note("Production configuration: subprocess Phase4/5/6; FIFO; workers=2; monitor=100ms; evaluation=1000ms; discovery=250ms")
        with tempfile.TemporaryDirectory(prefix="awavma-final-single-") as temporary:
            root = Path(temporary)
            for workload in WORKLOADS:
                for run_type, repetitions in (("WARMUP", 5), ("MEASURED", 5)):
                    for index in range(1, repetitions + 1):
                        order = ("BASELINE", "INTEGRATED") if index % 2 else ("INTEGRATED", "BASELINE")
                        for mode in order:
                            run_id = f"final-single-{workload}-{run_type.lower()}-{index}-{mode.lower()}"
                            run_root = root / run_id
                            run_root.mkdir()
                            if mode == "BASELINE":
                                metrics, status, notes = evaluation.baseline(workload, run_root)
                                app_id, outcomes = "NA", "NOT_RUN"
                            else:
                                metrics, status, notes, app_id, outcomes = evaluation.integrated(workload, run_root, run_id, run_type)
                            profile = [event for event in evaluation.profile_rows if event.get("run_id") == run_id]
                            scans = [single.number(event.get("duration_us")) for event in profile if event.get("component") == "application_discovery_scan"]
                            scan_values = [value for value in scans if value is not None]
                            if run_type == "WARMUP" and status == "MEASURED":
                                status = "WARMUP"
                            row = {field: "NA" for field in SINGLE_FIELDS}
                            row.update({"timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "run_id": run_id,
                                        "workload": workload, "mode": mode, "run_type": run_type, "run_index": str(index),
                                        "threads": "2", "memory_mb": "8", "iterations": "1000000", "seed": "12345",
                                        "app_id": app_id, "pipeline_statuses": outcomes, "status": status, "notes": notes or "",
                                        "discovery_scan_count": text(len(scan_values)), "discovery_total_us": text(sum(scan_values) if scan_values else None)})
                            for key, value in metrics.items():
                                if key in row:
                                    row[key] = text(value if isinstance(value, (int, float)) else None)
                            rows.append(row)
                            evaluation.note(f"RUN {run_id} status={status} pipeline={outcomes}")
        return rows, evaluation.profile_rows
    finally:
        evaluation.close()


def run_multi() -> tuple[FinalMulti, list[dict[str, str]], list[dict[str, str]], list[dict[str, str]], list[dict[str, str]]]:
    args = argparse.Namespace(iterations=17_841_231, warmups=3, measured=5, runtime_duration_ms=5000,
                              timeout_seconds=30.0, skip_heterogeneous=True)
    multi.APP_COUNTS, multi.WORKER_COUNTS = (1, 2, 4, 8), (2,)
    evaluation = FinalMulti(args)
    try:
        if not evaluation.build():
            raise RuntimeError("final multi-app prerequisites unavailable")
        iterations = evaluation.calibrate()
        evaluation.run_primary(iterations)
        summary, scaling, fairness, statuses = multi.summarize(evaluation.raw, evaluation.runs)
        return evaluation, summary, scaling, fairness, statuses
    except Exception:
        evaluation.close()
        raise


def combined_status(single_rows: list[dict[str, str]], multi_statuses: list[dict[str, str]]) -> list[dict[str, str]]:
    counts = Counter()
    for row in single_rows:
        if row.get("mode") != "INTEGRATED" or row.get("run_type") != "MEASURED":
            continue
        for item in row.get("pipeline_statuses", "").split(";"):
            if ":" in item:
                name, count = item.split(":", 1)
                counts[multi.status_outcome(name)] += int(count)
    for row in multi_statuses:
        counts[row["outcome"]] += int(row["count"])
    return [{"outcome": name, "count": str(counts.get(name, 0)),
             "status": "MEASURED" if counts.get(name, 0) else "NOT EXECUTED"}
            for name in ("INSUFFICIENT", "NO_MIGRATION", "VALIDATION_REJECTED", "VALIDATION_APPROVED", "MIGRATION_EXECUTED", "FEEDBACK_PENDING", "FEEDBACK_APPLIED")]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    if args.quick:
        raise SystemExit("quick mode is intentionally unsupported; final artifacts require the complete matrix")
    single_rows, single_events = run_single()
    multi_evaluation, multi_summary, scaling, fairness, multi_statuses = run_multi()
    summary, overhead = single_summary(single_rows)
    write(RESULTS / "final_integrated_performance_raw.csv", SINGLE_FIELDS, single_rows)
    write(RESULTS / "final_integrated_performance_profile_events.csv", single.PROFILE_FIELDS, single_events)
    write(RESULTS / "final_integrated_performance_summary.csv", SINGLE_SUMMARY_FIELDS, summary)
    write(RESULTS / "final_integrated_performance_overhead.csv", OVERHEAD_FIELDS, overhead)
    write(RESULTS / "final_integrated_multiapp_raw.csv", multi.RAW_FIELDS, multi_evaluation.raw)
    write(RESULTS / "final_integrated_multiapp_runs.csv", multi.RUN_FIELDS, multi_evaluation.runs)
    write(RESULTS / "final_integrated_multiapp_profile_events.csv", multi.PROFILE_FIELDS, multi_evaluation.events)
    write(RESULTS / "final_integrated_multiapp_summary.csv", multi.SUMMARY_FIELDS, multi_summary)
    write(RESULTS / "final_integrated_multiapp_scaling.csv", multi.SCALING_FIELDS, scaling)
    write(RESULTS / "final_integrated_multiapp_fairness.csv", multi.FAIRNESS_FIELDS, fairness)
    write(RESULTS / "final_integrated_status_counts.csv", ("outcome", "count", "status"), combined_status(single_rows, multi_statuses))
    write(RESULTS / "final_integrated_environment.csv", ("key", "value", "status"), single.environment_rows() + multi.environment(17_841_231))
    write(RESULTS / "final_integrated_multiapp_calibration.csv", ("attempt", "iterations", "elapsed_ms", "status"), multi_evaluation.calibration)
    multi_evaluation.close()
    print(f"Final integrated reprofile: single_rows={len(single_rows)} multi_rows={len(multi_evaluation.raw)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
