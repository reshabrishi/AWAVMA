#!/usr/bin/env python3
"""Synthetic validation for Phase 4-6 pipeline SVG generation."""

from __future__ import annotations

import csv
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/generate_phase46_pipeline_graphs.py"
NAMES = (
    "serial_wait_vs_application_count.svg",
    "phase456_execution_breakdown.svg",
    "subprocess_child_wall_by_phase.svg",
    "parent_prepare_result_parse.svg",
    "pipeline_total_before_vs_after.svg",
    "aggregate_throughput_before_vs_after.svg",
    "completion_latency_before_vs_after.svg",
    "runtime_cpu_before_vs_after.svg",
)


def write(path: Path, fields: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def valid(path: Path) -> bool:
    try:
        root = ElementTree.parse(path).getroot()
        return root.tag.endswith("svg") and not re.search(r"(?<![a-z])(nan|inf)(?![a-z])", path.read_text(encoding="utf-8").lower())
    except (OSError, ElementTree.ParseError):
        return False


def fixture_rows(variants: tuple[str, ...]) -> tuple[list[dict[str, str]], list[dict[str, str]], list[dict[str, str]]]:
    raw, summary, runs = [], [], []
    for variant_index, variant in enumerate(variants):
        for count in ("1", "2"):
            for phase_index, phase in enumerate(("phase4", "phase5", "phase6"), 4):
                values = {
                    "serial_wait_us": str(10 + variant_index + int(count)),
                    "parent_prepare_us": str(20 + phase_index + variant_index),
                    "fork_wait_wall_us": str(100 + phase_index + variant_index),
                    "child_profiled_total_us": str(80 + phase_index + variant_index),
                    "total_phase_us": str(200 + phase_index + variant_index),
                    "result_parse_us": str(30 + variant_index) if phase == "phase6" else "NA",
                    "completion_latency_us": str(300 + int(count) + variant_index),
                }
                raw.append({"variant": variant, "application_count": count, "run_type": "MEASURED", "phase": phase, "status": "OK", **values})
                for metric, value in values.items():
                    summary.append({"variant": variant, "application_count": count, "phase": phase, "metric": metric,
                                    "mean": value, "status": "MEASURED" if value != "NA" else "UNAVAILABLE"})
            runs.append({"variant": variant, "application_count": count, "run_type": "MEASURED", "status": "MEASURED",
                         "pipeline_total_us": str(500 + int(count) + variant_index),
                         "aggregate_throughput": str(600 + int(count) + variant_index),
                         "runtime_cpu_percent": str(40 + int(count) + variant_index)})
    return raw, summary, runs


def run_case(root: Path) -> list[dict[str, str]]:
    result = subprocess.run([sys.executable, str(SCRIPT), "--root", str(root)], capture_output=True, text=True)
    if result.returncode:
        raise AssertionError(result.stderr)
    report_path = root / "results/phase46_pipeline_graph_summary.csv"
    report = list(csv.DictReader(report_path.open(newline="", encoding="utf-8")))
    directory = root / "graphs/phase46-pipeline"
    if len(report) != len(NAMES) or {row["graph_name"] for row in report} != set(NAMES):
        raise AssertionError("expected exactly eight graph summary rows")
    if {path.name for path in directory.glob("*.svg")} != set(NAMES) or not all(valid(directory / name) for name in NAMES):
        raise AssertionError("expected exactly eight valid dependency-free SVGs")
    return report


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="awavma-phase46-graphs-") as temporary:
        root, results = Path(temporary), Path(temporary) / "results"
        results.mkdir()
        raw_fields = ("variant", "application_count", "run_type", "phase", "status", "serial_wait_us", "parent_prepare_us", "fork_wait_wall_us", "child_profiled_total_us", "total_phase_us", "result_parse_us", "completion_latency_us")
        summary_fields = ("variant", "application_count", "phase", "metric", "mean", "status")
        run_fields = ("variant", "application_count", "run_type", "status", "pipeline_total_us", "aggregate_throughput", "runtime_cpu_percent")
        raw, summary, runs = fixture_rows(("SUBPROCESS_SERIAL_BASELINE",))
        write(results / "phase46_pipeline_profile_raw.csv", raw_fields, raw)
        write(results / "phase46_pipeline_profile_summary.csv", summary_fields, summary)
        write(results / "phase46_pipeline_profile_runs.csv", run_fields, runs)
        report = run_case(root)
        if sum(row["status"] == "GENERATED" for row in report) != 4 or any(row["status"] != "NO_DATA" for row in report[4:]):
            raise AssertionError("single-variant comparisons must be NO_DATA")
        if not all("NO_DATA" in (root / "graphs/phase46-pipeline" / row["graph_name"]).read_text(encoding="utf-8") for row in report[4:]):
            raise AssertionError("NO_DATA comparisons must not fabricate values")
        raw, summary, runs = fixture_rows(("SUBPROCESS_SERIAL_BASELINE", "IN_PROCESS_PIPELINE"))
        write(results / "phase46_pipeline_profile_raw.csv", raw_fields, raw)
        write(results / "phase46_pipeline_profile_summary.csv", summary_fields, summary)
        write(results / "phase46_pipeline_profile_runs.csv", run_fields, runs)
        report = run_case(root)
        if any(row["status"] != "GENERATED" for row in report):
            raise AssertionError("paired variants should generate all eight graphs")
    print("phase46_pipeline_graph_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
