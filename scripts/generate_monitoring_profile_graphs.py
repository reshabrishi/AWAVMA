#!/usr/bin/env python3
"""Generate monitoring profiling graphs using the dependency-free SVG helpers."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from graph_utils import bar_chart, finite, line_chart


def read(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    summary = read(args.root / "results/monitoring_profile_summary.csv")
    applications = read(args.root / "results/monitoring_profile_application.csv")
    raw = read(args.root / "results/monitoring_profile_raw.csv")
    report = []

    def add(graph_id: str, filename: str, status: str, reason: str = "") -> None:
        report.append({"graph_id": graph_id, "graph_name": filename, "status": status, "reason": reason, "output_file": str((args.output_dir / filename).relative_to(args.root)) if status == "GENERATED" else ""})

    def graph_bar(graph_id: str, filename: str, title: str, labels: list[str], values: list[float], x_label: str, y_label: str) -> None:
        if labels and values:
            ok = bar_chart(args.output_dir / filename, title, x_label, y_label, labels, [("mean_us", values)], "results/monitoring_profile_summary.csv", "REAL")
            add(graph_id, filename, "GENERATED" if ok else "NO_DATA", "SVG validation failed" if not ok else "")
        else:
            add(graph_id, filename, "NO_DATA", "no finite profile values")

    selected = [row for row in summary if row.get("status") == "MEASURED" and row.get("component") in ("monitor_sample_total", "proc_pid_stat", "cpu_statistics", "memory_rss", "thread_statistics_total", "numa_information", "csv_formatting", "csv_write", "logging")]
    graph_bar("MP-G01", "monitoring_component_breakdown.svg", "Monitoring Component Breakdown", [row["component"] for row in selected], [finite(row.get("mean_us")) for row in selected if finite(row.get("mean_us")) is not None], "Component", "Mean microseconds")
    proc_names = ("proc_pid_stat", "memory_rss", "thread_enumeration", "per_thread_stat", "numa_information", "cpu_statistics")
    selected = [row for row in summary if row.get("status") == "MEASURED" and row.get("component") in proc_names]
    graph_bar("MP-G02", "proc_operation_breakdown.svg", "/proc Operation Breakdown", [row["component"] for row in selected], [finite(row.get("mean_us")) for row in selected if finite(row.get("mean_us")) is not None], "Operation", "Mean microseconds")
    selected = [row for row in summary if row.get("status") == "MEASURED" and row.get("component") in ("csv_formatting", "csv_write", "logging")]
    graph_bar("MP-G03", "csv_logging_overhead.svg", "CSV and Logging Overhead", [row["component"] for row in selected], [finite(row.get("mean_us")) for row in selected if finite(row.get("mean_us")) is not None], "Component", "Mean microseconds")
    if applications:
        labels = [row["application"] for row in applications]
        queue = [float(row["queue_wait_mean_us"]) for row in applications]
        monitor = [float(row["monitor_execution_mean_us"]) for row in applications]
        ok = bar_chart(args.output_dir / "queue_wait_vs_monitor_execution.svg", "Queue Wait vs Monitor Execution", "Application", "Mean microseconds", labels, [("queue_wait", queue), ("monitor_execution", monitor)], "results/monitoring_profile_application.csv", "CONTROLLED_RUNTIME")
        add("MP-G04", "queue_wait_vs_monitor_execution.svg", "GENERATED" if ok else "NO_DATA", "SVG validation failed" if not ok else "")
        ok = bar_chart(args.output_dir / "per_application_monitoring_time.svg", "Per-Application Monitoring Time", "Application", "Mean microseconds", labels, [("monitor_execution", monitor)], "results/monitoring_profile_application.csv", "CONTROLLED_RUNTIME")
        add("MP-G05", "per_application_monitoring_time.svg", "GENERATED" if ok else "NO_DATA", "SVG validation failed" if not ok else "")
    else:
        add("MP-G04", "queue_wait_vs_monitor_execution.svg", "NO_DATA", "runtime job lifecycle records unavailable")
        add("MP-G05", "per_application_monitoring_time.svg", "NO_DATA", "runtime job lifecycle records unavailable")
    workload_rows = [row for row in raw if row.get("component") == "monitor_sample_total" and row.get("workload") not in ("discovery", "multi_application") and "warmup" not in row.get("run_id", "")]
    grouped = {}
    for row in workload_rows:
        grouped.setdefault(row["workload"], []).append((int(row["start_time"]), float(row["duration_us"])))
    if grouped:
        ok = line_chart(args.output_dir / "monitoring_time_across_workloads.svg", "Monitoring Time Across Workloads", "Sample sequence", "Microseconds", [(name, [(float(index), value) for index, (_, value) in enumerate(values, 1)]) for name, values in sorted(grouped.items())], "results/monitoring_profile_raw.csv", "REAL")
        add("MP-G06", "monitoring_time_across_workloads.svg", "GENERATED" if ok else "NO_DATA", "SVG validation failed" if not ok else "")
    else:
        add("MP-G06", "monitoring_time_across_workloads.svg", "NO_DATA", "monitor sample records unavailable")
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    with args.summary.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=("graph_id", "graph_name", "status", "reason", "output_file"))
        writer.writeheader()
        writer.writerows(report)
    print(f"Monitoring profile graphs: generated={sum(row['status'] == 'GENERATED' for row in report)} no_data={sum(row['status'] == 'NO_DATA' for row in report)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
