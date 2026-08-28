#!/usr/bin/env python3
"""Generate Phase 10 comparison SVGs without changing Phase 9 graph code."""

from __future__ import annotations

import argparse
import csv
import logging
from pathlib import Path

from graph_utils import bar_chart, finite, line_chart


def rows(path: Path) -> list[dict[str, str]]:
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
    args.log.parent.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(filename=args.log, level=logging.INFO, format="%(asctime)s %(message)s")
    summary_rows = rows(args.root / "results/phase10_summary.csv")
    raw_rows = rows(args.root / "results/phase10_raw_results.csv")
    report: list[dict[str, str]] = []

    def add(graph_id: str, filename: str, metric: str, status: str, reason: str = "") -> None:
        report.append({"graph_id": graph_id, "graph_name": filename, "metric": metric, "status": status, "reason": reason, "output_file": str((args.output_dir / filename).relative_to(args.root)) if status == "GENERATED" else ""})
        logging.info("graph=%s metric=%s status=%s reason=%s", graph_id, metric, status, reason)

    for graph_id, filename, metric, title, ylabel in (
        ("P10-G01", "execution_time_baseline_vs_awavma.svg", "execution_time", "Execution Time: Baseline vs AWAVMA", "Seconds"),
        ("P10-G02", "throughput_baseline_vs_awavma.svg", "throughput", "Throughput: Baseline vs AWAVMA", "Operations/sec"),
        ("P10-G03", "cpu_utilization_baseline_vs_awavma.svg", "cpu_utilization", "CPU Utilization: Baseline vs AWAVMA", "Percent"),
        ("P10-G04", "rss_baseline_vs_awavma.svg", "rss", "RSS: Baseline vs AWAVMA", "KB"),
        ("P10-G05", "page_faults_baseline_vs_awavma.svg", "page_faults", "Page Faults: Baseline vs AWAVMA", "Faults"),
    ):
        selected = [row for row in summary_rows if row.get("metric") == metric and row.get("input_type") == "REAL" and row.get("status") == "MEASURED"]
        categories = [row["workload"] for row in selected]
        baseline = [finite(row.get("baseline_mean")) for row in selected]
        awavma = [finite(row.get("awavma_mean")) for row in selected]
        if categories and all(value is not None for value in baseline + awavma):
            ok = bar_chart(args.output_dir / filename, title, "Workload", ylabel, categories, [("Baseline", baseline), ("AWAVMA", awavma)], "results/phase10_summary.csv", "REAL")
            add(graph_id, filename, metric, "GENERATED" if ok else "NO_DATA", "SVG validation failed" if not ok else "")
        else:
            add(graph_id, filename, metric, "NO_DATA", "paired finite REAL samples unavailable")

    overhead_metrics = ("monitoring_overhead", "classifier_overhead", "decision_overhead", "validation_overhead", "migration_overhead", "feedback_overhead", "total_overhead")
    selected = [row for row in summary_rows if row.get("metric") in overhead_metrics and row.get("status") == "MEASURED"]
    categories = [f"{row['workload']}:{row['metric']}" for row in selected]
    values = [finite(row.get("awavma_mean")) for row in selected]
    if categories and all(value is not None for value in values):
        ok = bar_chart(args.output_dir / "awavma_overhead_by_workload.svg", "AWAVMA Measured Overhead by Workload", "Workload: component", "Seconds", categories, [("Measured overhead", values)], "results/phase10_summary.csv", "REAL")
        add("P10-G06", "awavma_overhead_by_workload.svg", "overhead", "GENERATED" if ok else "NO_DATA", "SVG validation failed" if not ok else "")
    else:
        add("P10-G06", "awavma_overhead_by_workload.svg", "overhead", "NO_DATA", "no finite overhead values")

    multi = [row for row in raw_rows if row.get("workload") == "multi_application" and row.get("status") == "MEASURED"]
    add("P10-G07", "multi_application_completion_time.svg", "multi_application", "NO_DATA", "full multi-application AWAVMA CLI unavailable")

    repeat_series = []
    for workload in sorted({row.get("workload") for row in raw_rows if row.get("workload") in ("sequential", "random", "hot", "moderate", "cold", "mixed", "changing", "local")}):
        for mode in ("baseline", "awavma"):
            values = [(float(row["run_number"]), finite(row.get("execution_time"))) for row in raw_rows if row.get("workload") == workload and row.get("run_id", "").startswith(mode + "-") and row.get("status") == "MEASURED" and finite(row.get("execution_time")) is not None]
            if values:
                repeat_series.append((f"{workload}-{mode}", values))
    if repeat_series:
        ok = line_chart(args.output_dir / "repeatability_distribution.svg", "Phase 10 Repeatability Samples", "Run number", "Wall-clock seconds", repeat_series, "results/phase10_raw_results.csv", "REAL")
        add("P10-G08", "repeatability_distribution.svg", "repeatability", "GENERATED" if ok else "NO_DATA", "SVG validation failed" if not ok else "")
    else:
        add("P10-G08", "repeatability_distribution.svg", "repeatability", "NO_DATA", "no finite repeated samples")

    args.summary.parent.mkdir(parents=True, exist_ok=True)
    with args.summary.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=("graph_id", "graph_name", "metric", "status", "reason", "output_file"))
        writer.writeheader()
        writer.writerows(report)
    print(f"Phase 10 graph generation completed: generated={sum(row['status'] == 'GENERATED' for row in report)} no_data={sum(row['status'] == 'NO_DATA' for row in report)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
