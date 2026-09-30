#!/usr/bin/env python3
"""Fail-closed CLI checks that stop before runtime initialization."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "bin" / "awavma-runtime"


def run(*arguments):
    return subprocess.run([str(RUNTIME), *arguments], cwd=ROOT, text=True, capture_output=True, check=False)


def main():
    missing = run("--migration-execution-enabled")
    assert missing.returncode != 0
    assert "validated benefit calibration" in missing.stderr
    with tempfile.TemporaryDirectory() as directory:
        artifact = Path(directory) / "invalid.csv"
        artifact.write_text("broken\n", encoding="utf-8")
        invalid = run("--migration-execution-enabled", "--benefit-calibration", str(artifact))
        assert invalid.returncode != 0
        assert "validated benefit calibration" in invalid.stderr
    with tempfile.TemporaryDirectory() as directory:
        observation = run("--duration-ms", "1", "--root-dir", str(Path(directory) / "runtime"))
        assert observation.returncode == 0
    print("benefit_calibration_cli_test: PASS")


if __name__ == "__main__":
    main()
