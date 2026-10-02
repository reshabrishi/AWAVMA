#!/usr/bin/env python3
"""Normalize validated, run-scoped Phase 2-8 and runtime CSV artifacts."""
from __future__ import annotations
import argparse, csv, ipaddress, json, math, re, statistics
from collections import defaultdict
from datetime import datetime
from pathlib import Path

WIDE_SCHEMA = ("schema_version","source_stage","record_type","experiment_id","run_id","timestamp_utc","scenario","workload","repetition","thread_node","memory_node","runtime_enabled","threads","memory_mb","duration_seconds","elapsed_seconds","throughput","latency_ms","cpu_utilization","memory_utilization","page_faults","migrations","migration_execution_ms","validation_score","feedback_reward","status","exit_code","data_source","source_file","manifest_file","collection_status","topology_source","comparison_key","comparable","provenance")
COMPARATOR_SCHEMA = WIDE_SCHEMA + ("metric","baseline_default_mean","baseline_local_mean","baseline_remote_mean","awavma_mean","awavma_vs_local_absolute","awavma_vs_local_percent","records_per_scenario")
SUMMARY_SCHEMA = ("schema_version","metric","comparison_key","scenario","records","mean","median","sample_stdev","data_source","provenance")
ADDRESS_FIELD = re.compile(r"(?:^|_)(?:ip|ipv[46]|address|host|hostname|mac)(?:$|_)", re.I)
MAC = re.compile(r"^(?:[0-9a-f]{2}:){5}[0-9a-f]{2}$", re.I)
SCENARIOS = ("baseline-default", "baseline-local", "baseline-remote", "awavma")
MEASUREMENT_SCHEMA = ("schema_version","experiment_id","timestamp_utc","run_id","scenario","workload","repetition","thread_node","memory_node","runtime_enabled","threads","memory_mb","duration_seconds","elapsed_seconds","operations","benchmark_execution_time_sec","throughput_ops_sec","exit_code","status")

ADAPTERS = {
    "phase2": ("benchmark", {"timestamp_utc":("timestamp",),"workload":("pattern",),"threads":("threads",),"memory_mb":("memory_mb",),"duration_seconds":("duration_sec",),"thread_node":("thread_node",),"memory_node":("memory_node",),"elapsed_seconds":("execution_time_sec",),"throughput":("throughput_ops_sec",)}),
    "phase3": ("monitor", {"timestamp_utc":("timestamp",),"workload":("benchmark_pattern",),"threads":("benchmark_threads",),"memory_mb":("benchmark_memory_mb",),"duration_seconds":("benchmark_duration_sec",),"cpu_utilization":("process_cpu_utilization_percent",),"page_faults":("minor_page_faults",)}),
    "phase4": ("classifier", {"timestamp_utc":("timestamp",),"latency_ms":("elapsed_ms",),"status":("status",)}),
    "phase5": ("decision", {"run_id":("app_id",),"status":("status",)}),
    "phase6": ("validation", {"run_id":("migration_id",),"validation_score":("validation_score",),"status":("final_decision","validation_status")}),
    "phase7": ("migration", {"run_id":("migration_id",),"migrations":("successful",),"migration_execution_ms":("execution_time_ms",),"status":("result",)}),
    "phase8": ("feedback", {"run_id":("feedback_id",),"feedback_reward":("effective_reward",),"status":("update_status","feedback_class")}),
    "runtime": ("runtime", {"run_id":("app_id",),"status":("status",)}),
    "evidence": ("evidence", {"run_id":("app_id","migration_id","feedback_id"),}),
    "performance": ("measurement", {"timestamp_utc":("timestamp_utc",),"run_id":("run_id",),"scenario":("scenario",),"workload":("workload",),"repetition":("repetition",),"thread_node":("thread_node",),"memory_node":("memory_node",),"runtime_enabled":("runtime_enabled",),"threads":("threads",),"memory_mb":("memory_mb",),"duration_seconds":("duration_seconds",),"elapsed_seconds":("benchmark_execution_time_sec",),"throughput":("throughput_ops_sec",),"exit_code":("exit_code",),"status":("status",)}),
}
RECOGNIZED = {
    "measurements.csv": "performance", "runtime_evidence.csv": "phase3",
    "runtime_diagnostics.csv": "evidence", "classification_full.csv": "phase4",
    "classifier.csv": "phase4", "decision.csv": "phase5", "validation.csv": "phase6",
    "migration_results.csv": "phase7", "adaptive_outcomes.csv": "evidence",
}

