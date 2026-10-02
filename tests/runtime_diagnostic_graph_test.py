#!/usr/bin/env python3
"""Diagnostic graph and unavailable-signal coverage."""
from __future__ import annotations
import csv, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/generate_multinuma_graphs.py"
HEADERS = ("timestamp","app_id","pid","access_value","classification_score","memory_score","thread_score","decision_margin","process_cpu_utilization_percent","interval_minor_faults","interval_major_faults","memory_dominant_node","thread_current_node","confidence_gate","roi_gate","safety_gate","decision","migration_result","rejection_reason")

def write(path, row):
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=HEADERS); writer.writeheader(); writer.writerow(row)

def run(diagnostic, output):
    result = subprocess.run([sys.executable, str(SCRIPT), "--input", str(output / "missing.csv"), "--summary-dir", str(output / "summaries"), "--output-dir", str(output), "--diagnostic", str(diagnostic)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    with (output / "graph_summary.csv").open(newline="", encoding="utf-8") as handle: return {row["graph"]: row for row in csv.DictReader(handle)}

def main():
    with tempfile.TemporaryDirectory(prefix="awavma-diagnostic-graphs-") as temporary:
        root = Path(temporary)
        measured = root / "measured.csv"
        write(measured, dict(zip(HEADERS, ("2026-10-02T00:00:00Z","app", "42", "50", "70", "NA", "1.2", "NA", "80", "2", "1", "1", "0", "PASS", "NOT_APPLICABLE", "PASS", "MOVE_THREAD", "NA", "NA"))))
        report = run(measured, root / "measured")
        assert report["diagnostic_cpu"]["status"] == "GENERATED"
        assert report["diagnostic_memory_score"]["status"] == "SKIPPED"
        assert report["diagnostic_signal_availability"]["status"] == "GENERATED"
        unavailable = root / "unavailable.csv"
        write(unavailable, {field:"NA" for field in HEADERS} | {"timestamp":"2026-10-02T00:00:00Z", "app_id":"app", "pid":"42"})
        report = run(unavailable, root / "unavailable")
        assert report["diagnostic_access"]["status"] == "SKIPPED"
        assert report["diagnostic_signal_availability"]["status"] == "GENERATED"
    print("runtime_diagnostic_graph_test: PASS")

if __name__ == "__main__": main()
