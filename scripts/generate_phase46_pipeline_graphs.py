#!/usr/bin/env python3
"""Generate dependency-free SVGs for the Phase 4-6 pipeline profile."""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path
from xml.sax.saxutils import escape

from graph_utils import bar_chart, finite, read_csv, validate_svg


GRAPH_FIELDS = ("graph_id", "graph_name", "status", "reason", "output_file")
BASELINE_VARIANT = "SUBPROCESS_SERIAL_BASELINE"
GRAPHS = (
    ("P46-G01", "serial_wait_vs_application_count.svg"),
    ("P46-G02", "phase456_execution_breakdown.svg"),
    ("P46-G03", "subprocess_child_wall_by_phase.svg"),
    ("P46-G04", "parent_prepare_result_parse.svg"),
    ("P46-G05", "pipeline_total_before_vs_after.svg"),
    ("P46-G06", "aggregate_throughput_before_vs_after.svg"),
    ("P46-G07", "completion_latency_before_vs_after.svg"),
    ("P46-G08", "runtime_cpu_before_vs_after.svg"),
)


def measured(row: dict[str, str], summary: bool = False) -> bool:
    """Accept only measured records; raw phase rows use OK for completion."""
    if summary:
        return row.get("status") == "MEASURED"
    return row.get("run_type") == "MEASURED" and row.get("status") in ("MEASURED", "OK")


def mean(values: list[float | None]) -> float | None:
    usable = [value for value in values if value is not None]
    return sum(usable) / len(usable) if usable else None


def app_counts(*row_sets: list[dict[str, str]]) -> list[int]:
    counts: set[int] = set()
    for rows in row_sets:
        for row in rows:
            try:
                counts.add(int(row.get("application_count", "")))
            except ValueError:
                pass
    return sorted(counts)


def display_variant(variant: str) -> str:
    return variant.replace("_", " ").title()


