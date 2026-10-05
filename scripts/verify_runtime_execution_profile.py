#!/usr/bin/env python3
"""Validate runtime-owned Phase 7 startup state for experiment orchestration."""
from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

PROFILE_FIELDS = (
    "requested_execution_mode", "effective_execution_mode", "migration_safety_effective",
    "migration_execution_effective", "page_registration_effective", "page_registration_ttl_ms",
    "numa_nodes", "thread_migration_ready", "page_migration_ready", "phase7_ready",
    "production_real_migration_ready", "activation_state", "activation_reason",
    "page_registration_provider_ready",
)


def load_profile(path: Path) -> dict[str, str]:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    if len(rows) != 1 or any(not rows[0].get(field, "") for field in PROFILE_FIELDS):
        raise ValueError("runtime execution profile is missing required fields")
    return rows[0]


def verify_profile(row: dict[str, str]) -> None:
    required = {
        "requested_execution_mode": "PRODUCTION_REAL_MIGRATION",
        "effective_execution_mode": "PRODUCTION_REAL_MIGRATION",
        "migration_safety_effective": "true",
        "migration_execution_effective": "true",
        "page_registration_effective": "true",
        "page_registration_provider_ready": "true",
        "thread_migration_ready": "true",
        "page_migration_ready": "true",
        "phase7_ready": "true",
        "production_real_migration_ready": "true",
        "activation_state": "ACTIVE",
    }
    if row["activation_state"] == "ENV_LIMITED":
        raise RuntimeError(f"ENV_LIMITED: {row['activation_reason']}")
    mismatches = [f"{field}={row[field]}" for field, expected in required.items() if row[field] != expected]
    if mismatches:
        raise ValueError("runtime production profile is not active: " + " ".join(mismatches))
    try:
        if int(row["numa_nodes"]) < 2 or int(row["page_registration_ttl_ms"]) <= 0:
            raise ValueError("runtime production profile has invalid NUMA count or registration TTL")
    except ValueError as error:
        raise ValueError("runtime production profile has invalid numeric fields") from error


def verify_environment(path: Path) -> None:
    values = {}
    for token in path.read_text(encoding="utf-8").strip().split():
        key, separator, value = token.partition("=")
        if separator:
            values[key] = value
    if values.get("production_real_migration_ready") != "true":
        raise RuntimeError("ENV_LIMITED: production real-migration capability profile is unavailable")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--environment-check", type=Path)
    parser.add_argument("--emit-fields", action="store_true")
    args = parser.parse_args()
    try:
        if args.environment_check is not None:
            verify_environment(args.environment_check)
            return 0
        if args.profile is None:
            raise ValueError("--profile or --environment-check is required")
        row = load_profile(args.profile)
        verify_profile(row)
        if args.emit_fields:
            print(",".join(row[field] for field in PROFILE_FIELDS))
        return 0
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 3
    except (OSError, csv.Error, ValueError) as error:
        print(f"ACTIVATION_FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
