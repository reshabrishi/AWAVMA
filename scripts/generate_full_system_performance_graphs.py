#!/usr/bin/env python3
"""Generate dependency-free SVG graphs for integrated runtime measurements."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from graph_utils import bar_chart, finite


WORKLOADS = ("sequential", "random", "hot", "moderate", "cold", "mixed", "changing", "local")


def read(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--prefix", default="full_system_performance")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    results = args.root / "results"
    output = args.root / "graphs/full-system-performance" if args.output_dir is None else args.output_dir
    if not output.is_absolute():
        output = args.root / output
    output.mkdir(parents=True, exist_ok=True)
    summary = read(results / f"{args.prefix}_summary.csv")
    overhead = read(results / f"{args.prefix}_overhead.csv")
    report: list[dict[str, str]] = []

    def value(workload: str, mode: str, metric: str) -> float | None:
        for row in summary:
            if row.get("workload") == workload and row.get("mode") == mode and row.get("metric") == metric:
                return finite(row.get("mean"))
        return None

    def graph(graph_id: str, filename: str, title: str, ylabel: str,
              series: list[tuple[str, list[float | None]]], source: str) -> None:
        labels = []
        values = [[] for _ in series]
        for index, workload in enumerate(WORKLOADS):
            if any(index < len(row) and row[index] is not None for _, row in series):
                labels.append(workload)
                for destination, (_, row) in zip(values, series):
                    destination.append(float(row[index]) if row[index] is not None else float("nan"))
        valid_series = [(name, row) for (name, _), row in zip(series, values)]
        ok = bool(labels) and bar_chart(output / filename, title, "Workload", ylabel, labels, valid_series, source, "REAL")
        report.append({"graph_id": graph_id, "graph_name": filename, "status": "GENERATED" if ok else "NO_DATA",
                       "reason": "" if ok else "no finite measured values", "output_file": str((output / filename).relative_to(args.root)) if ok else ""})

    baseline_time = [value(workload, "BASELINE", "benchmark_elapsed_ms") for workload in WORKLOADS]
    integrated_time = [value(workload, "INTEGRATED", "benchmark_elapsed_ms") for workload in WORKLOADS]
    graph("FSP-G01", "baseline_vs_integrated_execution_time.svg", "Baseline vs Integrated Execution Time", "Mean milliseconds", [("baseline", baseline_time), ("integrated", integrated_time)], "results/full_system_performance_summary.csv")
    overhead_values = []
    for workload in WORKLOADS:
        row = next((row for row in overhead if row.get("workload") == workload and row.get("metric") == "benchmark_elapsed_ms"), {})
        overhead_values.append(finite(row.get("percent_change")))
    graph("FSP-G02", "integrated_execution_overhead.svg", "Integrated Execution-Time Overhead", "Percent; positive is slower", [("overhead", overhead_values)], "results/full_system_performance_overhead.csv")
    phase_metrics = ("phase3_us", "phase4_us", "phase5_us", "phase6_us", "coordinator_overhead_us")
    phase_series = [(metric, [value(workload, "INTEGRATED", metric) for workload in WORKLOADS]) for metric in phase_metrics]
    graph("FSP-G03", "phase_latency_breakdown.svg", "Integrated Phase Latency Breakdown", "Mean microseconds", phase_series, "results/full_system_performance_summary.csv")
    graph("FSP-G04", "queue_wait_vs_worker_execution.svg", "Queue Wait vs Worker Execution", "Mean microseconds", [("queue_wait", [value(w, "INTEGRATED", "queue_wait_us") for w in WORKLOADS]), ("worker_execution", [value(w, "INTEGRATED", "worker_execution_us") for w in WORKLOADS])], "results/full_system_performance_summary.csv")
    graph("FSP-G05", "pipeline_serial_wait.svg", "Pipeline Serialization Wait", "Mean microseconds", [("serial_wait", [value(w, "INTEGRATED", "pipeline_serial_wait_us") for w in WORKLOADS])], "results/full_system_performance_summary.csv")
    graph("FSP-G06", "runtime_cpu_overhead.svg", "AWAVMA Runtime CPU", "Mean percent", [("runtime_cpu", [value(w, "INTEGRATED", "runtime_cpu_percent") for w in WORKLOADS])], "results/full_system_performance_summary.csv")
    graph("FSP-G07", "runtime_memory_usage.svg", "AWAVMA Runtime Memory", "Mean KiB", [("RSS", [value(w, "INTEGRATED", "runtime_rss_kb") for w in WORKLOADS]), ("VMS", [value(w, "INTEGRATED", "runtime_vms_kb") for w in WORKLOADS])], "results/full_system_performance_summary.csv")
    graph("FSP-G08", "completion_latency.svg", "Monitoring Job Completion Latency", "Mean microseconds", [("completion", [value(w, "INTEGRATED", "completion_latency_us") for w in WORKLOADS])], "results/full_system_performance_summary.csv")
    throughput_baseline = [value(workload, "BASELINE", "benchmark_throughput_ops_sec") for workload in WORKLOADS]
    throughput_integrated = [value(workload, "INTEGRATED", "benchmark_throughput_ops_sec") for workload in WORKLOADS]
    graph("FSP-G09", "baseline_vs_integrated_throughput.svg", "Baseline vs Integrated Throughput", "Mean operations per second", [("baseline", throughput_baseline), ("integrated", throughput_integrated)], "results/full_system_performance_summary.csv")
    with (results / f"{args.prefix}_graph_summary.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=("graph_id", "graph_name", "status", "reason", "output_file"))
        writer.writeheader()
        writer.writerows(report)
    print(f"Full-system performance graphs: generated={sum(row['status'] == 'GENERATED' for row in report)} no_data={sum(row['status'] == 'NO_DATA' for row in report)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
