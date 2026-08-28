#!/usr/bin/env python3
"""Generate source-labeled SVGs for Phase 5/6 subprocess measurements."""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

from graph_utils import bar_chart, finite, validate_svg


GRAPHS = (
    ("P56-G01", "phase5_operation_breakdown.svg"), ("P56-G02", "phase6_operation_breakdown.svg"),
    ("P56-G03", "phase5_persistence_breakdown.svg"), ("P56-G04", "phase6_persistence_breakdown.svg"),
    ("P56-G05", "phase56_child_envelopes.svg"), ("P56-G06", "pipeline_before_vs_after.svg"),
    ("P56-G07", "throughput_before_vs_after.svg"),
)


def read(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream)) if path.is_file() else []


def no_data(path: Path, title: str, reason: str) -> bool:
    path.write_text("\n".join((
        '<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="650">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text x="70" y="50" font-family="Arial" font-size="24">{title}</text>',
        '<text x="550" y="300" text-anchor="middle" font-family="Arial" font-size="30">NO_DATA</text>',
        f'<text x="550" y="330" text-anchor="middle" font-family="Arial" font-size="14">{reason}</text>',
        '</svg>', "")), encoding="utf-8")
    return validate_svg(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    results, output = root / "results", root / "graphs/phase56-investigation"
    output.mkdir(parents=True, exist_ok=True)
    summary, persistence = read(results / "phase56_subprocess_profile_summary.csv"), read(results / "phase56_persistence_breakdown.csv")
    report = []

    def chart(identifier: str, filename: str, title: str, rows: list[dict[str, str]], phase: str,
              components: list[str], metric: str, source: str) -> None:
        counts = ["1", "2", "4", "8"]
        series = []
        for component in components:
            values = []
            for count in counts:
                row = next((item for item in rows if item.get("application_count") == count and (phase == "all" or item.get("phase") == phase)
                            and item.get("component") == component and item.get("status") == "MEASURED"), None)
                values.append(finite(row.get(metric)) if row else math.nan)
            series.append((component.removeprefix("p5_").removeprefix("p6_").replace("_", " "), values))
        valid = any(math.isfinite(value) for _, values in series for value in values)
        path = output / filename
        status = "GENERATED" if valid and bar_chart(path, title, "Applications", "Mean microseconds", counts, series, source, "REAL") else "NO_DATA"
        if status == "NO_DATA":
            no_data(path, title, "measured values unavailable")
        report.append({"graph_id": identifier, "graph_name": filename, "status": status,
                       "reason": "" if status == "GENERATED" else "measured values unavailable",
                       "output_file": str(path.relative_to(root))})

    chart("P56-G01", "phase5_operation_breakdown.svg", "Phase 5 Operation Breakdown", summary, "phase5",
          ["p5_state_path_prepare", "p5_state_load_or_initialize", "p5_decision_compute", "p5_output_history_write", "p5_application_state_persist"], "mean_us", "results/phase56_subprocess_profile_summary.csv")
    chart("P56-G02", "phase6_operation_breakdown.svg", "Phase 6 Operation Breakdown", summary, "phase6",
          ["p6_initialize", "p6_confidence_gate", "p6_roi_gate", "p6_safety_gate", "p6_output_history_write"], "mean_us", "results/phase56_subprocess_profile_summary.csv")
    chart("P56-G03", "phase5_persistence_breakdown.svg", "Phase 5 Persistence Breakdown", persistence, "phase5",
          ["p5_state_path_prepare", "p5_state_load_or_initialize", "p5_output_history_write", "p5_history_cleanup", "p5_application_state_persist", "p5_logging"], "mean_us", "results/phase56_persistence_breakdown.csv")
    chart("P56-G04", "phase6_persistence_breakdown.svg", "Phase 6 Persistence Breakdown", persistence, "phase6",
          ["p6_output_history_write", "p6_human_log_write", "p6_history_cleanup"], "mean_us", "results/phase56_persistence_breakdown.csv")
    chart("P56-G05", "phase56_child_envelopes.svg", "Phase 5/6 Child Envelopes", summary, "all",
          ["phase5_child_api_total", "phase6_child_validate_log"], "mean_us", "results/phase56_subprocess_profile_summary.csv")
    for identifier, filename in GRAPHS[5:]:
        path = output / filename
        title = filename.removesuffix(".svg").replace("_", " ").title()
        no_data(path, title, "no Phase 5/6 candidate introduced")
        report.append({"graph_id": identifier, "graph_name": filename, "status": "NO_DATA",
                       "reason": "no Phase 5/6 candidate introduced", "output_file": str(path.relative_to(root))})
    with (results / "phase56_investigation_graph_summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=("graph_id", "graph_name", "status", "reason", "output_file"))
        writer.writeheader()
        writer.writerows(report)
    print(f"Phase56 investigation graphs: generated={sum(row['status'] == 'GENERATED' for row in report)} no_data={sum(row['status'] == 'NO_DATA' for row in report)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
