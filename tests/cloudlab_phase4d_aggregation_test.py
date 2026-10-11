#!/usr/bin/env python3
"""Focused explicit-run aggregation and completeness tests."""
from __future__ import annotations
import csv, hashlib, json, subprocess, sys, tempfile
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/aggregate_experiment_results.py"
FINGERPRINT = ROOT / "scripts/numa_topology_fingerprint.py"


def topology_text(free: str = "1000 MB", cpus: str = "0 1") -> str:
    return f"""available: 2 nodes (0-1)
node 0 cpus: {cpus}
node 0 size: 1024 MB
node 0 free: {free}
node 1 cpus: 2 3
node 1 size: 1024 MB
node 1 free: 1000 MB
node distances:
node   0   1
  0:  10  21
  1:  21  10
"""


def topology_hash(path: Path) -> str:
    return subprocess.check_output([sys.executable, str(FINGERPRINT), str(path)], text=True).strip()


def write_run(root: Path, name: str, scenarios: tuple[str, ...], status: str = "MEASURED", repetitions: int = 1,
              workload: str = "mixed", memory_mb: int = 4, duration: str = "1", free: str = "1000 MB") -> Path:
    run = root / name
    run.mkdir()
    topology = run / "metadata/numa_topology.txt"
    topology.parent.mkdir()
    topology.write_text(topology_text(free), encoding="utf-8")
    mode = "baseline" if "awavma" not in scenarios else "awavma" if len(scenarios) == 1 else "all"
    manifest = {"run_id":name, "data_source":"REAL", "collection_status":"PASS", "collection_mode":mode,
                "protocol_version":"phase4d-v1", "topology_fingerprint_version":"numactl-structural-v1",
                "topology_source":"metadata/numa_topology.txt", "topology_sha256":topology_hash(topology), "local_node":0,
                "remote_node":1, "numa_distance":"21", "numa_balancing_during":"0",
                "workload":workload, "threads":2, "memory_bytes":memory_mb * 1024 * 1024, "duration_seconds":duration,
                "calibration_sha256":"calibration" if "awavma" in scenarios else ""}
    (run / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    fields = "schema_version experiment_id run_id scenario workload repetition threads memory_mb duration_seconds registration_status benchmark_status runtime_status initial_placement_status final_placement_status exit_code status".split()
    with (run / "measurements.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields); writer.writeheader()
        for scenario in scenarios:
            for repetition in range(1, repetitions + 1):
                writer.writerow({"schema_version":"8", "experiment_id":name, "run_id":f"{name}-{scenario}-{repetition}", "scenario":scenario, "workload":workload, "repetition":str(repetition), "threads":"2", "memory_mb":str(memory_mb), "duration_seconds":duration, "registration_status":"ACCEPTED" if scenario == "awavma" else "NOT_REQUIRED", "benchmark_status":"PASS", "runtime_status":"PASS" if scenario == "awavma" else "NOT_REQUIRED", "initial_placement_status":"PASS", "final_placement_status":"PASS", "exit_code":"0", "status":status})
    with (run / "benchmark_results.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=("scenario", "repetition", "pattern", "threads", "memory_mb", "duration_sec", "operations", "execution_time_sec", "throughput_ops_sec")); writer.writeheader()
        for index, scenario in enumerate(scenarios, 1):
            for repetition in range(1, repetitions + 1):
                writer.writerow({"scenario":scenario, "repetition":str(repetition), "pattern":workload, "threads":"2", "memory_mb":str(memory_mb), "duration_sec":"30.0" if duration == "30" else duration, "operations":str(index*100), "execution_time_sec":str(repetition*2), "throughput_ops_sec":str(index*50/repetition)})
                if scenario == "awavma":
                    profile = run / f"awavma/runtime/{repetition}/runtime_execution_profile.csv"
                    profile.parent.mkdir(parents=True, exist_ok=True)
                    profile.write_text(f"controlled_workload_pattern,controlled_workload_threads,controlled_workload_memory_bytes,controlled_workload_duration_seconds\n{workload},2,{memory_mb * 1024 * 1024},{duration}\n", encoding="utf-8")
    return run


def rewrite_csv(path: Path, mutate) -> None:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle); rows = list(reader); fields = reader.fieldnames
    assert fields
    mutate(rows, fields)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields); writer.writeheader(); writer.writerows(rows)


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
        hot = write_run(root, "hot-real-schema", ("baseline-default", "baseline-local", "baseline-remote"),
                        workload="hot", memory_mb=1024, duration="30")
        assert invoke(hot, root / "hot-real-schema-output").returncode == 0
        workload_mismatch = write_run(root, "workload-config-mismatch", ("baseline-default", "baseline-local", "baseline-remote"),
                                       workload="hot", memory_mb=1024, duration="30")
        rewrite_csv(workload_mismatch / "benchmark_results.csv", lambda rows, fields: rows[0].update(pattern="mixed"))
        assert invoke(workload_mismatch, root / "workload-config-mismatch-output").returncode != 0
        threads_mismatch = write_run(root, "threads-config-mismatch", ("baseline-default", "baseline-local", "baseline-remote"),
                                      workload="hot", memory_mb=1024, duration="30")
        rewrite_csv(threads_mismatch / "benchmark_results.csv", lambda rows, fields: rows[0].update(threads="4"))
        assert invoke(threads_mismatch, root / "threads-config-mismatch-output").returncode != 0
        memory_mismatch = write_run(root, "memory-config-mismatch", ("baseline-default", "baseline-local", "baseline-remote"),
                                     workload="hot", memory_mb=1024, duration="30")
        data = json.loads((memory_mismatch / "manifest.json").read_text()); data["memory_bytes"] += 4096
        (memory_mismatch / "manifest.json").write_text(json.dumps(data))
        assert invoke(memory_mismatch, root / "memory-config-mismatch-output").returncode != 0
        duration_mismatch = write_run(root, "duration-config-mismatch", ("baseline-default", "baseline-local", "baseline-remote"),
                                       workload="hot", memory_mb=1024, duration="30")
        rewrite_csv(duration_mismatch / "benchmark_results.csv", lambda rows, fields: rows[0].update(duration_sec="31"))
        assert invoke(duration_mismatch, root / "duration-config-mismatch-output").returncode != 0
        missing_pattern = write_run(root, "missing-pattern", ("baseline-default", "baseline-local", "baseline-remote"))
        rewrite_csv(missing_pattern / "benchmark_results.csv", lambda rows, fields: (fields.remove("pattern"), [row.pop("pattern") for row in rows]))
        assert invoke(missing_pattern, root / "missing-pattern-output").returncode != 0
        missing_memory = write_run(root, "missing-memory", ("baseline-default", "baseline-local", "baseline-remote"))
        data = json.loads((missing_memory / "manifest.json").read_text()); data.pop("memory_bytes")
        (missing_memory / "manifest.json").write_text(json.dumps(data))
        assert invoke(missing_memory, root / "missing-memory-output").returncode != 0
        malformed = write_run(root, "malformed-config", ("baseline-default", "baseline-local", "baseline-remote"))
        rewrite_csv(malformed / "measurements.csv", lambda rows, fields: rows[0].update(memory_mb="not-a-number"))
        assert invoke(malformed, root / "malformed-config-output").returncode != 0
        t_run = write_run(root, "two-samples", ("baseline-default", "baseline-local", "baseline-remote", "awavma"), repetitions=2)
        assert invoke(t_run, root / "two-sample-output").returncode == 0
        t_rows = list(csv.DictReader((root / "two-sample-output/summaries/execution_time_seconds_summary.csv").open(encoding="utf-8")))
        t_row = next(row for row in t_rows if row["scenario"] == "baseline-default")
        assert math.isclose(float(t_row["ci95_low"]), 3.0 - 12.706, rel_tol=1e-9)
        reference = write_run(root, "reference", ("baseline-default", "baseline-local", "baseline-remote"), free="900 MB")
        # This retained reference predates structural fingerprints and records the old raw-file hash.
        data = json.loads((reference / "manifest.json").read_text())
        data.pop("topology_fingerprint_version")
        data["topology_sha256"] = hashlib.sha256((reference / "metadata/numa_topology.txt").read_bytes()).hexdigest()
        (reference / "manifest.json").write_text(json.dumps(data))
        assert invoke(reference, root / "baseline-output").returncode == 0
        assert list(csv.DictReader((root / "baseline-output/comparator.csv").open(encoding="utf-8"))) == []
        awavma = write_run(root, "awavma", ("awavma",))
        assert invoke(awavma, root / "missing-reference").returncode != 0
        assert invoke(awavma, root / "with-reference", reference).returncode == 0
        incompatible = write_run(root, "incompatible", ("baseline-default", "baseline-local", "baseline-remote"))
        data = json.loads((incompatible / "manifest.json").read_text()); data["numa_distance"] = "31"
        (incompatible / "manifest.json").write_text(json.dumps(data))
        assert invoke(awavma, root / "incompatible-output", incompatible).returncode != 0
        topology_changed = write_run(root, "topology-changed", ("baseline-default", "baseline-local", "baseline-remote"))
        path = topology_changed / "metadata/numa_topology.txt"
        path.write_text(topology_text(cpus="0 4"), encoding="utf-8")
        data = json.loads((topology_changed / "manifest.json").read_text())
        data["topology_sha256"] = topology_hash(path)
        (topology_changed / "manifest.json").write_text(json.dumps(data))
        assert invoke(awavma, root / "topology-changed-output", topology_changed).returncode != 0
        missing_topology = write_run(root, "missing-topology", ("baseline-default", "baseline-local", "baseline-remote"))
        (missing_topology / "metadata/numa_topology.txt").unlink()
        assert invoke(awavma, root / "missing-topology-output", missing_topology).returncode != 0
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
