#!/usr/bin/env python3
"""Acceptance tests for the experimental in-process Phase 4 adapter."""

from __future__ import annotations

import csv
import os
import signal
import subprocess
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "bin/profile-awavma-runtime"


def rows(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def stop(process: subprocess.Popen[str]) -> None:
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=5)


def runtime_command(root: Path, mode: str, pids: list[int]) -> list[str]:
    command = [str(RUNTIME), "--duration-ms", "1800", "--evaluation-ms", "500", "--monitor-ms", "50",
               "--discovery-interval-ms", "50", "--workers", "2", "--queue-capacity", "16",
               "--root-dir", str(root), "--phase4-mode", mode]
    for pid in pids:
        command.extend(("--pid", str(pid)))
    return command


def run_mode(root: Path, mode: str, count: int) -> tuple[int, list[dict[str, str]], list[dict[str, str]]]:
    targets = [subprocess.Popen(["sleep", "4"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(count)]
    environment = os.environ.copy()
    environment["AWAVMA_PROFILE_PATH"] = str(root / "profile.csv")
    runtime = subprocess.Popen(runtime_command(root, mode, [target.pid for target in targets]), cwd=ROOT,
                               env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        runtime.wait(timeout=15)
    finally:
        for target in targets:
            stop(target)
        stop(runtime)
    return runtime.returncode, rows(root / "runtime_results.csv"), rows(root / "profile.csv")


def report(identifier: str, passed: bool) -> int:
    print(f"{identifier}: {'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


def main() -> int:
    failures = 0
    with tempfile.TemporaryDirectory(prefix="awavma-p46-") as temporary:
        root = Path(temporary)
        cli = subprocess.run([str(ROOT / "bin/classifier"), "--help"], cwd=ROOT, capture_output=True, text=True)
        decision = subprocess.run([str(ROOT / "bin/decision"), "--help"], cwd=ROOT, capture_output=True, text=True)
        validation = subprocess.run([str(ROOT / "bin/validation"), "--help"], cwd=ROOT, capture_output=True, text=True)
        failures += report("P46-01", all(result.returncode == 0 for result in (cli, decision, validation)))

        subprocess_root, direct_root = root / "subprocess", root / "direct"
        subprocess_root.mkdir()
        direct_root.mkdir()
        subprocess_code, subprocess_records, _ = run_mode(subprocess_root, "subprocess", 1)
        direct_code, direct_records, direct_events = run_mode(direct_root, "in-process", 1)
        subprocess_statuses = [row.get("status") for row in subprocess_records]
        direct_statuses = [row.get("status") for row in direct_records]
        failures += report("P46-02", subprocess_code == 0 and direct_code == 0 and subprocess_statuses == direct_statuses)
        failures += report("P46-03", direct_statuses == ["INSUFFICIENT"])

        two_root = root / "two-app"
        two_root.mkdir()
        code, records, events = run_mode(two_root, "in-process", 2)
        app_ids = [row.get("app_id", "") for row in records]
        isolated = code == 0 and len(app_ids) == 2 and len(set(app_ids)) == 2
        for app_id in app_ids:
            isolated = isolated and (two_root / "apps" / app_id / "state").is_dir() and (two_root / "apps" / app_id / "history").is_dir()
        failures += report("P46-04", isolated)

        ordered = True
        for app_id in app_ids:
            generation_events = [row for row in events if row.get("application") == app_id and row.get("component") in ("phase4_direct_api_total", "phase5_execution", "phase6_execution")]
            starts = {row.get("component"): int(row.get("start_ns", "0")) for row in generation_events}
            ordered = ordered and all(component in starts for component in ("phase4_direct_api_total", "phase5_execution", "phase6_execution")) and starts["phase4_direct_api_total"] <= starts["phase5_execution"] <= starts["phase6_execution"]
        failures += report("P46-05", ordered)
        failures += report("P46-06", all(row.get("status") == "INSUFFICIENT" for row in records))
        print("P46-07: NOT APPLICABLE (pipeline concurrency not enabled)")
        print("P46-08: NOT APPLICABLE (Phase 5 remains a subprocess)")

        phase6_intervals = [(int(row["start_ns"]), int(row["end_ns"])) for row in events if row.get("component") == "phase6_execution"]
        phase6_intervals.sort()
        serialized = all(previous[1] <= current[0] for previous, current in zip(phase6_intervals, phase6_intervals[1:]))
        failures += report("P46-09", serialized)

        shutdown_root = root / "shutdown"
        shutdown_root.mkdir()
        target = subprocess.Popen(["sleep", "4"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        runtime = subprocess.Popen(runtime_command(shutdown_root, "in-process", [target.pid]), cwd=ROOT,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)
        runtime.send_signal(signal.SIGTERM)
        runtime.wait(timeout=10)
        stop(target)
        failures += report("P46-10", runtime.returncode == 0)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
