#!/usr/bin/env python3
"""Generate dependency-free SVGs for multi-application AWAVMA measurements."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from graph_utils import bar_chart, finite


def read(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def value(rows: list[dict[str, str]], apps: int, workers: int, mode: str, metric: str) -> float | None:
    for row in rows:
        if (row.get("application_count") == str(apps) and row.get("worker_count") == str(workers) and
                row.get("mode") == mode and row.get("metric") == metric and row.get("status") == "MEASURED"):
            return finite(row.get("mean"))
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    results = root / "results"
    output = root / "graphs/multi-application-performance" if args.output_dir is None else args.output_dir
    if not output.is_absolute():
        output = root / output
    output.mkdir(parents=True, exist_ok=True)
    scaling = read(results / "multi_application_scaling.csv")
    summary = read(results / "multi_application_performance_summary.csv")
    fairness = read(results / "multi_application_fairness.csv")
    apps = sorted({int(row["application_count"]) for row in scaling if row.get("application_count", "").isdigit()})
    workers = sorted({int(row["worker_count"]) for row in scaling if row.get("worker_count", "").isdigit()})
    report: list[dict[str, str]] = []

    def graph(graph_id: str, filename: str, title: str, x_label: str, y_label: str, labels: list[str],
              series: list[tuple[str, list[float | None]]], source: str) -> None:
        prepared = [(name, [item if item is not None else float("nan") for item in values]) for name, values in series]
        valid = any(item == item for _, values in prepared for item in values)
        ok = valid and bar_chart(output / filename, title, x_label, y_label, labels, prepared, source, "REAL")
        report.append({"graph_id": graph_id, "graph_name": filename, "status": "GENERATED" if ok else "NO_DATA",
                       "reason": "" if ok else "no finite measured values", "output_file": str((output / filename).relative_to(root)) if ok else ""})

    def application_series(metric: str) -> list[tuple[str, list[float | None]]]:
        return [(f"{worker} workers", [finite(next((row.get(metric) for row in scaling if row.get("mode") == "INTEGRATED" and row.get("application_count") == str(count) and row.get("worker_count") == str(worker)), None)) for count in apps]) for worker in workers]

    def worker_series(metric: str) -> list[tuple[str, list[float | None]]]:
        return [(f"{count} apps", [finite(next((row.get(metric) for row in scaling if row.get("mode") == "INTEGRATED" and row.get("application_count") == str(count) and row.get("worker_count") == str(worker)), None)) for worker in workers]) for count in apps]

    app_labels, worker_labels = [str(item) for item in apps], [str(item) for item in workers]
    graph("MAP-G01", "application_count_vs_aggregate_throughput.svg", "Application Count vs Aggregate Throughput", "Applications", "Operations per second", app_labels, application_series("aggregate_throughput"), "results/multi_application_scaling.csv")
    graph("MAP-G02", "application_count_vs_runtime_cpu.svg", "Application Count vs Runtime CPU", "Applications", "Mean percent", app_labels, application_series("runtime_cpu_percent"), "results/multi_application_scaling.csv")
    graph("MAP-G03", "application_count_vs_runtime_rss.svg", "Application Count vs Runtime RSS", "Applications", "Mean KiB", app_labels, application_series("runtime_rss_kb"), "results/multi_application_scaling.csv")
    graph("MAP-G04", "application_count_vs_queue_wait.svg", "Application Count vs Queue Wait", "Applications", "Mean microseconds", app_labels, application_series("mean_queue_wait_us"), "results/multi_application_scaling.csv")
    graph("MAP-G05", "application_count_vs_max_queue_depth.svg", "Application Count vs Maximum Queue Depth", "Applications", "Maximum queued jobs", app_labels, application_series("max_queue_depth"), "results/multi_application_scaling.csv")
    graph("MAP-G06", "worker_count_vs_queue_wait.svg", "Worker Count vs Queue Wait", "Workers", "Mean microseconds", worker_labels, worker_series("mean_queue_wait_us"), "results/multi_application_scaling.csv")
    graph("MAP-G07", "worker_count_vs_completion_latency.svg", "Worker Count vs Completion Latency", "Workers", "Mean microseconds", worker_labels, worker_series("mean_completion_latency_us"), "results/multi_application_scaling.csv")
    graph("MAP-G08", "application_count_vs_pipeline_serial_wait.svg", "Application Count vs Pipeline Serial Wait", "Applications", "Mean microseconds", app_labels, application_series("mean_serial_wait_us"), "results/multi_application_scaling.csv")
    graph("MAP-G09", "worker_count_vs_pipeline_serial_wait.svg", "Worker Count vs Pipeline Serial Wait", "Workers", "Mean microseconds", worker_labels, worker_series("mean_serial_wait_us"), "results/multi_application_scaling.csv")

    overhead = []
    for worker in workers:
        values = []
        for count in apps:
            baseline = value(summary, count, worker, "BASELINE", "benchmark_elapsed_ms")
            integrated = value(summary, count, worker, "INTEGRATED", "benchmark_elapsed_ms")
            values.append((integrated - baseline) / baseline * 100.0 if baseline and integrated is not None else None)
        overhead.append((f"{worker} workers", values))
    graph("MAP-G10", "application_count_vs_benchmark_overhead.svg", "Application Count vs Benchmark Overhead", "Applications", "Percent; positive is slower", app_labels, overhead, "results/multi_application_performance_summary.csv")

    fairness_values = []
    for worker in workers:
        values = []
        for count in apps:
            selected = [finite(row.get("jain_fairness_index")) for row in fairness if row.get("application_count") == str(count) and row.get("worker_count") == str(worker) and "mode=INTEGRATED" in row.get("configuration", "")]
            selected = [item for item in selected if item is not None]
            values.append(sum(selected) / len(selected) if selected else None)
        fairness_values.append((f"{worker} workers", values))
    graph("MAP-G11", "fairness_index_by_configuration.svg", "Jain Fairness Index by Configuration", "Applications", "Index; 1 is equal counts", app_labels, fairness_values, "results/multi_application_fairness.csv")

    cycle_labels, cycle_values = [], []
    for row in fairness:
        if row.get("application_count") != "8" or row.get("worker_count") != "2" or "mode=INTEGRATED" not in row.get("configuration", ""):
            continue
        if row.get("run_id", "").find("measured") < 0:
            continue
        cycle_labels.append(row.get("application_index", "?"))
        cycle_values.append(finite(row.get("completed_cycles")))
    graph("MAP-G12", "per_application_completed_cycle_counts.svg", "Per-Application Completed Cycle Counts", "Application index; 8 apps, 2 workers", "Completed evaluations", cycle_labels, [("completed", cycle_values)], "results/multi_application_fairness.csv")

    phase3 = [(f"{worker} workers", [value(summary, count, worker, "INTEGRATED", "mean_phase3_us") for count in apps]) for worker in workers]
    phase456 = []
    for worker in workers:
        totals = []
        for count in apps:
            phases = [value(summary, count, worker, "INTEGRATED", metric) for metric in ("mean_phase4_us", "mean_phase5_us", "mean_phase6_us")]
            totals.append(sum(item for item in phases if item is not None) if all(item is not None for item in phases) else None)
        phase456.append((f"{worker} workers Phase 4-6", totals))
    graph("MAP-G13", "phase3_vs_phase4_6_cost_by_scale.svg", "Phase 3 vs Phase 4-6 Cost by Scale", "Applications", "Mean microseconds", app_labels, phase3 + phase456, "results/multi_application_performance_summary.csv")
    discovery = [(f"{worker} workers", [value(summary, count, worker, "INTEGRATED", "discovery_total_us") for count in apps]) for worker in workers]
    graph("MAP-G14", "discovery_contribution_vs_application_count.svg", "Discovery Contribution vs Application Count", "Applications", "Total discovery microseconds per run", app_labels, discovery, "results/multi_application_performance_summary.csv")

    with (results / "multi_application_graph_summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=("graph_id", "graph_name", "status", "reason", "output_file"))
        writer.writeheader()
        writer.writerows(report)
    print(f"Multi-application performance graphs: generated={sum(row['status'] == 'GENERATED' for row in report)} no_data={sum(row['status'] == 'NO_DATA' for row in report)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
