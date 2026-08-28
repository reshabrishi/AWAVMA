#!/usr/bin/env python3
"""Artifact and instrumentation checks for the Phase 5/6 investigation."""

from __future__ import annotations

import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
REQUIRED_COMPONENTS = {
    "phase5_child_api_total", "p5_state_load_or_initialize", "p5_decision_compute",
    "p5_output_history_write", "p5_application_state_persist", "phase6_child_validate_log",
    "p6_initialize", "p6_confidence_gate", "p6_roi_gate", "p6_safety_gate", "p6_output_history_write",
}


def read(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream)) if path.is_file() else []


def check(identifier: str, passed: bool) -> int:
    print(f"{identifier}: {'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


def main() -> int:
    failures = 0
    expected = [RESULTS / name for name in (
        "phase56_subprocess_profile_raw.csv", "phase56_subprocess_profile_summary.csv",
        "phase56_persistence_breakdown.csv", "phase56_candidate_comparison.csv",
        "phase56_investigation_report.md", "phase56_investigation_graph_summary.csv",
    )]
    failures += check("P56-01", all(path.is_file() and path.stat().st_size > 0 for path in expected))
    raw = read(RESULTS / "phase56_subprocess_profile_raw.csv")
    components = {row.get("component") for row in raw if row.get("status") == "OK"}
    failures += check("P56-02", REQUIRED_COMPONENTS.issubset(components))
    failures += check("P56-03", all(row.get("variant") == "SUBPROCESS_SERIAL_BASELINE" for row in raw))
    failures += check("P56-04", {row.get("application_count") for row in raw if row.get("run_type") == "MEASURED"} == {"1", "2", "4", "8"})
    comparison = read(RESULTS / "phase56_candidate_comparison.csv")
    failures += check("P56-05", len(comparison) == 4 and all(row.get("status") == "NO_CANDIDATE" for row in comparison))
    graphs = read(RESULTS / "phase56_investigation_graph_summary.csv")
    failures += check("P56-06", len(graphs) == 7 and all((ROOT / row.get("output_file", "")).is_file() for row in graphs))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
