#!/usr/bin/env python3
"""Join runtime cycle artifacts into an auditable, measured-only diagnostic CSV."""
from __future__ import annotations
import argparse, csv
from pathlib import Path
import re

FIELDS = ("timestamp","app_id","pid","start_time_ticks","cycle_id","sample_index","process_cpu_utilization_percent","interval_minor_faults","interval_major_faults","cache_references","cache_misses","memory_dominant_node","memory_dominant_fraction","thread_current_node","remote_placement","access_value","access_status","access_sources","classification","classification_score","f_access","f_threshold","f_gain_memory","f_cost_memory","f_cpu_memory","f_sharing_memory","f_gain_thread","f_cost_thread","f_cpu_thread","f_sharing_thread","memory_score","thread_score","decision_margin","decision","confidence_gate","roi_gate","safety_gate","page_locked","memory_pinned","proposed_action","source_node","destination_node","migration_authorized","migration_result","rejection_reason")

def read(path: Path):
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            return list(csv.DictReader(handle))
    except OSError:
        return []

def value(row, name, default="NA"): return row.get(name, "").strip() or default
def match(rows, raw):
    """Stay inside one cycle; match PID/entity before deterministic row order."""
    candidates = [row for row in rows if value(row, "pid") == value(raw, "pid")]
    entity = value(raw, "entity_id", "")
    if entity: candidates = [row for row in candidates if value(row, "entity_id", "") == entity] or candidates
    return candidates[0] if candidates else None
def residency(text):
    pairs = [(int(node), int(pages)) for node, pages in re.findall(r"N(\d+)=(\d+)", text or "")]
    if not pairs: return "NA", "NA"
    node, pages = max(pairs, key=lambda pair: pair[1]); total = sum(pair[1] for pair in pairs)
    return str(node), f"{pages / total:.9f}" if total else "NA"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = []
    starts, runtime_status = {}, {}
    for result in (args.runtime_dir / "runtime_results.csv",):
        for raw in read(result):
            starts[value(raw, "pid")] = value(raw, "start_time_ticks")
            runtime_status[value(raw, "pid")] = (value(raw, "status"), value(raw, "detail"))
    for evidence in sorted(args.runtime_dir.glob("apps/*/cycles/*/runtime_evidence.csv")):
        cycle = evidence.parent
        cycle_rows = []
        for raw in read(evidence):
            row = {field: "NA" for field in FIELDS}
            memory_node, memory_fraction = residency(value(raw, "process_numa_pages"))
            row.update({"timestamp":value(raw,"timestamp"), "app_id":value(raw,"app_id"), "pid":value(raw,"pid"), "start_time_ticks":starts.get(value(raw,"pid"), "NA"), "cycle_id":cycle.name,
                        "process_cpu_utilization_percent":value(raw,"process_cpu_utilization_percent"), "interval_minor_faults":value(raw,"interval_minor_faults"),
                        "interval_major_faults":value(raw,"interval_major_faults"), "cache_references":value(raw,"cache_references"), "cache_misses":value(raw,"cache_misses"),
                        "memory_dominant_node":memory_node, "memory_dominant_fraction":memory_fraction, "access_value":value(raw,"access_value"), "access_status":value(raw,"access_status"),
                        "access_sources":"CPU;INTERVAL_FAULTS" + (";CACHE" if value(raw,"access_status").endswith("CACHE") else "")})
            row["migration_result"], row["rejection_reason"] = runtime_status.get(row["pid"], ("NA", "NA"))
            row["sample_index"] = str(sum(1 for previous in rows if previous["app_id"] == row["app_id"] and previous["pid"] == row["pid"]))
            cycle_rows.append(row); rows.append(row)
        for path, mapping in (("decision_evidence.csv", {"classification":"classification","classification_score":"classification_score","memory_dominant_node":"memory_dominant_node","thread_current_node":"thread_dominant_node","remote_placement":"placement_relation","f_access":"f_access","f_threshold":"f_threshold","f_gain_memory":"f_gain_memory","f_cost_memory":"f_cost_memory","f_cpu_memory":"f_cpu_memory","f_sharing_memory":"f_sharing_memory","f_gain_thread":"f_gain_thread","f_cost_thread":"f_cost_thread","f_cpu_thread":"f_cpu_thread","f_sharing_thread":"f_sharing_thread"}), ("decision.csv", {"memory_score":"memory_score_final","thread_score":"thread_score_final","decision_margin":"decision_margin","decision":"decision","proposed_action":"decision"}), ("validation.csv", {"confidence_gate":"confidence_status","roi_gate":"roi_status","safety_gate":"safety_status","page_locked":"page_locked","memory_pinned":"memory_pinned","proposed_action":"action","source_node":"source_node","destination_node":"destination_node"}), ("migration_results.csv", {"migration_result":"result","rejection_reason":"error_reason"})):
            for raw in read(cycle / path):
                row = match(cycle_rows, raw)
                if row is not None:
                    for destination, source in mapping.items(): row[destination] = value(raw, source)
    for row in rows:
        if row["remote_placement"] == "REMOTE": row["remote_placement"] = "true"
        elif row["remote_placement"] == "LOCAL": row["remote_placement"] = "false"
        if row["decision"] == "MOVE_THREAD" and row["confidence_gate"] == "PASS" and row["safety_gate"] == "PASS": row["migration_authorized"] = "true"
        elif row["decision"] != "NA": row["migration_authorized"] = "false"
    if not rows: raise SystemExit("NO_EVIDENCE: runtime_evidence.csv contains no usable rows")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS); writer.writeheader(); writer.writerows(rows)

if __name__ == "__main__": main()
