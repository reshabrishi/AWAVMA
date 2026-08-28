#!/usr/bin/env python3
"""Run required final regressions and preserve their actual outcomes."""

from __future__ import annotations

import csv
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
COMMANDS = (
    ("make test-runtime", ["make", "test-runtime"]),
    ("make test-discovery-cadence", ["make", "test-discovery-cadence"]),
    ("make test-continuous-monitor", ["make", "test-continuous-monitor"]),
    ("make test-validation", ["make", "test-validation"]),
    ("make test-feedback", ["make", "test-feedback"]),
    ("make test-migration", ["make", "test-migration"]),
    ("make test-phase46-pipeline", ["make", "test-phase46-pipeline"]),
    ("make test-phase56-subprocess-profile", ["make", "test-phase56-subprocess-profile"]),
    ("python3 tests/final_integration_test.py", [sys.executable, "tests/final_integration_test.py"]),
    ("python3 tests/system_regression_test.py", [sys.executable, "tests/system_regression_test.py"]),
)


def run(command: list[str]) -> tuple[int, str]:
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    detail = (result.stdout + result.stderr).strip().replace("\n", " | ")
    return result.returncode, detail[-1000:]


def main() -> int:
    rows, failed = [], False
    for label, command in COMMANDS:
        code, detail = run(command)
        rows.append({"command": label, "result": "PASS" if code == 0 else "FAIL", "notes": detail})
        print(f"{label}: {'PASS' if code == 0 else 'FAIL'}")
        if label == "make test-continuous-monitor" and code != 0:
            retry_code, retry_detail = run(command)
            rows.append({"command": f"{label} (rerun)", "result": "PASS" if retry_code == 0 else "FAIL",
                         "notes": f"first failure: {detail[-300:]} | rerun: {retry_detail[-600:]}"})
            print(f"{label} rerun: {'PASS' if retry_code == 0 else 'FAIL'}")
            code = retry_code
        failed = failed or code != 0
    RESULTS.mkdir(parents=True, exist_ok=True)
    with (RESULTS / "final_integrated_regression.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=("command", "result", "notes"))
        writer.writeheader()
        writer.writerows(rows)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
