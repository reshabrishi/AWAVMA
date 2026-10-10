#!/usr/bin/env python3
import csv, hashlib, json, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/report_beneficial_calibration.py"
FIELDS = ("collection_experiment_id", "calibration_status", "calibration_version", "topology_fingerprint", "expected_recoverable_gain_pct", "uncertainty_pct", "safety_margin_pct", "estimated_cost_pct", "marker")

def main() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary); source = root / "calibration.csv"; manifest = root / "manifest.json"; output = root / "report.csv"
        with source.open("w", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=FIELDS); writer.writeheader()
            writer.writerows([dict(zip(FIELDS, row)) for row in (
                ("run-1", "VALIDATED_PRODUCTION", "p4c-v2", "top1-fixture", "20", "1", "1", "2", "positive"),
                ("run-1", "VALIDATED_PRODUCTION", "p4c-v2", "top1-fixture", "4", "1", "1", "2", "zero"),
                ("run-1", "VALIDATED_PRODUCTION", "p4c-v2", "top1-fixture", "2", "1", "1", "2", "negative"))])
        base = {"schema_version":4, "kind":"AWAVMA_P4_CALIBRATION_AUTHORITY", "mode":"full",
                "status":"VALIDATED_PRODUCTION", "production_authority":True, "collection_experiment_id":"run-1",
                "numa_balancing_restore_status":"RESTORED", "timing_valid":True, "cost_migration_valid":True,
                "placement_validation_valid":True, "numa_balancing_transaction_valid":True,
                "strict_validation_valid":True, "overall_valid":True, "calibration_artifact_basename":source.name,
                "calibration_artifact_sha256":hashlib.sha256(source.read_bytes()).hexdigest(),
                "calibration_record_count":3, "calibration_version":"p4c-v2", "timing_metric":"throughput_ops_sec", "topology_fingerprints":["top1-fixture"],
                "raw_artifacts":[{"path":"raw_cost.csv","role":"COST","sha256":"0" * 64,"size_bytes":0}]}
        manifest.write_text(json.dumps(base))
        command = [sys.executable, str(TOOL), "--calibration", str(source), "--production-manifest", str(manifest), "--output", str(output)]
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        rows = list(csv.DictReader(output.open()))
        assert len(rows) == 3 and {row["marker"] for row in rows} == {"positive", "zero", "negative"}
        assert [row["benefit_status"] for row in rows] == ["BENEFICIAL", "NOT_BENEFICIAL", "NOT_BENEFICIAL"]
        assert [float(row["conservative_roi"]) for row in rows] == [4.0, 0.0, -0.5]
        assert all(row["effective_cost_pct"] == "4" for row in rows)
        bad = dict(base); bad["collection_experiment_id"] = "other"; manifest.write_text(json.dumps(bad))
        assert subprocess.run(command, capture_output=True).returncode != 0
        source.write_text("wrong,header\n1,2\n"); manifest.write_text(json.dumps(base))
        assert subprocess.run(command, capture_output=True).returncode != 0
    print("beneficial_calibration_report_test: PASS")
    return 0

if __name__ == "__main__": raise SystemExit(main())
