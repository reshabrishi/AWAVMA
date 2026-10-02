#!/usr/bin/env python3
"""Behavioral Phase 4D aggregation validation coverage."""
from __future__ import annotations
import csv, json, math, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/aggregate_experiment_results.py"
FIELDS = ["schema_version", "experiment_id", "timestamp_utc", "run_id", "scenario", "workload", "repetition", "thread_node", "memory_node", "runtime_enabled", "threads", "memory_mb", "duration_seconds", "elapsed_seconds", "operations", "benchmark_execution_time_sec", "throughput_ops_sec", "exit_code", "status"]

def rows(name: str, scenarios=("baseline-default", "baseline-local", "baseline-remote", "awavma"), repetition="1"):
    placement = {"baseline-default": ("", ""), "baseline-local": ("0", "0"), "baseline-remote": ("1", "0"), "awavma": ("1", "0")}
    result = []
    for index, scenario in enumerate(scenarios, 1):
        thread, memory = placement[scenario]
        result.append({"schema_version":"4", "experiment_id":name, "timestamp_utc":"2026-01-01T00:00:00Z", "run_id":f"{name}-{scenario}-{repetition}", "scenario":scenario, "workload":"mixed", "repetition":repetition, "thread_node":thread, "memory_node":memory, "runtime_enabled":"true" if scenario == "awavma" else "false", "threads":"2", "memory_mb":"4", "duration_seconds":"1", "elapsed_seconds":str(index), "operations":"100", "benchmark_execution_time_sec":str(index), "throughput_ops_sec":str(100 // index), "exit_code":"0", "status":"MEASURED"})
    return result

def write_run(root: Path, name: str, *, status="PASS", scenario_rows=None, manifest=None, fields=FIELDS) -> Path:
    run = root / name; run.mkdir()
    document = {"run_id":name, "data_source":"REAL", "collection_status":status, "topology_source":"metadata/topology.txt", "local_node":0, "remote_node":1}
    if manifest: document.update(manifest)
    (run / "manifest.json").write_text(json.dumps(document), encoding="utf-8")
    with (run / "measurements.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore"); writer.writeheader(); writer.writerows(scenario_rows if scenario_rows is not None else rows(name))
    return run

def invoke(raw: Path, include=None):
    output = raw.parent / "unified.csv"; comparator = raw.parent / "comparator.csv"; summaries = raw.parent / "summaries"
    command = [sys.executable, str(SCRIPT), "--input-dir", str(raw), "--output", str(output), "--comparison-output", str(comparator), "--summary-dir", str(summaries)]
    if include: command.extend(["--include-in-progress-run", include])
    return subprocess.run(command, capture_output=True, text=True), output, comparator, summaries

def rejected(root: Path, name: str, **kwargs):
    raw = root / name; raw.mkdir(); write_run(raw, name, **kwargs)
    result, *_ = invoke(raw)
    assert result.returncode != 0, f"{name} unexpectedly accepted"

def main() -> int:
    with tempfile.TemporaryDirectory(prefix="awavma-phase4d-") as directory:
        root = Path(directory)
        # A1: valid PASS run; A22: complete comparable set; A23: finite summaries.
        raw = root / "valid"; raw.mkdir(); write_run(raw, "valid")
        result, output, comparator, summaries = invoke(raw)
        assert result.returncode == 0, result.stderr
        assert len(list(csv.DictReader(comparator.open(encoding="utf-8")))) == 2
        for path in summaries.glob("*.csv"):
            for row in csv.DictReader(path.open(encoding="utf-8")):
                for field in ("mean", "median", "sample_stdev"):
                    if row[field]: assert math.isfinite(float(row[field]))
        # A2-A4: only the exact named transactional run is included; FAILED is excluded.
        transactional = root / "transactional"; transactional.mkdir(); write_run(transactional, "active", status="IN_PROGRESS"); write_run(transactional, "other", status="IN_PROGRESS"); write_run(transactional, "failed", status="FAILED")
        excluded, *_ = invoke(transactional)
        assert excluded.returncode != 0
        included, current_output, *_ = invoke(transactional, "active")
        assert included.returncode == 0, included.stderr
        assert {row["experiment_id"] for row in csv.DictReader(current_output.open(encoding="utf-8"))} == {"active"}
        # A5 malformed JSON.
        malformed = root / "manifest"; malformed.mkdir(); (malformed / "manifest.json").write_text("{", encoding="utf-8")
        assert invoke(malformed)[0].returncode != 0
        # A6-A17: invalid measurement or topology evidence fails closed.
        invalid_cases = {
            "exit": lambda data: data[0].update(exit_code="1"),
            "failed-status": lambda data: data[0].update(status="FAILED"),
            "nan": lambda data: data[0].update(throughput_ops_sec="NaN"),
            "infinite": lambda data: data[0].update(benchmark_execution_time_sec="inf"),
            "operations": lambda data: data[0].update(operations="0"),
            "negative-operations": lambda data: data[0].update(operations="-1"),
            "experiment": lambda data: data[0].update(experiment_id="other"),
            "baseline-runtime": lambda data: data[0].update(runtime_enabled="true"),
            "awavma-runtime": lambda data: data[-1].update(runtime_enabled="false"),
            "timestamp": lambda data: data[0].update(timestamp_utc="not-a-time"),
            "topology": lambda data: data[1].update(memory_node="1"),
        }
        for name, mutate in invalid_cases.items():
            raw_case = root / name; raw_case.mkdir(); data = rows(name); mutate(data); write_run(raw_case, name, scenario_rows=data)
            assert invoke(raw_case)[0].returncode != 0, name
        missing = root / "missing"; missing.mkdir(); write_run(missing, "missing", fields=FIELDS[:-1])
        assert invoke(missing)[0].returncode != 0  # A11
        duplicate = root / "duplicate"; duplicate.mkdir(); data = rows("duplicate"); data.append(dict(data[0], run_id="duplicate-another", scenario="baseline-default")); write_run(duplicate, "duplicate", scenario_rows=data)
        assert invoke(duplicate)[0].returncode != 0  # A12
        # A18 privacy; A19 malformed optional phase artifact.
        privacy = root / "privacy"; privacy.mkdir(); write_run(privacy, "privacy", fields=FIELDS + ["ip_address"])
        assert invoke(privacy)[0].returncode != 0
        phase = root / "phase"; phase.mkdir(); run = write_run(phase, "phase"); (run / "classifier.csv").write_text("timestamp,status\nnot-a-time,\n", encoding="utf-8")
        assert invoke(phase)[0].returncode != 0
        # Unknown state/history-looking files are not evidence, even with misleading runtime columns.
        ignored = root / "ignored"; ignored.mkdir(); run = write_run(ignored, "ignored")
        (run / "weights.csv").write_text("app_id,status\nwrong,APPROVED\n", encoding="utf-8")
        assert invoke(ignored)[0].returncode == 0
        # A20-A21 valid partial collections aggregate without comparisons.
        for name, scenarios in (("baseline-only", ("baseline-default", "baseline-local", "baseline-remote")), ("awavma-only", ("awavma",))):
            partial = root / name; partial.mkdir(); write_run(partial, name, scenario_rows=rows(name, scenarios))
            partial_result, _, partial_comparator, _ = invoke(partial)
            assert partial_result.returncode == 0, partial_result.stderr
            assert not list(csv.DictReader(partial_comparator.open(encoding="utf-8")))
        mismatched = root / "mismatched"; mismatched.mkdir(); data = rows("mismatched"); data[-1]["repetition"] = "2"; data[-1]["run_id"] = "mismatched-awavma-2"; write_run(mismatched, "mismatched", scenario_rows=data)
        mismatch_result, _, mismatch_comparator, _ = invoke(mismatched)
        assert mismatch_result.returncode == 0, mismatch_result.stderr
        assert not list(csv.DictReader(mismatch_comparator.open(encoding="utf-8")))  # A22: incompatible repetitions are not comparable.
    print("cloudlab_phase4d_aggregation_test: PASS")
    return 0

if __name__ == "__main__": raise SystemExit(main())