def fail(message: str) -> None: raise ValueError(message)
def unsafe(value: str) -> bool:
    value = value.strip()
    if MAC.fullmatch(value): return True
    try: ipaddress.ip_address(value.strip("[]")); return True
    except ValueError: return False
def first(raw, names):
    for name in names:
        value = raw.get(name, "").strip()
        if value and value != "NA":
            return value
    return ""
def adapter(path: Path, fields: list[str]):
    name = path.name.lower()
    if name not in RECOGNIZED: return None
    if name == "measurements.csv": return "performance", ADAPTERS["performance"][1]
    stage = RECOGNIZED[name]
    return stage, ADAPTERS[stage][1]

def parse_timestamp(value: str, context: str) -> None:
    for fmt in ("%Y-%m-%dT%H:%M:%SZ", "%Y-%m-%dT%H:%M:%S"):
        try:
            datetime.strptime(value, fmt)
            return
        except ValueError:
            pass
    fail(f"{context}: timestamp must be ISO-8601 seconds with optional trailing Z")
def integer(value: str, context: str, positive: bool = False) -> int:
    if not re.fullmatch(r"[0-9]+", value or ""): fail(f"{context}: integer is required")
    parsed = int(value)
    if positive and parsed <= 0: fail(f"{context}: positive integer is required")
    return parsed
def node(value: str, context: str) -> int:
    if not re.fullmatch(r"[0-9]+", value or ""): fail(f"{context}: NUMA node must be a non-negative integer")
    return int(value)
def number(value: str, context: str, positive: bool = False) -> float:
    try: parsed = float(value)
    except (TypeError, ValueError): fail(f"{context}: finite number is required")
    if not math.isfinite(parsed): fail(f"{context}: finite number is required")
    if positive and parsed <= 0: fail(f"{context}: positive finite number is required")
    return parsed
def comparison_key(row): return "|".join(row[field] for field in ("workload","threads","memory_mb","duration_seconds","topology_source"))

def validate_manifest(run: Path, include: str | None) -> tuple[dict, bool]:
    path = run / "manifest.json"
    try: manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error: fail(f"{path}: invalid manifest JSON: {error}")
    if not isinstance(manifest, dict): fail(f"{path}: manifest must be an object")
    run_id = str(manifest.get("run_id", "")).strip()
    if not run_id or run_id != run.name: fail(f"{path}: manifest run_id must match run directory")
    status = manifest.get("collection_status")
    included = status == "IN_PROGRESS" and run_id == include
    if status == "PASS": included = True
    if not included: return manifest, False
    if manifest.get("data_source") != "REAL": fail(f"{path}: accepted runs require data_source REAL")
    topology = str(manifest.get("topology_source", "")).strip()
    if not topology: fail(f"{path}: topology_source is required")
    local = node(str(manifest.get("local_node", "")), f"{path}: local_node")
    remote = node(str(manifest.get("remote_node", "")), f"{path}: remote_node")
    if local == remote: fail(f"{path}: local_node and remote_node must differ")
    return manifest, True

