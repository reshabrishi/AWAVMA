#!/usr/bin/env python3
"""Synthetic validation for multi-application SVG generation."""

from __future__ import annotations

import csv
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/generate_multi_application_performance_graphs.py"


def write(path: Path, fields: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def valid(path: Path) -> bool:
    try:
        svg = ElementTree.parse(path).getroot()
        return svg.tag.endswith("svg") and not re.search(r"(?<![a-z])(nan|inf)(?![a-z])", path.read_text(encoding="utf-8").lower())
    except (OSError, ElementTree.ParseError):
        return False


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="awavma-multi-graphs-") as temporary:
        root, results = Path(temporary), Path(temporary) / "results"
        results.mkdir()
        scaling_fields = ("mode", "application_count", "worker_count", "aggregate_throughput", "runtime_cpu_percent", "runtime_rss_kb", "mean_queue_wait_us", "max_queue_depth", "mean_serial_wait_us", "mean_completion_latency_us", "fairness_index", "status")
        scaling = []
        for apps in ("1", "2"):
            for workers in ("1", "2"):
                scaling.append({"mode": "INTEGRATED", "application_count": apps, "worker_count": workers, "aggregate_throughput": "100", "runtime_cpu_percent": "10", "runtime_rss_kb": "1000", "mean_queue_wait_us": "20", "max_queue_depth": "2", "mean_serial_wait_us": "30", "mean_completion_latency_us": "40", "fairness_index": "1", "status": "MEASURED"})
        write(results / "multi_application_scaling.csv", scaling_fields, scaling)
        summary_fields = ("scope", "application_count", "worker_count", "mode", "metric", "mean", "status")
        summary = []
        for apps in ("1", "2"):
            for workers in ("1", "2"):
                for mode, elapsed in (("BASELINE", "10"), ("INTEGRATED", "11")):
                    for metric, value in (("benchmark_elapsed_ms", elapsed), ("mean_phase3_us", "2"), ("mean_phase4_us", "3"), ("mean_phase5_us", "4"), ("mean_phase6_us", "5"), ("discovery_total_us", "6")):
                        summary.append({"scope": "APPLICATION", "application_count": apps, "worker_count": workers, "mode": mode, "metric": metric, "mean": value, "status": "MEASURED"})
        write(results / "multi_application_performance_summary.csv", summary_fields, summary)
        fairness_fields = ("configuration", "run_id", "application_count", "worker_count", "application_index", "completed_cycles", "jain_fairness_index")
        fairness = [{"configuration": "apps=8;workers=2;mode=INTEGRATED", "run_id": "measured-1", "application_count": "8", "worker_count": "2", "application_index": str(index), "completed_cycles": "3", "jain_fairness_index": "1"} for index in range(8)]
        fairness.extend({"configuration": "apps=1;workers=1;mode=INTEGRATED", "run_id": "measured-1", "application_count": "1", "worker_count": "1", "application_index": "0", "completed_cycles": "3", "jain_fairness_index": "1"} for _ in range(1))
        write(results / "multi_application_fairness.csv", fairness_fields, fairness)
        result = subprocess.run([sys.executable, str(SCRIPT), "--root", str(root)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        report = list(csv.DictReader((results / "multi_application_graph_summary.csv").open(newline="", encoding="utf-8")))
        directory = root / "graphs/multi-application-performance"
        if len(report) != 14 or any(row["status"] != "GENERATED" for row in report) or not all(valid(directory / row["graph_name"]) for row in report):
            raise AssertionError("expected fourteen valid SVGs")
    print("multi_application_graph_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
