#!/usr/bin/env python3
"""Normalize only passing, run-scoped Phase 2-8 and runtime CSV artifacts."""
from __future__ import annotations
import argparse, csv, ipaddress, json, math, re, statistics
from collections import defaultdict
from pathlib import Path

WIDE_SCHEMA = ("schema_version","source_stage","record_type","experiment_id","run_id","timestamp_utc","scenario","workload","repetition","thread_node","memory_node","runtime_enabled","threads","memory_mb","duration_seconds","elapsed_seconds","throughput","latency_ms","cpu_utilization","memory_utilization","page_faults","migrations","migration_execution_ms","validation_score","feedback_reward","status","exit_code","data_source","source_file","manifest_file","collection_status","topology_source","comparison_key","comparable","provenance")
COMPARATOR_SCHEMA = WIDE_SCHEMA + ("metric","baseline_default_mean","baseline_local_mean","baseline_remote_mean","awavma_mean","awavma_vs_local_absolute","awavma_vs_local_percent","records_per_scenario")
SUMMARY_SCHEMA = ("schema_version","metric","comparison_key","scenario","records","mean","median","sample_stdev","data_source","provenance")
ADDRESS_FIELD = re.compile(r"(?:^|_)(?:ip|ipv[46]|address|host|hostname|mac)(?:$|_)", re.I)
MAC = re.compile(r"^(?:[0-9a-f]{2}:){5}[0-9a-f]{2}$", re.I)
SCENARIOS = ("baseline-default", "baseline-local", "baseline-remote", "awavma")

ADAPTERS = {
    "phase2": ("benchmark", {"timestamp_utc":("timestamp",),"workload":("pattern",),"threads":("threads",),"memory_mb":("memory_mb",),"duration_seconds":("duration_sec",),"thread_node":("thread_node",),"memory_node":("memory_node",),"elapsed_seconds":("execution_time_sec",),"throughput":("throughput_ops_sec",)}),
    "phase3": ("monitor", {"timestamp_utc":("timestamp",),"workload":("benchmark_pattern",),"threads":("benchmark_threads",),"memory_mb":("benchmark_memory_mb",),"duration_seconds":("benchmark_duration_sec",),"cpu_utilization":("process_cpu_utilization_percent",),"page_faults":("minor_page_faults",)}),
    "phase4": ("classifier", {"timestamp_utc":("timestamp",),"latency_ms":("elapsed_ms",),"status":("status",)}),
    "phase5": ("decision", {"timestamp_utc":("timestamp",),"run_id":("app_id",),"status":("status",)}),
    "phase6": ("validation", {"timestamp_utc":("timestamp",),"run_id":("migration_id",),"validation_score":("validation_score",),"status":("final_decision","validation_status")} ),
    "phase7": ("migration", {"timestamp_utc":("timestamp",),"run_id":("migration_id",),"migrations":("successful",),"migration_execution_ms":("execution_time_ms",),"status":("result",)}),
    "phase8": ("feedback", {"timestamp_utc":("timestamp",),"run_id":("feedback_id",),"feedback_reward":("effective_reward",),"status":("update_status","feedback_class")} ),
    "runtime": ("runtime", {"run_id":("app_id",),"status":("status",)}),
    "performance": ("measurement", {"timestamp_utc":("timestamp_utc",),"run_id":("run_id",),"scenario":("scenario",),"workload":("workload",),"repetition":("repetition",),"thread_node":("thread_node",),"memory_node":("memory_node",),"runtime_enabled":("runtime_enabled",),"threads":("threads",),"memory_mb":("memory_mb",),"duration_seconds":("duration_seconds",),"elapsed_seconds":("benchmark_execution_time_sec","elapsed_seconds"),"throughput":("throughput_ops_sec",),"exit_code":("exit_code",),"status":("status",)}),
}

def unsafe(value: str) -> bool:
    value = value.strip()
    if MAC.fullmatch(value): return True
    try: ipaddress.ip_address(value.strip("[]")); return True
    except ValueError: return False
def first(raw, names): return next((raw.get(name, "").strip() for name in names if raw.get(name, "").strip()), "")
def adapter(path: Path, fields: list[str]):
    name, joined = path.name.lower(), " ".join(fields).lower()
    for stage, token in (("performance","measurements"),("phase2","benchmark"),("phase3","monitor"),("phase4","classif"),("phase5","decision"),("phase6","validation"),("phase7","migration"),("phase8","feedback")):
        if token in name or token in joined: return stage, ADAPTERS[stage][1]
    return "runtime", ADAPTERS["runtime"][1]
def comparison_key(row): return "|".join(row[field] for field in ("workload","threads","memory_mb","duration_seconds"))
def finite(value):
    try:
        result=float(value)
        return result if math.isfinite(result) else None
    except (TypeError, ValueError): return None
