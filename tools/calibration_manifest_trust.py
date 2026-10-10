#!/usr/bin/env python3
"""Production trust verification for the P4 calibration artifact pair."""

import csv
import hashlib
import json
import os
from pathlib import Path

SCHEMA_VERSION = 4
COLLECTION_KIND = "AWAVMA_P4_CALIBRATION_COLLECTION"
AUTHORITY_KIND = "AWAVMA_P4_CALIBRATION_AUTHORITY"
CALIBRATION_VERSION = "p4c-v2"
TIMING_METRIC = "throughput_ops_sec"
VALIDATION_FIELDS = (
    "timing_valid",
    "cost_migration_valid",
    "placement_validation_valid",
    "numa_balancing_transaction_valid",
    "strict_validation_valid",
    "overall_valid",
)


class TrustError(ValueError):
    pass


def read_manifest(path):
    try:
        value = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise TrustError("CALIBRATION_MANIFEST_MALFORMED") from error
    if not isinstance(value, dict):
        raise TrustError("CALIBRATION_MANIFEST_MALFORMED")
    return value


def _regular_file(path: Path) -> None:
    if path.is_symlink() or not path.is_file():
        raise TrustError("CALIBRATION_RAW_ARTIFACT_NOT_REGULAR")


def raw_artifact(path: Path, role: str, root: Path) -> dict:
    """Create the canonical local-file manifest entry used by the builder."""
    _regular_file(path)
    try:
        relative = path.relative_to(root)
    except ValueError as error:
        raise TrustError("CALIBRATION_RAW_ARTIFACT_OUTSIDE_BUNDLE") from error
    return {"path": relative.as_posix(), "role": role,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "size_bytes": path.stat().st_size}


def verify_raw_artifacts(manifest, root: Path, required: dict[str, str]) -> None:
    """Verify the complete local evidence set consumed by a calibration build."""
    entries = manifest.get("raw_artifacts")
    if not isinstance(entries, list) or entries != sorted(entries, key=lambda item: item.get("path", "")):
        raise TrustError("CALIBRATION_RAW_ARTIFACTS_INVALID")
    seen = set()
    actual = {}
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"path", "role", "sha256", "size_bytes"}:
            raise TrustError("CALIBRATION_RAW_ARTIFACTS_INVALID")
        relative, role, digest, size = (entry.get(key) for key in ("path", "role", "sha256", "size_bytes"))
        if (not isinstance(relative, str) or not relative or Path(relative).is_absolute() or
                ".." in Path(relative).parts or relative in seen or not isinstance(role, str) or
                not isinstance(digest, str) or len(digest) != 64 or
                any(character not in "0123456789abcdef" for character in digest) or
                not isinstance(size, int) or size < 0):
            raise TrustError("CALIBRATION_RAW_ARTIFACTS_INVALID")
        seen.add(relative)
        path = root / relative
        _regular_file(path)
        content = path.read_bytes()
        if len(content) != size or hashlib.sha256(content).hexdigest() != digest:
            raise TrustError("CALIBRATION_RAW_ARTIFACT_HASH_MISMATCH")
        actual[relative] = role
    if any(actual.get(path) != role for path, role in required.items()):
        raise TrustError("CALIBRATION_RAW_ARTIFACT_SET_MISMATCH")


def validate_raw_artifact_metadata(manifest) -> None:
    entries = manifest.get("raw_artifacts")
    if not isinstance(entries, list) or not entries:
        raise TrustError("CALIBRATION_RAW_ARTIFACTS_INVALID")
    paths = []
    for entry in entries:
        if not isinstance(entry, dict) or set(entry) != {"path", "role", "sha256", "size_bytes"}:
            raise TrustError("CALIBRATION_RAW_ARTIFACTS_INVALID")
        path, digest, size = entry.get("path"), entry.get("sha256"), entry.get("size_bytes")
        if (not isinstance(path, str) or not path or Path(path).is_absolute() or ".." in Path(path).parts or
                not isinstance(digest, str) or len(digest) != 64 or
                any(character not in "0123456789abcdef" for character in digest) or
                not isinstance(size, int) or size < 0):
            raise TrustError("CALIBRATION_RAW_ARTIFACTS_INVALID")
        paths.append(path)
    if paths != sorted(paths) or len(set(paths)) != len(paths):
        raise TrustError("CALIBRATION_RAW_ARTIFACTS_INVALID")


