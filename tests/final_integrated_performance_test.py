#!/usr/bin/env python3
"""Validate final integrated performance artifacts without rerunning benchmarks."""

from __future__ import annotations

import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"


def read(name: str) -> list[dict[str, str]]:
    path = RESULTS / name
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream)) if path.is_file() else []


def check(identifier: str, passed: bool) -> int:
    print(f"{identifier}: {'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


def main() -> int:
    failures = 0
    required = ("final_integrated_performance_raw.csv", "final_integrated_performance_summary.csv",
                "final_integrated_performance_overhead.csv", "final_integrated_multiapp_raw.csv",
                "final_integrated_multiapp_summary.csv", "final_integrated_status_counts.csv",
                "final_integrated_performance_report.md", "phase10_final_runtime_report.md",
                "final_integrated_performance_graph_summary.csv")
    failures += check("FIP-01", all((RESULTS / name).is_file() and (RESULTS / name).stat().st_size > 0 for name in required))
    single = read("final_integrated_performance_raw.csv")
    measured = [row for row in single if row.get("run_type") == "MEASURED" and row.get("status") == "MEASURED"]
    failures += check("FIP-02", len(measured) == 80 and {row.get("workload") for row in measured} == {"sequential", "random", "hot", "moderate", "cold", "mixed", "changing", "local"})
    failures += check("FIP-03", all(row.get("threads") == "2" and row.get("memory_mb") == "8" and row.get("iterations") == "1000000" for row in measured))
    multi = read("final_integrated_multiapp_raw.csv")
    multi_measured = [row for row in multi if row.get("run_type") == "MEASURED" and row.get("status") == "MEASURED"]
    failures += check("FIP-04", {row.get("application_count") for row in multi_measured} == {"1", "2", "4", "8"} and all(row.get("worker_count") == "2" for row in multi_measured))
    failures += check("FIP-05", all(row.get("iterations") == "17841231" for row in multi_measured))
    graphs = read("final_integrated_performance_graph_summary.csv")
    failures += check("FIP-06", len(graphs) == 12 and all(row.get("status") == "GENERATED" and (ROOT / row.get("output_file", "")).is_file() for row in graphs))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