def validate_measurement(raw: dict[str, str], fields: list[str], manifest: dict, context: str) -> dict[str, str]:
    if tuple(fields) != MEASUREMENT_SCHEMA: fail(f"{context}: measurements.csv schema mismatch")
    if None in raw or any(value is None for value in raw.values()): fail(f"{context}: malformed measurement CSV row")
    for field in MEASUREMENT_SCHEMA:
        if not raw.get(field, "").strip() and field not in ("thread_node", "memory_node"): fail(f"{context}: missing {field}")
    if raw["schema_version"] != "4": fail(f"{context}: unsupported measurement schema_version")
    manifest_id = str(manifest["run_id"])
    if raw["experiment_id"].strip() != manifest_id: fail(f"{context}: experiment_id does not match manifest")
    if not raw["run_id"].strip() or not raw["run_id"].startswith(manifest_id + "-"): fail(f"{context}: run_id is not scoped to manifest")
    parse_timestamp(raw["timestamp_utc"], context)
    if raw["scenario"] not in SCENARIOS or not raw["workload"].strip(): fail(f"{context}: invalid scenario or workload")
    integer(raw["repetition"], context + ": repetition", True)
    integer(raw["threads"], context + ": threads", True); integer(raw["memory_mb"], context + ": memory_mb", True)
    number(raw["duration_seconds"], context + ": duration_seconds", True); number(raw["elapsed_seconds"], context + ": elapsed_seconds", True)
    integer(raw["operations"], context + ": operations", True); number(raw["benchmark_execution_time_sec"], context + ": benchmark_execution_time_sec", True); number(raw["throughput_ops_sec"], context + ": throughput_ops_sec", True)
    if raw["status"] != "MEASURED" or integer(raw["exit_code"], context + ": exit_code") != 0: fail(f"{context}: accepted measurements require MEASURED with exit_code 0")
    expected_runtime = raw["scenario"] == "awavma"
    if raw["runtime_enabled"] not in ("true", "false") or (raw["runtime_enabled"] == "true") != expected_runtime: fail(f"{context}: scenario/runtime_enabled mismatch")
    local, remote = str(manifest["local_node"]), str(manifest["remote_node"])
    expected_nodes = {"baseline-default": ("", ""), "baseline-local": (local, local), "baseline-remote": (remote, local), "awavma": (remote, local)}[raw["scenario"]]
    if (raw["thread_node"], raw["memory_node"]) != expected_nodes: fail(f"{context}: scenario NUMA placement contradicts manifest topology")
    for field in ("thread_node", "memory_node"):
        if raw[field]: node(raw[field], context + f": {field}")
    return raw

def validate_phase_row(raw: dict[str, str], fields: list[str], stage: str, mapping: dict, context: str) -> None:
    if not fields or len(fields) != len(set(fields)) or any(not field for field in fields): fail(f"{context}: malformed CSV header")
    if any(ADDRESS_FIELD.search(field) for field in fields): fail(f"{context}: raw address field is forbidden")
    for field, value in raw.items():
        if value is None: fail(f"{context}: too many CSV values")
        if unsafe(str(value)): fail(f"{context}: raw address is forbidden")
        if str(value).strip().lower() in {"nan", "+nan", "-nan", "inf", "+inf", "-inf", "infinity", "+infinity", "-infinity"}: fail(f"{context}: non-finite value")
    for target, sources in mapping.items():
        value = first(raw, sources)
        if value and target == "timestamp_utc": parse_timestamp(value, context)
        if value and target in {"threads", "memory_mb", "page_faults", "migrations"}: integer(value, context + f": {target}")
        if value and target in {"duration_seconds", "elapsed_seconds", "throughput", "latency_ms", "cpu_utilization", "memory_utilization", "migration_execution_ms", "validation_score", "feedback_reward"}: number(value, context + f": {target}")
    if stage in {"phase4", "runtime"} and not first(raw, mapping["status"]): fail(f"{context}: status is required")

def read_run(run: Path, root: Path, include_in_progress_run: str | None = None):
    manifest, accepted = validate_manifest(run, include_in_progress_run)
    if not accepted: return []
    output, measurement_ids, row_ids = [], set(), set()
    for path in sorted(run.rglob("*.csv")):
        if "finalized" in path.parts or path.name in {"cloudlab_experiment_results.csv", "cloudlab_comparator.csv"}: continue
        try:
            with path.open(newline="", encoding="utf-8") as handle:
                reader = csv.DictReader(handle); fields = reader.fieldnames or []
                adapted = adapter(path, fields)
                if adapted is None: continue
                stage, mapping = adapted
                if stage == "performance" and path.name != "measurements.csv": fail(f"{path}: unexpected measurement filename")
                for line, raw in enumerate(reader, 2):
                    context = f"{path}:{line}"
                    if stage == "performance":
                        raw = validate_measurement(raw, fields, manifest, context)
                        identity = (manifest["run_id"], raw["scenario"], raw["workload"], raw["repetition"])
                        if identity in measurement_ids or raw["run_id"] in row_ids: fail(f"{context}: duplicate measurement identity")
                        measurement_ids.add(identity); row_ids.add(raw["run_id"])
                    else: validate_phase_row(raw, fields, stage, mapping, context)
                    row = {field:"" for field in WIDE_SCHEMA}
                    row.update({"schema_version":"4","source_stage":stage,"record_type":path.stem,"experiment_id":str(manifest["run_id"]),"data_source":"REAL","source_file":path.relative_to(root).as_posix(),"manifest_file":(run / "manifest.json").relative_to(root).as_posix(),"collection_status":"PASS","topology_source":f"{manifest['topology_source']}|{manifest['local_node']}->{manifest['remote_node']}"})
                    for field, names in mapping.items(): row[field] = first(raw, names)
                    if stage != "performance":
                        row["run_id"] = row["run_id"] or f"{manifest['run_id']}-{path.stem}-{line}"
                        row["status"] = row["status"] or "OBSERVED"; row["scenario"] = row["scenario"] or "observed"
                    row["comparison_key"] = comparison_key(row); row["provenance"] = f"{row['manifest_file']}:{row['source_file']}:{line}"
                    output.append(row)
        except (OSError, csv.Error, UnicodeDecodeError) as error: fail(f"{path}: unreadable CSV: {error}")
    if not any(row["source_stage"] == "performance" for row in output): fail(f"{run}: measurements.csv is required")
    return output

