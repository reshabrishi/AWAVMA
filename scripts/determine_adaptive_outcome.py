#!/usr/bin/env python3
"""Derive a deterministic adaptive outcome from canonical runtime artifacts."""
from __future__ import annotations
import argparse, csv
from pathlib import Path

def rows(path):
    try:
        with path.open(newline="", encoding="utf-8") as handle: return list(csv.DictReader(handle))
    except OSError: return []

def main():
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument("--runtime-dir", type=Path, required=True); args=parser.parse_args()
    migrations=[]
    for path in sorted(args.runtime_dir.rglob("migration_results.csv")): migrations.extend(rows(path))
    thread=[row for row in migrations if row.get("action") == "MOVE_THREAD"]
    success=any(row.get("successful") == "true" or row.get("result") in {"MIGRATION_SUCCESS","MIGRATION_PARTIAL_SUCCESS"} for row in thread)
    failure=any(row.get("attempted") == "true" and (row.get("failed") == "true" or row.get("result","" ).startswith("MIGRATION_")) for row in thread)
    runtime=[]
    for row in rows(args.runtime_dir / "runtime_results.csv"): runtime.append(row)
    statuses={row.get("status", "") for row in runtime}
    if success: outcome="ADAPTIVE_THREAD_MIGRATION_SUCCEEDED"
    elif failure: outcome="ADAPTIVE_THREAD_MIGRATION_FAILED"
    elif statuses & {"REJECTED", "MIGRATION_METADATA_UNAVAILABLE"}: outcome="ADAPTIVE_DECISION_REJECTED"
    elif "INSUFFICIENT" in statuses: outcome="PIPELINE_EVIDENCE_UNAVAILABLE"
    else: outcome="EXPERIMENT_COMPLETED_NO_ADAPTIVE_ACTION"
    print(outcome)

if __name__ == "__main__": main()
