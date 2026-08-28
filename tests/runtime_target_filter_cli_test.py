#!/usr/bin/env python3
"""CLI coverage for repeatable AWAVMA runtime target identities."""

from __future__ import annotations

import csv
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "bin/awavma-runtime"


def command(root: Path, *pids: int, duration_ms: int = 260) -> list[str]:
    args = [str(BINARY), "--duration-ms", str(duration_ms), "--evaluation-ms", "80",
            "--monitor-ms", "20", "--discovery-interval-ms", "20", "--workers", "1",
            "--queue-capacity", "4", "--root-dir", str(root), "--bin-dir", str(ROOT / "bin"),
            "--config", str(ROOT / "config/awavma.conf")]
    for pid in pids:
        args.extend(("--pid", str(pid)))
    return args


def rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def stop(process: subprocess.Popen[str]) -> None:
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        process.wait(timeout=3)


def wait_for_target(path: Path, pid: int, runtime: subprocess.Popen[str]) -> bool:
    deadline = time.monotonic() + 10

    while time.monotonic() < deadline and runtime.poll() is None:
        if path.is_file() and any(row["pid"] == str(pid) for row in rows(path)):
            return True
        time.sleep(0.020)
    return False


def main() -> int:
    failures = 0
    targets = [subprocess.Popen(["sleep", "5"]) for _ in range(3)]
    try:
        with tempfile.TemporaryDirectory(prefix="awavma-target-filter-") as temporary:
            output = Path(temporary) / "repeated"
            result = subprocess.run(command(output, targets[0].pid, targets[0].pid, targets[1].pid),
                                    cwd=ROOT, capture_output=True, text=True, timeout=20)
            selected = {str(targets[0].pid), str(targets[1].pid)}
            recorded = rows(output / "runtime_results.csv") if result.returncode == 0 else []
            passed = result.returncode == 0 and {row["pid"] for row in recorded} == selected and \
                     len(recorded) == len(selected) and str(targets[2].pid) not in {row["pid"] for row in recorded}
            print(f"RTFCLI01: {'PASS' if passed else 'FAIL'}")
            failures += not passed

            malformed_commands = [command(Path(temporary) / "malformed") + ["--pid", "not-a-pid"],
                                  command(Path(temporary) / "missing") + ["--pid"]]
            malformed_results = [subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=5)
                                 for args in malformed_commands]
            passed = all(result.returncode != 0 for result in malformed_results)
            print(f"RTFCLI02: {'PASS' if passed else 'FAIL'}")
            failures += not passed

            unavailable = subprocess.run(command(Path(temporary) / "unavailable") + ["--pid", "999999999"],
                                         cwd=ROOT, capture_output=True, text=True, timeout=5)
            passed = unavailable.returncode != 0
            print(f"RTFCLI03: {'PASS' if passed else 'FAIL'}")
            failures += not passed

            exiting_root = Path(temporary) / "exiting"
            runtime = subprocess.Popen(command(exiting_root, targets[2].pid, duration_ms=1500), cwd=ROOT,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            observed = wait_for_target(exiting_root / "runtime_results.csv", targets[2].pid, runtime)
            stop(targets[2])
            stdout, stderr = runtime.communicate(timeout=20)
            recorded = rows(exiting_root / "runtime_results.csv") if runtime.returncode == 0 else []
            passed = observed and runtime.returncode == 0 and len(recorded) == 1 and \
                     recorded[0]["pid"] == str(targets[2].pid) and recorded[0]["status"] == "TARGET_GONE"
            print(f"RTFCLI04: {'PASS' if passed else 'FAIL'}")
            if not passed:
                print(stdout, stderr, file=sys.stderr)
            failures += not passed
    finally:
        for target in targets:
            stop(target)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