def read_run(run: Path, root: Path):
    manifest_path=run / "manifest.json"
    manifest=json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("data_source") != "REAL" or manifest.get("collection_status") != "PASS": return []
    output=[]
    for path in sorted(run.rglob("*.csv")):
        # A run cannot ingest its own generated unified artifacts or another run.
        if path.parent == run.parent or path.name in {"cloudlab_experiment_results.csv","cloudlab_comparator.csv"}: continue
        with path.open(newline="", encoding="utf-8") as handle:
            reader=csv.DictReader(handle); fields=reader.fieldnames or []
            if any(ADDRESS_FIELD.search(field) for field in fields): raise ValueError(f"{path}: raw address field is forbidden")
            stage, mapping=adapter(path, fields)
            for line, raw in enumerate(reader, 2):
                if any(unsafe(str(value)) for value in raw.values()): raise ValueError(f"{path}:{line}: raw address is forbidden")
                row={field:"" for field in WIDE_SCHEMA}
                row.update({"schema_version":"4","source_stage":stage,"record_type":path.stem,"experiment_id":str(manifest.get("run_id","")),"data_source":"REAL","source_file":path.relative_to(root).as_posix(),"manifest_file":manifest_path.relative_to(root).as_posix(),"collection_status":"PASS","topology_source":str(manifest.get("topology_source", ""))})
                for field, names in mapping.items(): row[field]=first(raw, names)
                row["run_id"] = row["run_id"] or f"{manifest.get('run_id','run')}-{path.stem}-{line}"
                row["status"] = row["status"] or "OBSERVED"
                row["scenario"] = row["scenario"] or "observed"
                row["comparison_key"]=comparison_key(row)
                row["provenance"]=f"{row['manifest_file']}:{row['source_file']}:{line}"
                output.append(row)
    return output
def write_csv(path, fields, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer=csv.DictWriter(handle, fieldnames=fields); writer.writeheader(); writer.writerows(rows)
def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", type=Path, default=root / "results/cloudlab/raw")
    parser.add_argument("--output", type=Path, default=root / "results/cloudlab/unified/cloudlab_experiment_results.csv")
    parser.add_argument("--comparison-output", type=Path, default=root / "results/cloudlab/unified/cloudlab_comparator.csv")
    parser.add_argument("--summary-dir", type=Path, default=root / "results/cloudlab/unified/summaries")
    args=parser.parse_args()
    rows=[]
    for manifest in sorted(args.input_dir.glob("*/manifest.json")): rows.extend(read_run(manifest.parent, args.input_dir))
    if not rows: raise SystemExit("no passing real run-scoped CSV artifacts found")
    scenarios=defaultdict(set)
    for row in rows: scenarios[row["comparison_key"]].add(row["scenario"])
    for row in rows: row["comparable"]="true" if set(SCENARIOS) <= scenarios[row["comparison_key"]] else "false"
    write_csv(args.output, WIDE_SCHEMA, rows)
    comparator=[]
    summaries=defaultdict(list)
    for metric in ("elapsed_seconds","throughput","latency_ms"):
        for key in sorted(scenarios):
            values={scenario:[finite(row[metric]) for row in rows if row["comparison_key"] == key and row["scenario"] == scenario and finite(row[metric]) is not None] for scenario in SCENARIOS}
            if not all(values.values()): continue
            means={scenario:statistics.mean(values[scenario]) for scenario in SCENARIOS}
            local=means["baseline-local"]
            comparator.append({**{field:"" for field in COMPARATOR_SCHEMA},"schema_version":"4","metric":metric,"comparison_key":key,"baseline_default_mean":str(means["baseline-default"]),"baseline_local_mean":str(local),"baseline_remote_mean":str(means["baseline-remote"]),"awavma_mean":str(means["awavma"]),"awavma_vs_local_absolute":str(means["awavma"]-local),"awavma_vs_local_percent":str((means["awavma"]-local)/local*100) if local else "","records_per_scenario":str(len(values["awavma"])),"data_source":"REAL","collection_status":"PASS","comparable":"true","provenance":"aggregated"})
            for scenario, numbers in values.items(): summaries[metric].append({"schema_version":"4","metric":metric,"comparison_key":key,"scenario":scenario,"records":str(len(numbers)),"mean":str(statistics.mean(numbers)),"median":str(statistics.median(numbers)),"sample_stdev":str(statistics.stdev(numbers)) if len(numbers)>1 else "","data_source":"REAL","provenance":"aggregated"})
    write_csv(args.comparison_output, COMPARATOR_SCHEMA, comparator)
    for metric in ("elapsed_seconds","throughput","latency_ms"): write_csv(args.summary_dir / f"{metric}_summary.csv", SUMMARY_SCHEMA, summaries[metric])
if __name__ == "__main__": main()
