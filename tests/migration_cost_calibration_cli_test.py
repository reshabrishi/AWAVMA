#!/usr/bin/env python3
"""Focused CLI checks for the fixed migration-cost calibration route."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "run_migration_cost_calibration.py"


def run(*arguments):
    return subprocess.run([sys.executable, str(SCRIPT), *arguments], cwd=ROOT,
                          text=True, capture_output=True, check=False)


def main():
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "cost.csv"
        rejected = run("--output", str(output), "--source-node", "0", "--target-node", "1")
        assert rejected.returncode != 0
        assert "1 -> 0" in rejected.stderr

        if sys.platform == "win32":
            binary = Path(directory) / "calibration.cmd"
            binary.write_text("@exit /b 0\n", encoding="ascii")
        else:
            binary = Path(directory) / "calibration.py"
            binary.write_text(
                "#!/usr/bin/env python3\nimport sys\nassert sys.argv[1:3] == ['1', '0']\n",
                encoding="ascii")
            binary.chmod(0o700)
        accepted = run("--output", str(output), "--source-node", "1", "--target-node", "0",
                       "--binary", str(binary))
        assert accepted.returncode == 0, accepted.stderr
        assert accepted.stdout.strip() == str(output)
    print("migration_cost_calibration_cli_test: PASS")


if __name__ == "__main__":
    main()
