#!/usr/bin/env python3
"""TEST-ONLY fixture coverage for P4-C planning/statistics/output safety."""
import csv, os, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RAW_HEADER = "run_id,pair_index,pair_order,warmup,action_kind,benchmark_pattern,placement_mode,threads,memory_bytes,page_size,local_node,remote_node,numa_distance,duration_seconds,elapsed_ms,verification_status,memory_policy_restored,total_pages,queryable_pages,other_pages,unknown_pages,placement_artifact,benchmark_exit_status,measurement_valid,invalid_reason".split(",")
COST_HEADER = "run_id,warmup,requested_pages,attempted_pages,migrated_pages,failed_pages,elapsed_ms,source_node,destination_node,distance,page_size,measurement_valid,failure_reason".split(",")
def write(path, header, rows):
    with open(path, "w", newline="") as handle: writer=csv.DictWriter(handle, fieldnames=header); writer.writeheader(); writer.writerows(rows)
def main():
    with tempfile.TemporaryDirectory() as directory:
        d=Path(directory); raw=d/"raw.csv"; cost=d/"cost.csv"; out=d/"artifact.csv"; rows=[]
        for pair in range(1, 8):
            for placement, elapsed in (("local", 10 + pair / 100), ("remote", 20 + pair / 100)):
                evidence=d/f"{placement}-{pair}.csv"; evidence.write_text("schema_version,placement_mode,local_node,requested_memory_node,numa_distance,total_pages,queryable_pages,expected_node_pages,local_pages,remote_pages,other_pages,unknown_pages,expected_node_ratio,observed_dominant_node,verification_status,verification_reason,memory_policy_restored\n1,%s,0,%s,20,256,256,256,256,0,0,0,1,0,PASS,verified,true\n" % (placement, 0 if placement == "local" else 1))
                rows.append(dict(zip(RAW_HEADER, ["fixture",str(pair),"LOCAL_REMOTE","false","MOVE_MEMORY","mixed",placement,"2","1048576","4096","0","1","20","30",str(elapsed),"PASS","true","256","256","0","0",str(evidence),"0","true",""])))
        # Retained invalid evidence must not alter accepted statistics.
        rows.append(dict(zip(RAW_HEADER, ["fixture","99","LOCAL_REMOTE","false","MOVE_MEMORY","mixed","remote","2","1048576","4096","0","1","20","30","1","FAIL","false","256","0","0","256","missing","1","false","PLACEMENT_FAILED"])))
        write(raw, RAW_HEADER, rows)
        write(cost, COST_HEADER, [dict(zip(COST_HEADER, ["fixture","false","4096","4096","4096","0",str(2 + i / 100),"1","0","20","4096","true",""])) for i in range(7)])
        command=[sys.executable, str(ROOT/"tools/build_numa_calibration.py"), "--raw",str(raw),"--cost",str(cost),"--output",str(out),"--experiment-id","fixture","--created-at-utc","2026-10-05T00:00:00Z","--cpu-architecture","x86_64","--cpu-model","fixture","--online-numa-nodes","2","--local-permitted-cpu-count","4","--production"]
        subprocess.run(command, check=True); subprocess.run([str(ROOT/"bin/calibration-validate"),str(out)], check=True)
        assert "0x" not in out.read_text().lower()
        bad=rows[:2]; write(d/"bad.csv", RAW_HEADER, bad)
        assert subprocess.run(command[:command.index("--raw") + 1] + [str(d/"bad.csv")] + command[command.index("--cost"):], capture_output=True).returncode != 0
if __name__ == "__main__": main()
