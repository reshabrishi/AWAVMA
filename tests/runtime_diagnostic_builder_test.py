#!/usr/bin/env python3
"""Runtime diagnostic CSV preserves unavailable fields and runtime rejection reasons."""
from __future__ import annotations
import csv, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def write(path, text): path.parent.mkdir(parents=True, exist_ok=True); path.write_text(text, encoding="utf-8")

def main():
    with tempfile.TemporaryDirectory(prefix="awavma-runtime-diagnostic-") as temporary:
        root = Path(temporary) / "runtime" / "1"; cycle = root / "apps/app/cycles/7"; other = root / "apps/app/cycles/8"
        write(root / "runtime_results.csv", "app_id,pid,start_time_ticks,generation,phase3_samples,phase5_committed_rows,status,detail\napp,42,99,1,1,1,REJECTED,LOW_CONFIDENCE\n")
        write(cycle / "runtime_evidence.csv", "timestamp,elapsed_ms,pid,process_cpu_utilization_percent,interval_minor_faults,interval_major_faults,cache_references,cache_misses,process_numa_pages,app_id,entity_id,access_value,access_status\n2026-10-02T00:00:00Z,0,42,80,2,1,10,2,N0=2 N1=8,app,process,55,MEASURED_CPU_FAULT_CACHE\n")
        write(cycle / "decision_evidence.csv", "timestamp,pid,entity_id,classification,classification_score,memory_dominant_node,thread_dominant_node,placement_relation,f_access,f_threshold,f_gain_memory,f_cost_memory,f_cpu_memory,f_sharing_memory,f_gain_thread,f_cost_thread,f_cpu_thread,f_sharing_thread\n2026-10-02T00:00:01Z,42,process,HOT,90,1,0,REMOTE,0.4,0.5,NA,NA,NA,NA,0.7,0,0.3,0\n")
        write(cycle / "decision.csv", "timestamp,pid,entity_id,memory_score_final,thread_score_final,decision_margin,decision\n2026-10-02T00:00:02Z,42,process,NA,1.2,NA,MOVE_THREAD\n")
        write(cycle / "validation.csv", "timestamp,pid,entity_id,action,confidence_status,roi_status,safety_status,source_node,destination_node\n2026-10-02T00:00:03Z,42,process,MOVE_THREAD,PASS,NOT_APPLICABLE,PASS,0,1\n")
        write(other / "runtime_evidence.csv", "timestamp,elapsed_ms,pid,process_cpu_utilization_percent,interval_minor_faults,interval_major_faults,cache_references,cache_misses,process_numa_pages,app_id,entity_id,access_value,access_status\n2026-10-02T00:01:00Z,0,42,10,0,0,10,1,N0=10,app,process,10,MEASURED_CPU_FAULT_CACHE\n")
        output = Path(temporary) / "diagnostic.csv"
        result = subprocess.run([sys.executable, str(ROOT / "scripts/build_runtime_diagnostics.py"), "--runtime-dir", str(root), "--output", str(output)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        row = next(csv.DictReader(output.open(newline="", encoding="utf-8")))
        assert row["timestamp"].endswith("Z") and row["start_time_ticks"] == "99"
        assert row["memory_dominant_node"] == "1" and row["memory_dominant_fraction"] == "0.800000000"
        assert row["access_sources"] == "CPU;INTERVAL_FAULTS;CACHE" and row["rejection_reason"] == "LOW_CONFIDENCE"
        assert row["f_gain_memory"] == "NA" and row["migration_authorized"] == "true"
        assert row["cycle_id"] == "7" and row["classification"] == "HOT" and row["f_access"] == "0.4"
        assert row["decision"] == "MOVE_THREAD" and row["confidence_gate"] == "PASS" and row["safety_gate"] == "PASS"
        rows = list(csv.DictReader(output.open(newline="", encoding="utf-8")))
        assert len(rows) == 2 and rows[1]["cycle_id"] == "8" and rows[1]["classification"] == "NA"
    print("runtime_diagnostic_builder_test: PASS")

if __name__ == "__main__": main()