def write_csv(path, fields, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields); writer.writeheader(); writer.writerows(rows)
def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", type=Path, default=root / "results/cloudlab/raw")
    parser.add_argument("--output", type=Path, default=root / "results/cloudlab/unified/cloudlab_experiment_results.csv")
    parser.add_argument("--comparison-output", type=Path, default=root / "results/cloudlab/unified/cloudlab_comparator.csv")
    parser.add_argument("--summary-dir", type=Path, default=root / "results/cloudlab/unified/summaries")
    parser.add_argument("--include-in-progress-run", help="Temporarily include one named transactional run")
    args = parser.parse_args(); rows = []
    for manifest in sorted(args.input_dir.glob("*/manifest.json")): rows.extend(read_run(manifest.parent, args.input_dir, args.include_in_progress_run))
    if not rows: raise SystemExit("no passing real run-scoped CSV artifacts found")
    measured = [row for row in rows if row["source_stage"] == "performance"]
    groups = defaultdict(lambda: defaultdict(list))
    for row in measured: groups[row["comparison_key"]][row["scenario"]].append(row)
    comparable = set()
    for key, scenarios in groups.items():
        repetitions = {scenario: {row["repetition"] for row in values} for scenario, values in scenarios.items()}
        if set(SCENARIOS) <= set(scenarios) and len({tuple(sorted(repetitions[scenario])) for scenario in SCENARIOS}) == 1: comparable.add(key)
    for row in rows: row["comparable"] = "true" if row["source_stage"] == "performance" and row["comparison_key"] in comparable else "false"
    write_csv(args.output, WIDE_SCHEMA, rows)
    comparator, summaries = [], defaultdict(list)
    for metric in ("elapsed_seconds","throughput"):
        for key in sorted(comparable):
            values = {scenario: [number(row[metric], f"{row['provenance']}: {metric}", True) for row in groups[key][scenario]] for scenario in SCENARIOS}
            means = {scenario: statistics.mean(values[scenario]) for scenario in SCENARIOS}; local = means["baseline-local"]
            comparator.append({**{field:"" for field in COMPARATOR_SCHEMA},"schema_version":"4","metric":metric,"comparison_key":key,"baseline_default_mean":str(means["baseline-default"]),"baseline_local_mean":str(local),"baseline_remote_mean":str(means["baseline-remote"]),"awavma_mean":str(means["awavma"]),"awavma_vs_local_absolute":str(means["awavma"]-local),"awavma_vs_local_percent":str((means["awavma"]-local)/local*100),"records_per_scenario":str(len(values["awavma"])),"data_source":"REAL","collection_status":"PASS","comparable":"true","provenance":"aggregated"})
            for scenario, values_for_scenario in values.items(): summaries[metric].append({"schema_version":"4","metric":metric,"comparison_key":key,"scenario":scenario,"records":str(len(values_for_scenario)),"mean":str(statistics.mean(values_for_scenario)),"median":str(statistics.median(values_for_scenario)),"sample_stdev":str(statistics.stdev(values_for_scenario)) if len(values_for_scenario)>1 else "","data_source":"REAL","provenance":"aggregated"})
    write_csv(args.comparison_output, COMPARATOR_SCHEMA, comparator)
    for metric in ("elapsed_seconds","throughput","latency_ms"): write_csv(args.summary_dir / f"{metric}_summary.csv", SUMMARY_SCHEMA, summaries[metric])
if __name__ == "__main__": main()