def validate_collection_manifest(manifest, experiment_id):
    if manifest.get("schema_version") != SCHEMA_VERSION or manifest.get("kind") != COLLECTION_KIND:
        raise TrustError("CALIBRATION_COLLECTION_SCHEMA_INVALID")
    if manifest.get("collection_experiment_id") != experiment_id:
        raise TrustError("CALIBRATION_EXPERIMENT_MISMATCH")
    if manifest.get("mode") != "full" or manifest.get("production_authority") is not False:
        raise TrustError("CALIBRATION_COLLECTION_NOT_FULL_CANDIDATE")
    if manifest.get("status") != "PRODUCTION_AUTHORITY_CANDIDATE":
        raise TrustError("CALIBRATION_COLLECTION_STATUS_INVALID")
    if manifest.get("timing_metric") != TIMING_METRIC:
        raise TrustError("CALIBRATION_COLLECTION_METRIC_PROVENANCE_INVALID")
    if manifest.get("calibration_version") != CALIBRATION_VERSION:
        raise TrustError("CALIBRATION_COLLECTION_VERSION_MISMATCH")
    if any(manifest.get(field) is not True for field in VALIDATION_FIELDS):
        raise TrustError("CALIBRATION_COLLECTION_VALIDATION_FAILED")
    if manifest.get("numa_balancing_restore_status") != "RESTORED":
        raise TrustError("CALIBRATION_COLLECTION_RESTORE_FAILED")
    validate_raw_artifact_metadata(manifest)


def verify_production_pair(artifact_path, manifest_path):
    artifact = Path(artifact_path)
    manifest = read_manifest(manifest_path)
    if manifest.get("schema_version") != SCHEMA_VERSION or manifest.get("kind") != AUTHORITY_KIND:
        raise TrustError("CALIBRATION_AUTHORITY_SCHEMA_INVALID")
    if manifest.get("mode") != "full" or manifest.get("production_authority") is not True:
        raise TrustError("CALIBRATION_NOT_PRODUCTION_AUTHORITY")
    if manifest.get("status") != "VALIDATED_PRODUCTION":
        raise TrustError("CALIBRATION_AUTHORITY_STATUS_INVALID")
    if manifest.get("timing_metric") != TIMING_METRIC:
        raise TrustError("CALIBRATION_AUTHORITY_METRIC_PROVENANCE_INVALID")
    if any(manifest.get(field) is not True for field in VALIDATION_FIELDS):
        raise TrustError("CALIBRATION_AUTHORITY_VALIDATION_FAILED")
    if manifest.get("numa_balancing_restore_status") != "RESTORED":
        raise TrustError("CALIBRATION_AUTHORITY_RESTORE_FAILED")
    validate_raw_artifact_metadata(manifest)
    try:
        content = artifact.read_bytes()
        with artifact.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
    except (OSError, UnicodeError, csv.Error) as error:
        raise TrustError("CALIBRATION_ARTIFACT_MALFORMED") from error
    if not rows or manifest.get("calibration_artifact_basename") != artifact.name:
        raise TrustError("CALIBRATION_ARTIFACT_IDENTITY_MISMATCH")
    if manifest.get("calibration_artifact_sha256") != hashlib.sha256(content).hexdigest():
        raise TrustError("CALIBRATION_ARTIFACT_HASH_MISMATCH")
    if manifest.get("calibration_record_count") != len(rows):
        raise TrustError("CALIBRATION_RECORD_COUNT_MISMATCH")
    experiment = manifest.get("collection_experiment_id")
    topologies = sorted({row.get("topology_fingerprint", "") for row in rows})
    if (not experiment or any(row.get("collection_experiment_id") != experiment for row in rows) or
            any(row.get("calibration_status") != "VALIDATED_PRODUCTION" for row in rows) or
            any(row.get("calibration_version") != CALIBRATION_VERSION for row in rows)):
        raise TrustError("CALIBRATION_ARTIFACT_PROVENANCE_MISMATCH")
    if manifest.get("calibration_version") != CALIBRATION_VERSION:
        raise TrustError("CALIBRATION_VERSION_MISMATCH")
    if manifest.get("topology_fingerprints") != topologies or not all(topologies):
        raise TrustError("CALIBRATION_TOPOLOGY_IDENTITY_MISMATCH")
    return rows
