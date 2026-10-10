#!/usr/bin/env python3
"""Focused explicit-run aggregation and completeness tests."""
from __future__ import annotations
import csv, json, subprocess, sys, tempfile
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/aggregate_experiment_results.py"


def write_run(root: Path, name: str, scenarios: tuple[str, ...], status: str = "MEASURED", repetitions: int = 1) -> Path:
    run = root / name
    run.mkdir()
    mode = "baseline" if "awavma" not in scenarios else "awavma" if len(scenarios) == 1 else "all"
    manifest = {"run_id":name, "data_source":"REAL", "collection_status":"PASS", "collection_mode":mode,
                "protocol_version":"phase4d-v1", "topology_sha256":"topology", "local_node":0,
                "remote_node":1, "numa_distance":"21", "numa_balancing_during":"0",
                "workload":"mixed", "threads":2, "memory_bytes":4194304, "duration_seconds":1,
                "calibration_sha256":"calibration" if "awavma" in scenarios else ""}
    (run / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    fields = "schema_version experiment_id run_id scenario workload repetition threads memory_mb duration_seconds registration_status benchmark_status runtime_status initial_placement_status final_placement_status exit_code status".split()
    with (run / "measurements.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields); writer.writeheader()
        for scenario in scenarios:
            for repetition in range(1, repetitions + 1):
                writer.writerow({"schema_version":"8", "experiment_id":name, "run_id":f"{name}-{scenario}-{repetition}", "scenario":scenario, "workload":"mixed", "repetition":str(repetition), "threads":"2", "memory_mb":"4", "duration_seconds":"1", "registration_status":"ACCEPTED" if scenario == "awavma" else "NOT_REQUIRED", "benchmark_status":"PASS", "runtime_status":"PASS" if scenario == "awavma" else "NOT_REQUIRED", "initial_placement_status":"PASS", "final_placement_status":"PASS", "exit_code":"0", "status":status})
    with (run / "benchmark_results.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=("scenario", "repetition", "pattern", "threads", "memory_mb", "duration_sec", "operations", "execution_time_sec", "throughput_ops_sec")); writer.writeheader()
        for index, scenario in enumerate(scenarios, 1):
            for repetition in range(1, repetitions + 1):
                writer.writerow({"scenario":scenario, "repetition":str(repetition), "pattern":"mixed", "threads":"2", "memory_mb":"4", "duration_sec":"1", "operations":str(index*100), "execution_time_sec":str(repetition*2), "throughput_ops_sec":str(index*50/repetition)})
                if scenario == "awavma":
                    profile = run / f"awavma/runtime/{repetition}/runtime_execution_profile.csv"
                    profile.parent.mkdir(parents=True, exist_ok=True)
                    profile.write_text("controlled_workload_pattern,controlled_workload_threads,controlled_workload_memory_bytes,controlled_workload_duration_seconds\nmixed,2,4194304,1\n", encoding="utf-8")
    return run


def invoke(run: Path, destination: Path, reference: Path | None = None):
    command = [sys.executable, str(SCRIPT), "--run-dir", str(run), "--output", str(destination / "unified.csv"), "--comparison-output", str(destination / "comparator.csv"), "--summary-dir", str(destination / "summaries")]
    if reference: command += ["--reference-run", str(reference)]
    return subprocess.run(command, capture_output=True, text=True)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="awavma-phase4d-") as directory:
        root = Path(directory); output = root / "output"
        full = write_run(root, "full", ("baseline-default", "baseline-local", "baseline-remote", "awavma"))
        result = invoke(full, output)
        assert result.returncode == 0, result.stderr
        comparisons = list(csv.DictReader((output / "comparator.csv").open(encoding="utf-8")))
        assert len(comparisons) == 12
        assert {row["comparison"] for row in comparisons} == {"primary_awavma_vs_remote", "secondary_awavma_vs_local", "secondary_awavma_vs_default"}
        summary = list(csv.DictReader((output / "summaries/execution_time_seconds_summary.csv").open(encoding="utf-8")))
        assert all(field in summary[0] for field in ("mean", "median", "sample_stdev", "minimum", "maximum", "ci95_low", "ci95_high"))
        t_run = write_run(root, "two-samples", ("baseline-default", "baseline-local", "baseline-remote", "awavma"), repetitions=2)
        assert invoke(t_run, root / "two-sample-output").returncode == 0
        t_rows = list(csv.DictReader((root / "two-sample-output/summaries/execution_time_seconds_summary.csv").open(encoding="utf-8")))
        t_row = next(row for row in t_rows if row["scenario"] == "baseline-default")
        assert math.isclose(float(t_row["ci95_low"]), 3.0 - 12.706, rel_tol=1e-9)
        reference = write_run(root, "reference", ("baseline-default", "baseline-local", "baseline-remote"))
        assert invoke(reference, root / "baseline-output").returncode == 0
        assert list(csv.DictReader((root / "baseline-output/comparator.csv").open(encoding="utf-8"))) == []
        awavma = write_run(root, "awavma", ("awavma",))
        assert invoke(awavma, root / "missing-reference").returncode != 0
        assert invoke(awavma, root / "with-reference", reference).returncode == 0
        incompatible = write_run(root, "incompatible", ("baseline-default", "baseline-local", "baseline-remote"))
        data = json.loads((incompatible / "manifest.json").read_text()); data["numa_distance"] = "31"
        (incompatible / "manifest.json").write_text(json.dumps(data))
        assert invoke(awavma, root / "incompatible-output", incompatible).returncode != 0
        failed = write_run(root, "failed", ("baseline-default", "baseline-local", "baseline-remote", "awavma"), "FAILED")
        assert invoke(failed, root / "failed-output").returncode != 0
        duplicate = write_run(root, "duplicate", ("baseline-default", "baseline-local", "baseline-remote", "awavma"))
        with (duplicate / "measurements.csv").open("a", encoding="utf-8") as handle:
            handle.write((duplicate / "measurements.csv").read_text(encoding="utf-8").splitlines()[1] + "\n")
        assert invoke(duplicate, root / "duplicate-output").returncode != 0
        addressed = write_run(root, "addressed", ("baseline-default", "baseline-local", "baseline-remote", "awavma"))
        text = (addressed / "benchmark_results.csv").read_text(encoding="utf-8")
        (addressed / "benchmark_results.csv").write_text(text.replace("scenario,repetition", "ip_address,scenario,repetition").replace("baseline-default,1", "192.0.2.1,baseline-default,1"), encoding="utf-8")
        assert invoke(addressed, root / "address-output").returncode != 0
        mismatch = write_run(root, "workload-mismatch", ("baseline-default", "baseline-local", "baseline-remote", "awavma"))
        profile = mismatch / "awavma/runtime/1/runtime_execution_profile.csv"
        profile.write_text(profile.read_text().replace("mixed,2", "random,2"))
        assert invoke(mismatch, root / "workload-mismatch-output").returncode != 0
    print("cloudlab_phase4d_aggregation_test: PASS")
    return 0


if __name__ == "__main__": raise SystemExit(main())
