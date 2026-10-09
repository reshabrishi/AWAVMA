#!/usr/bin/env python3
"""TEST-ONLY fixture coverage for P4-C planning/statistics/output safety."""
import csv, json, os, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RAW_HEADER = "run_id,pair_index,pair_order,warmup,action_kind,benchmark_pattern,placement_mode,threads,memory_bytes,page_size,local_node,remote_node,numa_distance,duration_seconds,elapsed_ms,verification_status,memory_policy_restored,total_pages,queryable_pages,other_pages,unknown_pages,placement_artifact,benchmark_exit_status,measurement_valid,invalid_reason".split(",")
COST_HEADER = "run_id,warmup,requested_pages,attempted_pages,migrated_pages,failed_pages,elapsed_ms,source_node,destination_node,distance,page_size,measurement_valid,failure_reason".split(",")
def write(path, header, rows):
    with open(path, "w", newline="") as handle: writer=csv.DictWriter(handle, fieldnames=header); writer.writeheader(); writer.writerows(rows)
def cost_row(warmup, elapsed="2", **changes):
    row = dict(zip(COST_HEADER, ["fixture",warmup,"4096","4096","4096","0",elapsed,"1","0","20","4096","true",""]))
    row.update(changes)
    return row
def cost_valid(path, mode):
    return subprocess.run([sys.executable, str(ROOT/"tools/validate_numa_calibration.py"), "cost-valid", str(path), mode], capture_output=True).returncode == 0
def raw_row(path, placement):
    command = [sys.executable, str(ROOT/"tools/validate_numa_calibration.py"), "raw-row", str(path), "fixture", "1", "mixed", placement, "2", "1048576", "30", "100", "0", "true", "", "false"]
    return next(csv.DictReader([",".join(RAW_HEADER), subprocess.run(command, check=True, capture_output=True, text=True).stdout]))
