#!/usr/bin/env python3
"""Canonical migration-result outcome precedence coverage."""
from __future__ import annotations
import subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]; SCRIPT=ROOT/"scripts/determine_adaptive_outcome.py"
HEADER="timestamp,migration_id,app_id,pid,expected_start_time_ticks,tid,entity_id,action,phase5_decision,phase6_validation,source_node,destination_node,destination_cpu,requested,attempted,successful,failed,execution_time_ms,verification_status,result,error_reason\n"
def outcome(root): return subprocess.run([sys.executable,str(SCRIPT),"--runtime-dir",str(root)],capture_output=True,text=True,check=True).stdout.strip()
def write(path,text): path.parent.mkdir(parents=True,exist_ok=True); path.write_text(text,encoding="utf-8")
def migration(result, successful="false", failed="false"): return HEADER+f"t,id,app,42,1,42,e,MOVE_THREAD,x,x,0,1,1,true,true,{successful},{failed},1,PASS,{result},reason\n"
def main():
 with tempfile.TemporaryDirectory(prefix="awavma-outcome-") as temporary:
  root=Path(temporary)/"runtime/1"; nested=root/"apps/app/cycles/1/migration_results.csv"
  write(nested,migration("MIGRATION_SUCCESS","true")); write(root/"runtime_results.csv","app_id,pid,start_time_ticks,generation,phase3_samples,phase5_committed_rows,status,detail\napp,42,1,1,1,1,TARGET_GONE,exited\n")
  assert outcome(root)=="ADAPTIVE_THREAD_MIGRATION_SUCCEEDED"
  write(nested,migration("MIGRATION_FAILED","false","true")); assert outcome(root)=="ADAPTIVE_THREAD_MIGRATION_FAILED"
  nested.unlink(); write(root/"runtime_results.csv","app_id,pid,start_time_ticks,generation,phase3_samples,phase5_committed_rows,status,detail\napp,42,1,1,1,1,REJECTED,gate\n"); assert outcome(root)=="ADAPTIVE_DECISION_REJECTED"
  write(root/"runtime_results.csv","app_id,pid,start_time_ticks,generation,phase3_samples,phase5_committed_rows,status,detail\napp,42,1,1,1,1,MONITORING,done\n"); assert outcome(root)=="EXPERIMENT_COMPLETED_NO_ADAPTIVE_ACTION"
 print("adaptive_outcome_test: PASS")
if __name__=="__main__": main()