def write_no_data_svg(path: Path, title: str, source: str, reason: str) -> bool:
    """Keep the requested graph artifact while making unavailable data explicit."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "\n".join((
            '<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="650" viewBox="0 0 1100 650">',
            '<rect width="100%" height="100%" fill="#ffffff"/>',
            '<text x="70" y="34" font-family="Arial,Helvetica,sans-serif" font-size="22" font-weight="bold" fill="#20252b">' + escape(title) + "</text>",
            '<text x="70" y="55" font-family="Arial,Helvetica,sans-serif" font-size="12" fill="#59636e">Source: ' + escape(source) + " | Data: REAL</text>",
            '<text x="550" y="300" text-anchor="middle" font-family="Arial,Helvetica,sans-serif" font-size="30" font-weight="bold" fill="#59636e">NO_DATA</text>',
            '<text x="550" y="330" text-anchor="middle" font-family="Arial,Helvetica,sans-serif" font-size="14" fill="#59636e">' + escape(reason) + "</text>",
            "</svg>",
            "",
        )),
        encoding="utf-8",
    )
    return validate_svg(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()

    root = args.root.resolve()
    results = root / "results"
    output = args.output_dir or root / "graphs/phase46-pipeline"
    if not output.is_absolute():
        output = root / output
    output.mkdir(parents=True, exist_ok=True)
    expected_names = {filename for _, filename in GRAPHS}
    for path in output.glob("*.svg"):
        if path.name not in expected_names:
            path.unlink()

    raw, _, raw_error = read_csv(results / "phase46_pipeline_profile_raw.csv", ("variant", "application_count", "run_type", "phase", "status"))
    summary, _, summary_error = read_csv(results / "phase46_pipeline_profile_summary.csv", ("variant", "application_count", "phase", "metric", "mean", "status"))
    runs, _, runs_error = read_csv(results / "phase46_pipeline_profile_runs.csv", ("variant", "application_count", "run_type", "status"))
    input_errors = [error for error in (raw_error, summary_error, runs_error) if error]
    counts = app_counts(raw, summary, runs)
    report: list[dict[str, str]] = []

    def summary_value(variant: str, count: int, phase: str, metric: str) -> float | None:
        return mean([finite(row.get("mean")) for row in summary if measured(row, summary=True)
                     and row.get("variant") == variant and row.get("application_count") == str(count)
                     and row.get("phase") == phase and row.get("metric") == metric])

    def run_value(variant: str, count: int, metric: str) -> float | None:
        return mean([finite(row.get(metric)) for row in runs if measured(row)
                     and row.get("variant") == variant and row.get("application_count") == str(count)])

    summary_variants = {row.get("variant", "") for row in summary if row.get("variant") and measured(row, summary=True)}
    run_variants = {row.get("variant", "") for row in runs if row.get("variant") and measured(row)}
    variants = sorted(summary_variants | run_variants)
    baseline = BASELINE_VARIANT if BASELINE_VARIANT in variants else (variants[0] if variants else "")
    after = next((variant for variant in variants if variant != baseline), "")

    def record(graph_id: str, filename: str, title: str, source: str, status: str, reason: str = "") -> None:
        path = output / filename
        if status == "NO_DATA":
            if not write_no_data_svg(path, title, source, reason):
                raise RuntimeError(f"invalid NO_DATA SVG: {path}")
        report.append({"graph_id": graph_id, "graph_name": filename, "status": status, "reason": reason,
                       "output_file": str(path.relative_to(root))})

    def bar(graph_id: str, filename: str, title: str, x_label: str, y_label: str,
            labels: list[str], series: list[tuple[str, list[float | None]]], source: str) -> None:
        reason = "; ".join(input_errors) if input_errors else "no finite measured values"
        prepared = [(name, [value if value is not None else math.nan for value in values]) for name, values in series]
        valid = bool(labels) and any(math.isfinite(value) for _, values in prepared for value in values)
        ok = valid and bar_chart(output / filename, title, x_label, y_label, labels, prepared, source, "REAL")
        record(graph_id, filename, title, source, "GENERATED" if ok else "NO_DATA", "" if ok else reason)

    labels = [str(count) for count in counts]
    serial_series = [(display_variant(variant), [summary_value(variant, count, "phase4", "serial_wait_us") for count in counts]) for variant in variants]
    bar("P46-G01", "serial_wait_vs_application_count.svg", "Serial Wait vs Application Count", "Applications", "Mean microseconds", labels, serial_series, "results/phase46_pipeline_profile_summary.csv")

    phase_series = [(phase.title(), [summary_value(baseline, count, phase, "total_phase_us") for count in counts]) for phase in ("phase4", "phase5", "phase6")]
    bar("P46-G02", "phase456_execution_breakdown.svg", "Phase 4-6 Execution Breakdown", "Applications", "Mean microseconds", labels, phase_series, "results/phase46_pipeline_profile_summary.csv")

    child_wall = [(phase.title(), [summary_value(baseline, count, phase, "child_profiled_total_us") for count in counts]) for phase in ("phase4", "phase5", "phase6")]
    bar("P46-G03", "subprocess_child_wall_by_phase.svg", "Subprocess Child Wall Time by Phase", "Applications", "Mean microseconds", labels, child_wall, "results/phase46_pipeline_profile_summary.csv")

    preparation = [("Parent prepare", [mean([summary_value(baseline, count, phase, "parent_prepare_us") for count in counts]) for phase in ("phase4", "phase5", "phase6")])]
    parsing = [("Result parse", [mean([summary_value(baseline, count, phase, "result_parse_us") for count in counts]) for phase in ("phase4", "phase5", "phase6")])]
    parent_values = preparation[0][1]
    parse_values = parsing[0][1]
    bar("P46-G04", "parent_prepare_result_parse.svg", "Parent Prepare and Result Parse", "Phase", "Mean microseconds", ["Phase 4", "Phase 5", "Phase 6"], [("Parent prepare", parent_values), ("Result parse", parse_values)], "results/phase46_pipeline_profile_summary.csv")

    def comparison(graph_id: str, filename: str, title: str, metric: str, source: str, value) -> None:
        if not after:
            record(graph_id, filename, title, source, "NO_DATA", "second measured variant unavailable")
            return
        paired = [(count, value(baseline, count, metric), value(after, count, metric)) for count in counts]
        paired = [(count, before_value, after_value) for count, before_value, after_value in paired
                  if before_value is not None and after_value is not None]
        if not paired:
            record(graph_id, filename, title, source, "NO_DATA", "paired finite measured values unavailable")
            return
        bar(graph_id, filename, title, "Applications", "Mean microseconds" if metric in ("pipeline_total_us", "completion_latency_us") else ("Operations per second" if metric == "aggregate_throughput" else "Mean percent"),
            [str(count) for count, _, _ in paired], [(display_variant(baseline), [before_value for _, before_value, _ in paired]), (display_variant(after), [after_value for _, _, after_value in paired])], source)

    comparison("P46-G05", "pipeline_total_before_vs_after.svg", "Pipeline Total: Before vs After", "pipeline_total_us", "results/phase46_pipeline_profile_runs.csv", run_value)
    comparison("P46-G06", "aggregate_throughput_before_vs_after.svg", "Aggregate Throughput: Before vs After", "aggregate_throughput", "results/phase46_pipeline_profile_runs.csv", run_value)
    comparison("P46-G07", "completion_latency_before_vs_after.svg", "Completion Latency: Before vs After", "completion_latency_us", "results/phase46_pipeline_profile_summary.csv", lambda variant, count, metric: summary_value(variant, count, "phase4", metric))
    comparison("P46-G08", "runtime_cpu_before_vs_after.svg", "Runtime CPU: Before vs After", "runtime_cpu_percent", "results/phase46_pipeline_profile_runs.csv", run_value)

    with (results / "phase46_pipeline_graph_summary.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=GRAPH_FIELDS)
        writer.writeheader()
        writer.writerows(report)
    print(f"Phase 4-6 pipeline graphs: generated={sum(row['status'] == 'GENERATED' for row in report)} no_data={sum(row['status'] == 'NO_DATA' for row in report)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
