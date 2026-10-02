#!/usr/bin/env python3
"""Pure unit checks for production benefit-calibration semantics."""
import argparse
import csv
import hashlib
import importlib.util
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "run_benefit_calibration", ROOT / "scripts" / "run_benefit_calibration.py")
COLLECTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COLLECTOR)


def production_args(**overrides):
    values = {
        "workload": "mixed", "threads": 2, "memory_mb": 1024,
        "duration_seconds": 10.0, "runs": 5, "warmups": 2,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


def main():
    assert COLLECTOR.SCHEMA == 3
    assert COLLECTOR.is_production_methodology(production_args())
    for field, value in (("workload", "random"), ("threads", 1), ("memory_mb", 512),
                         ("duration_seconds", 5.0), ("runs", 4), ("warmups", 1)):
        assert not COLLECTOR.is_production_methodology(production_args(**{field: value}))

    local_node, remote_node = 2, 7
    source_node, target_node = COLLECTOR.supported_migration_route(local_node, remote_node)
    assert (source_node, target_node) == (remote_node, local_node)
    assert (source_node, target_node) != (local_node, remote_node)
    schedule = COLLECTOR.calibration_schedule(local_node, remote_node)
    assert [entry["logical_run_id"] for entry in schedule] == [
        "warmup-local-01", "warmup-local-02", "warmup-remote-01", "warmup-remote-02",
        "measured-local-01", "measured-remote-01", "measured-local-02", "measured-remote-02",
        "measured-local-03", "measured-remote-03", "measured-local-04", "measured-remote-04",
        "measured-local-05", "measured-remote-05"]
    assert [entry["sequence_index"] for entry in schedule] == list(range(1, 15))
    measured = [entry for entry in schedule if entry["role"] == "MEASURED"]
    assert len(measured) == 10
    assert [entry["condition"] for entry in measured] == ["LOCAL", "REMOTE"] * 5
    assert sum(entry["condition"] == "LOCAL" for entry in measured) == 5
    assert sum(entry["condition"] == "REMOTE" for entry in measured) == 5
    metric_rows = []
    for entry in schedule:
        is_measured = entry["role"] == "MEASURED"
        metric_rows.append({"scenario": entry["scenario"], "status": "MEASURED",
                            "throughput_ops_sec": "100" if entry["condition"] == "LOCAL" else "90",
                            "execution_time_sec": "10" if entry["condition"] == "LOCAL" else "11"})
        if not is_measured:
            metric_rows[-1]["throughput_ops_sec"] = "10000"
    local_t = COLLECTOR.metric_summary(metric_rows, "LOCAL", "throughput_ops_sec")["mean"]
    remote_t = COLLECTOR.metric_summary(metric_rows, "REMOTE", "throughput_ops_sec")["mean"]
    local_e = COLLECTOR.metric_summary(metric_rows, "LOCAL", "execution_time_sec")["mean"]
    remote_e = COLLECTOR.metric_summary(metric_rows, "REMOTE", "execution_time_sec")["mean"]
    assert (local_t, remote_t, local_e, remote_e) == (100.0, 90.0, 10.0, 11.0)
    assert abs((local_t - remote_t) / remote_t * 100.0 - 11.11111111111111) < 1e-12
    assert abs((remote_e - local_e) / remote_e * 100.0 - 9.090909090909092) < 1e-12
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        raw = directory / "raw"; raw.mkdir()
        rows = []
        for entry in schedule:
            entry = dict(entry)
            entry["native_evidence_path"] = f"raw/{entry['logical_run_id']}.csv"
            (directory / entry["native_evidence_path"]).write_text(entry["logical_run_id"] + "\n", encoding="ascii")
            rows.append(entry)
        with (directory / "calibration_runs.csv").open("w", newline="", encoding="ascii") as stream:
            writer = csv.DictWriter(stream, fieldnames=rows[0]); writer.writeheader(); writer.writerows(rows)
        args = production_args(); args.calibration_id = "fixture"
        manifest = COLLECTOR.canonical_evidence_manifest(args, [local_node, remote_node], local_node,
                                                          remote_node, "0123456789abcdef", "fixture-cpu", 2,
                                                          rows, directory)
        assert "methodology_version=1\n" in manifest and "schedule_version=1\n" in manifest
        assert manifest.count("run=") == 14
        assert hashlib.sha256(manifest.encode("ascii")).hexdigest() == hashlib.sha256(manifest.encode("ascii")).hexdigest()
        raw_digest = COLLECTOR.sha256_file(directory / rows[0]["native_evidence_path"])
        assert raw_digest in manifest
        (directory / rows[0]["native_evidence_path"]).write_text("tampered\n", encoding="ascii")
        assert COLLECTOR.sha256_file(directory / rows[0]["native_evidence_path"]) != raw_digest
    print("benefit_calibration_collector_test: PASS")


if __name__ == "__main__":
    main()
