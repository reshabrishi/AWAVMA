#!/usr/bin/env python3
"""DC10: discovery cadence CLI rejects invalid positive-integer values."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    binary = ROOT / "bin/awavma-runtime"
    failures = 0
    for value in ("0", "-1", "invalid", "999999999999999999999999999999999999"):
        result = subprocess.run([str(binary), "--discovery-interval-ms", value], cwd=ROOT,
                                capture_output=True, text=True)
        passed = result.returncode != 0
        print(f"DC10[{value}]: {'PASS' if passed else 'FAIL'}")
        failures += not passed
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
