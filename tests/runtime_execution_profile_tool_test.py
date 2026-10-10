#!/usr/bin/env python3
"""Validate strict parsing of runtime-owned execution-profile artifacts."""
from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "scripts/verify_runtime_execution_profile.py"
FIELDS = ("schema_version", "requested_execution_mode", "effective_execution_mode", "migration_safety_requested", "migration_safety_effective", "migration_execution_requested", "migration_execution_effective", "page_registration_requested", "page_registration_effective", "page_registration_ttl_ms", "numa_nodes", "thread_migration_ready", "page_migration_ready", "phase7_ready", "production_real_migration_ready", "activation_state", "activation_reason", "page_registration_provider_ready", "page_registration_socket", "worker_evidence_socket")


def write_profile(path: Path, **changes: str) -> None:
    row = {"schema_version":"2", "requested_execution_mode":"PRODUCTION_REAL_MIGRATION", "effective_execution_mode":"PRODUCTION_REAL_MIGRATION", "migration_safety_requested":"true", "migration_safety_effective":"true", "migration_execution_requested":"true", "migration_execution_effective":"true", "page_registration_requested":"true", "page_registration_effective":"true", "page_registration_ttl_ms":"30000", "numa_nodes":"2", "thread_migration_ready":"true", "page_migration_ready":"true", "phase7_ready":"true", "production_real_migration_ready":"true", "activation_state":"ACTIVE", "activation_reason":"active", "page_registration_provider_ready":"true", "page_registration_socket":"", "worker_evidence_socket":""}
    row.update(changes)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerow(row)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="awavma-profile-tool-") as directory:
        path = Path(directory) / "profile.csv"
        write_profile(path)
        assert subprocess.run([sys.executable, str(TOOL), "--profile", str(path)], capture_output=True).returncode == 0
        page_socket = "/tmp/awavma-test/page-registration.sock"
        worker_socket = "/tmp/awavma-test/worker-evidence.sock"
        write_profile(path, schema_version="3", page_registration_socket=page_socket,
                      worker_evidence_socket=worker_socket)
        emitted = subprocess.run([sys.executable, str(TOOL), "--profile", str(path),
                                  "--emit-field", "page_registration_socket"],
                                 check=True, capture_output=True, text=True)
        assert emitted.stdout.strip() == page_socket
        write_profile(path, schema_version="3", page_registration_socket="",
                      worker_evidence_socket=worker_socket)
        assert subprocess.run([sys.executable, str(TOOL), "--profile", str(path)], capture_output=True).returncode == 1
        path.write_text("broken\n", encoding="utf-8")
        assert subprocess.run([sys.executable, str(TOOL), "--profile", str(path)], capture_output=True).returncode == 1
        write_profile(path, activation_state="INCOMPATIBLE")
        assert subprocess.run([sys.executable, str(TOOL), "--profile", str(path)], capture_output=True).returncode == 1
        write_profile(path, activation_state="ENV_LIMITED", activation_reason="single numa")
        assert subprocess.run([sys.executable, str(TOOL), "--profile", str(path)], capture_output=True).returncode == 3
    print("runtime_execution_profile_tool_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
