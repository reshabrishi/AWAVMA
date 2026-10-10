#!/usr/bin/env python3
"""Fixture coverage for the P4 production calibration trust boundary."""

import csv
import hashlib
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from calibration_manifest_trust import TrustError, validate_collection_manifest, verify_production_pair


def rejected(artifact, manifest):
    try:
        verify_production_pair(artifact, manifest)
        return False
    except TrustError:
        return True


def main():
    historical = ROOT / "results/cloudlab_calibration/full/p4c-20261010T081928Z-3122"
    assert rejected(historical / "numa_calibration.csv", historical / "manifest.json")
    candidate = {"schema_version":4, "kind":"AWAVMA_P4_CALIBRATION_COLLECTION",
                 "collection_experiment_id":"experiment-1", "mode":"full",
                 "status":"PRODUCTION_AUTHORITY_CANDIDATE", "production_authority":False,
                 "numa_balancing_restore_status":"RESTORED", "timing_valid":True,
                 "cost_migration_valid":True, "placement_validation_valid":True,
                 "numa_balancing_transaction_valid":True, "strict_validation_valid":True,
                   "overall_valid":True,"calibration_version":"p4c-v2","timing_metric":"throughput_ops_sec",
                  "raw_artifacts":[{"path":"raw_cost.csv","role":"COST","sha256":"0" * 64,"size_bytes":0}]}
    validate_collection_manifest(candidate, "experiment-1")
    collection_cases = [("mode", "smoke"), ("status", "NOT_PRODUCTION_CALIBRATION"),
                        ("production_authority", True), ("numa_balancing_restore_status", "FAILED"),
                        ("collection_experiment_id", "other")]
    collection_cases.extend((field, False) for field in ("timing_valid", "cost_migration_valid",
        "placement_validation_valid", "numa_balancing_transaction_valid", "strict_validation_valid", "overall_valid"))
    for key, value in collection_cases:
        changed = dict(candidate); changed[key] = value
        try: validate_collection_manifest(changed, "experiment-1")
        except TrustError: pass
        else: raise AssertionError(f"collector contradiction accepted: {key}")
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory); artifact = root / "calibration.csv"; manifest_path = root / "manifest.json"
        fields = ["collection_experiment_id", "calibration_status", "calibration_version", "topology_fingerprint"]
        with artifact.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=fields); writer.writeheader()
            writer.writerow({"collection_experiment_id":"experiment-1", "calibration_status":"VALIDATED_PRODUCTION",
                              "calibration_version":"p4c-v2", "topology_fingerprint":"top1-0000000000000001"})
        base = {"schema_version":4, "kind":"AWAVMA_P4_CALIBRATION_AUTHORITY", "mode":"full",
                "status":"VALIDATED_PRODUCTION", "production_authority":True,
                "collection_experiment_id":"experiment-1", "numa_balancing_restore_status":"RESTORED",
                "timing_valid":True, "cost_migration_valid":True, "placement_validation_valid":True,
                "numa_balancing_transaction_valid":True, "strict_validation_valid":True, "overall_valid":True,
                "calibration_artifact_basename":artifact.name,
                "calibration_artifact_sha256":hashlib.sha256(artifact.read_bytes()).hexdigest(),
                 "calibration_record_count":1, "calibration_version":"p4c-v2", "timing_metric":"throughput_ops_sec",
                 "topology_fingerprints":["top1-0000000000000001"],
                 "raw_artifacts":[{"path":"raw_cost.csv","role":"COST","sha256":"0" * 64,"size_bytes":0}]}
        manifest_path.write_text(json.dumps(base), encoding="utf-8")
        assert len(verify_production_pair(artifact, manifest_path)) == 1
        cases = [
            ("schema_version", 2), ("kind", "wrong"), ("mode", "smoke"),
            ("status", "NOT_PRODUCTION_CALIBRATION"), ("production_authority", False),
            ("collection_experiment_id", "other"), ("numa_balancing_restore_status", "FAILED"),
            ("timing_valid", False), ("cost_migration_valid", False),
            ("placement_validation_valid", False), ("numa_balancing_transaction_valid", False),
            ("strict_validation_valid", False), ("overall_valid", False),
            ("calibration_artifact_basename", "other.csv"), ("calibration_artifact_sha256", "0" * 64),
            ("calibration_record_count", 2), ("calibration_version", "p4c-v1"), ("timing_metric", "elapsed_ms"),
            ("topology_fingerprints", ["top1-ffffffffffffffff"]),
        ]
        for key, value in cases:
            changed = dict(base); changed[key] = value; manifest_path.write_text(json.dumps(changed))
            assert rejected(artifact, manifest_path), key
        manifest_path.write_text("not-json"); assert rejected(artifact, manifest_path)
        manifest_path.write_text(json.dumps(base)); artifact.write_text(artifact.read_text().replace("experiment-1", "other"))
        changed = dict(base); changed["calibration_artifact_sha256"] = hashlib.sha256(artifact.read_bytes()).hexdigest()
        manifest_path.write_text(json.dumps(changed)); assert rejected(artifact, manifest_path)
    print("calibration_manifest_trust_test: PASS")


if __name__ == "__main__": main()