def main():
    with tempfile.TemporaryDirectory() as directory:
        d=Path(directory); raw=d/"raw.csv"; cost=d/"cost.csv"; out=d/"artifact.csv"; rows=[]
        for pair in range(1, 8):
            for placement, elapsed in (("local", 10 + pair / 100), ("remote", 20 + pair / 100)):
                evidence=d/f"{placement}-{pair}.csv"; evidence.write_text("schema_version,placement_mode,local_node,remote_node,requested_memory_node,numa_distance,total_pages,queryable_pages,expected_node_pages,local_pages,remote_pages,other_pages,unknown_pages,expected_node_ratio,observed_dominant_node,verification_status,verification_reason,memory_policy_restored\n1,%s,0,1,%s,20,256,256,256,256,0,0,0,1,0,PASS,verified,true\n" % (placement, 0 if placement == "local" else 1))
                rows.append(dict(zip(RAW_HEADER, ["fixture",str(pair),"LOCAL_REMOTE","false","MOVE_MEMORY","mixed",placement,"2","1048576","4096","0","1","20","30",str(elapsed),"PASS","true","256","256","0","0",str(evidence),"0","true",""])))
        # Retained invalid evidence must not alter accepted statistics.
        rows.append(dict(zip(RAW_HEADER, ["fixture","99","LOCAL_REMOTE","false","MOVE_MEMORY","mixed","remote","2","1048576","4096","0","1","20","30","1","FAIL","false","256","0","0","256","missing","1","false","PLACEMENT_FAILED"])))
        write(raw, RAW_HEADER, rows)
        write(cost, COST_HEADER, [cost_row("false", str(2 + i / 100)) for i in range(7)])
        command=[sys.executable, str(ROOT/"tools/build_numa_calibration.py"), "--raw",str(raw),"--cost",str(cost),"--output",str(out),"--experiment-id","fixture","--created-at-utc","2026-10-05T00:00:00Z","--cpu-architecture","x86_64","--cpu-model","fixture","--online-numa-nodes","2","--local-permitted-cpu-count","4","--production"]
        subprocess.run(command, check=True); subprocess.run([str(ROOT/"bin/calibration-validate"),str(out)], check=True)
        assert "0x" not in out.read_text().lower()
        # The builder accepts the controlled matrix it receives; it does not
        # require the legacy placement-oriented "local" benchmark pattern.
        assert {row["benchmark_pattern"] for row in csv.DictReader(out.open())} == {"mixed"}
        local_raw = raw_row(d/"local-1.csv", "local")
        remote_raw = raw_row(d/"remote-1.csv", "remote")
        assert (local_raw["local_node"], local_raw["remote_node"], local_raw["numa_distance"]) == ("0", "1", "20")
        assert (remote_raw["local_node"], remote_raw["remote_node"], remote_raw["numa_distance"]) == ("0", "1", "20")
        assert local_raw["placement_mode"] == "local" and remote_raw["placement_mode"] == "remote"
        assert not (local_raw["local_node"] == local_raw["remote_node"] and local_raw["numa_distance"] != "0")
        impossible = d/"impossible.csv"
        impossible.write_text((d/"local-1.csv").read_text().replace("1,local,0,1,0,20", "1,local,0,0,0,21"))
        assert subprocess.run([sys.executable, str(ROOT/"tools/validate_numa_calibration.py"), "placement", str(impossible), "local"], capture_output=True).returncode != 0
        # Warmup, invalid, and partial rows are retained but cannot satisfy seven measured samples.
        six = [cost_row("false", str(2 + i / 100)) for i in range(6)]
        six.extend([cost_row("true"), cost_row("false", migrated_pages="4095", failed_pages="1"),
                    cost_row("false", measurement_valid="false", failure_reason="INVALID"),
                    cost_row("false", "0", failure_reason="ZERO_TIME")])
        write(d/"six-cost.csv", COST_HEADER, six)
        assert subprocess.run(command[:command.index("--cost") + 1] + [str(d/"six-cost.csv")] + command[command.index("--output"):], capture_output=True).returncode != 0
        bad=rows[:2]; write(d/"bad.csv", RAW_HEADER, bad)
        assert subprocess.run(command[:command.index("--raw") + 1] + [str(d/"bad.csv")] + command[command.index("--cost"):], capture_output=True).returncode != 0
        # Manifest cost validity is mode-specific and only examines retained cost evidence.
        smoke = d/"smoke-cost.csv"; write(smoke, COST_HEADER, [cost_row("true")]); assert cost_valid(smoke, "smoke")
        write(smoke, COST_HEADER, [cost_row("true", "0")]); assert not cost_valid(smoke, "smoke")
        full_rows = [cost_row("true"), cost_row("true", "2.1")] + [cost_row("false", str(3 + i / 100)) for i in range(7)]
        full = d/"full-cost.csv"; write(full, COST_HEADER, full_rows); assert cost_valid(full, "full")
        assert not cost_valid(d/"six-cost.csv", "full")
        check_dir = d/"full-check"; check_dir.mkdir()
        (check_dir/"manifest.json").write_text(json.dumps({"mode":"full", "status":"NOT_PRODUCTION_CALIBRATION", "cost_migration_valid":True}))
        check_rows = []
        for index in range(126):
            check_rows.append(dict(zip(RAW_HEADER, ["fixture",str(index + 1),"LOCAL_REMOTE","true" if index < 28 else "false","MOVE_MEMORY","mixed","local" if index % 2 == 0 else "remote","2","1048576","4096","0","1","20","30","10","PASS","true","256","256","0","0","fixture.csv","0","true",""])))
        write(check_dir/"raw_timing.csv", RAW_HEADER, check_rows)
        write(check_dir/"raw_cost.csv", COST_HEADER, full_rows)
        checker = [sys.executable, str(ROOT/"tools/check_cloudlab_p4_results.py"), str(check_dir)]
        assert subprocess.run(checker, capture_output=True).returncode == 0
        check_rows[0]["remote_node"] = "0"; write(check_dir/"raw_timing.csv", RAW_HEADER, check_rows)
        assert subprocess.run(checker, capture_output=True).returncode != 0
        for changes in ({"measurement_valid":"false"}, {"elapsed_ms":"0"}, {"requested_pages":"4095"},
                        {"migrated_pages":"4095"}, {"failed_pages":"1"}, {"destination_node":"2"}):
            invalid = [dict(row) for row in full_rows]; invalid[-1].update(changes)
            write(full, COST_HEADER, invalid); assert not cost_valid(full, "full")
        collector = (ROOT/"tools/collect_numa_calibration.sh").read_text()
        assert '"$COST_VALID"' in collector
        assert '"thread_calibration":"NOT_IMPLEMENTED"' in collector
        assert '"awavma_remote_equivalence":"PENDING"' in collector
if __name__ == "__main__": main()
