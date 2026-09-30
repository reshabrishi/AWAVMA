#!/usr/bin/env python3
"""Fixture coverage for run scoping, comparison, and address rejection."""
from __future__ import annotations
import csv, json, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/aggregate_experiment_results.py"

def write_run(root: Path, name: str, address: bool = False) -> None:
    run = root / name
    run.mkdir()
    (run / "manifest.json").write_text(json.dumps({"run_id": name, "data_source": "REAL", "collection_status": "PASS", "topology_source": "metadata/topology.txt"}), encoding="utf-8")
    fields = ["schema_version", "experiment_id", "timestamp_utc", "run_id", "scenario", "workload", "repetition", "thread_node", "memory_node", "runtime_enabled", "threads", "memory_mb", "duration_seconds", "elapsed_seconds", "exit_code", "status"]
    if address: fields.append("ip_address")
    with (run / "measurements.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for scenario, elapsed in (("baseline-default", "4"), ("baseline-local", "3"), ("baseline-remote", "5"), ("awavma", "2")):
            row = {"schema_version":"4", "experiment_id":name, "timestamp_utc":"2026-01-01T00:00:00Z", "run_id":f"{name}-{scenario}", "scenario":scenario, "workload":"mixed", "repetition":"1", "thread_node":"0", "memory_node":"0", "runtime_enabled":"false", "threads":"2", "memory_mb":"4", "duration_seconds":"1", "elapsed_seconds":elapsed, "exit_code":"0", "status":"MEASURED"}
            if address: row["ip_address"] = "192.0.2.1"
            writer.writerow(row)

def main() -> int:
    with tempfile.TemporaryDirectory(prefix="awavma-phase4d-") as directory:
        raw = Path(directory) / "raw"; raw.mkdir(); write_run(raw, "good")
        unified = Path(directory) / "unified.csv"; comparator = Path(directory) / "comparator.csv"; summaries = Path(directory) / "summaries"
        result = subprocess.run([sys.executable, str(SCRIPT), "--input-dir", str(raw), "--output", str(unified), "--comparison-output", str(comparator), "--summary-dir", str(summaries)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        assert len(list(csv.DictReader(comparator.open(encoding="utf-8")))) == 1
        assert all((summaries / f"{metric}_summary.csv").is_file() for metric in ("elapsed_seconds", "throughput", "latency_ms"))
        write_run(raw, "unsafe", address=True)
        rejected = subprocess.run([sys.executable, str(SCRIPT), "--input-dir", str(raw), "--output", str(unified), "--comparison-output", str(comparator), "--summary-dir", str(summaries)], capture_output=True, text=True)
        assert rejected.returncode != 0 and "address" in rejected.stderr
    print("cloudlab_phase4d_aggregation_test: PASS")
    return 0

if __name__ == "__main__": raise SystemExit(main())
