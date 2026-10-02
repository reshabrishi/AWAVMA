#!/usr/bin/env python3
"""Fail-closed CLI checks that stop before runtime initialization."""
from pathlib import Path
import subprocess
import sys
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
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(5)"])
        try:
            observation = run("--duration-ms", "1000", "--pid", str(child.pid),
                              "--root-dir", str(Path(directory) / "runtime"))
            assert observation.returncode == 0
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
    print("benefit_calibration_cli_test: PASS")


if __name__ == "__main__":
    main()
