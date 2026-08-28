#!/usr/bin/env python3
"""Generate SVG graphs for discovery cadence measurements."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from graph_utils import bar_chart, finite


def read(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    results = args.root / "results"
    output = args.root / "graphs/discovery-cadence"
    output.mkdir(parents=True, exist_ok=True)
    summary = read(results / "discovery_cadence_summary.csv")
    detection = read(results / "discovery_cadence_detection_latency.csv")
    reference_summary = read(results / "full_system_performance_summary.csv")
    candidate_summary = read(results / "full_system_performance_discovery_optimized_summary.csv")
    reference_overhead = read(results / "full_system_performance_overhead.csv")
    candidate_overhead = read(results / "full_system_performance_discovery_optimized_overhead.csv")
    intervals = sorted({row.get("interval_ms", "") for row in summary if row.get("interval_ms", "").isdigit()}, key=int)
    report = []

    def values(metric: str, mode: str = "INTEGRATED") -> list[float]:
        output_values = []
        for interval in intervals:
            row = next((row for row in summary if row.get("interval_ms") == interval and row.get("mode") == mode and row.get("metric") == metric), {})
            value = finite(row.get("mean"))
            output_values.append(value if value is not None else float("nan"))
        return output_values

    def graph(graph_id: str, filename: str, title: str, y_label: str, series: list[tuple[str, list[float]]]) -> None:
        valid = any(value == value for _, row in series for value in row)
        ok = valid and bar_chart(output / filename, title, "Discovery interval (ms)", y_label, intervals, series,
                                 "results/discovery_cadence_summary.csv", "REAL")
        report.append({"graph_id": graph_id, "graph_name": filename, "status": "GENERATED" if ok else "NO_DATA",
                       "reason": "" if ok else "no finite measurements", "output_file": str((output / filename).relative_to(args.root)) if ok else ""})

    graph("DC-G01", "interval_vs_scan_count.svg", "Discovery Interval vs Scans per Run", "Mean scans", [("scans", values("discovery_scans"))])
    graph("DC-G02", "interval_vs_total_discovery_time.svg", "Discovery Interval vs Total Discovery Time", "Mean microseconds", [("discovery_total", values("discovery_total_us"))])
    graph("DC-G03", "interval_vs_runtime_cpu.svg", "Discovery Interval vs Runtime CPU", "Mean percent", [("runtime_cpu", values("runtime_cpu_percent"))])
    baseline = values("benchmark_elapsed_ms", "BASELINE")
    integrated = values("benchmark_elapsed_ms")
    overhead = [(current - reference) / reference * 100.0 if reference == reference and current == current and reference != 0 else float("nan") for reference, current in zip(baseline, integrated)]
    graph("DC-G04", "interval_vs_benchmark_overhead.svg", "Discovery Interval vs Benchmark Overhead", "Percent; positive is slower", [("overhead", overhead)])
    latency = []
    for interval in intervals:
        values_for_interval = [finite(row.get("latency_ms")) for row in detection if row.get("interval_ms") == interval and row.get("status") == "MEASURED"]
        values_for_interval = [value for value in values_for_interval if value is not None]
        latency.append(sum(values_for_interval) / len(values_for_interval) if values_for_interval else float("nan"))
    graph("DC-G05", "interval_vs_detection_latency.svg", "Discovery Interval vs Detection Latency", "Mean milliseconds", [("detection_latency", latency)])

    def pooled(summary_rows: list[dict[str, str]], metric: str) -> float:
        measured = [finite(row.get("mean")) for row in summary_rows
                    if row.get("mode") == "INTEGRATED" and row.get("metric") == metric and row.get("status") == "MEASURED"]
        values_for_metric = [value for value in measured if value is not None]
        return sum(values_for_metric) / len(values_for_metric) if values_for_metric else float("nan")

    phase_metrics = ("discovery_us", "queue_wait_us", "worker_execution_us", "phase3_us",
                     "pipeline_serial_wait_us", "phase4_us", "phase5_us", "phase6_us",
                     "coordinator_overhead_us")
    phase_labels = ("discovery", "queue wait", "worker", "phase 3", "pipeline wait", "phase 4", "phase 5", "phase 6", "coordinator")
    reference_phases = [pooled(reference_summary, metric) for metric in phase_metrics]
    candidate_phases = [pooled(candidate_summary, metric) for metric in phase_metrics]
    cycle_valid = any(value == value for value in reference_phases + candidate_phases)
    cycle_ok = cycle_valid and bar_chart(output / "before_vs_after_cycle_breakdown.svg",
                                        "Before vs After Runtime Cycle Breakdown", "Runtime component",
                                        "Mean microseconds", phase_labels,
                                        [("legacy", reference_phases), ("250 ms cadence", candidate_phases)],
                                        "results/full_system_performance*_summary.csv", "REAL")
    report.append({"graph_id": "DC-G06", "graph_name": "before_vs_after_cycle_breakdown.svg",
                   "status": "GENERATED" if cycle_ok else "NO_DATA",
                   "reason": "" if cycle_ok else "no finite reprofile values",
                   "output_file": str((output / "before_vs_after_cycle_breakdown.svg").relative_to(args.root)) if cycle_ok else ""})

    comparison_rows = []
    workload_labels = []
    reference_values = []
    candidate_values = []
    for reference in reference_overhead:
        if reference.get("metric") != "benchmark_elapsed_ms":
            continue
        workload = reference.get("workload", "")
        candidate = next((row for row in candidate_overhead if row.get("workload") == workload and row.get("metric") == "benchmark_elapsed_ms"), {})
        before = finite(reference.get("percent_change"))
        after = finite(candidate.get("percent_change"))
        status = "MEASURED" if before is not None and after is not None else "UNAVAILABLE"
        comparison_rows.append({"workload": workload, "legacy_overhead_percent": "NA" if before is None else f"{before:.3f}",
                                "cadence_250_overhead_percent": "NA" if after is None else f"{after:.3f}",
                                "change_percentage_points": "NA" if status != "MEASURED" else f"{after - before:.3f}", "status": status})
        workload_labels.append(workload)
        reference_values.append(before if before is not None else float("nan"))
        candidate_values.append(after if after is not None else float("nan"))
    with (results / "discovery_cadence_reprofile_comparison.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=("workload", "legacy_overhead_percent", "cadence_250_overhead_percent", "change_percentage_points", "status"))
        writer.writeheader()
        writer.writerows(comparison_rows)
    overhead_valid = any(value == value for value in reference_values + candidate_values)
    overhead_ok = overhead_valid and bar_chart(output / "before_vs_after_workload_overhead.svg",
                                                "Before vs After Workload Overhead", "Workload",
                                                "Percent; positive is slower", workload_labels,
                                                [("legacy", reference_values), ("250 ms cadence", candidate_values)],
                                                "results/discovery_cadence_reprofile_comparison.csv", "REAL")
    report.append({"graph_id": "DC-G07", "graph_name": "before_vs_after_workload_overhead.svg",
                   "status": "GENERATED" if overhead_ok else "NO_DATA",
                   "reason": "" if overhead_ok else "no finite reprofile values",
                   "output_file": str((output / "before_vs_after_workload_overhead.svg").relative_to(args.root)) if overhead_ok else ""})
    with (results / "discovery_cadence_graph_summary.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=("graph_id", "graph_name", "status", "reason", "output_file"))
        writer.writeheader()
        writer.writerows(report)
    print(f"Discovery cadence graphs: generated={sum(row['status'] == 'GENERATED' for row in report)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
