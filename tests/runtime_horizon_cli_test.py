#!/usr/bin/env python3
"""Exercise the runtime's explicit positive horizon option."""
import pathlib
import subprocess
import sys
import tempfile


def run(binary, value):
    with tempfile.TemporaryDirectory() as directory:
        return subprocess.run(
            [str(binary), "--duration-ms", "1", "--root-dir", directory,
             "--thread-evaluation-horizon-seconds", value],
            text=True, capture_output=True)


def main():
    binary = pathlib.Path("bin/awavma-runtime")
    if not binary.exists():
        print("runtime_horizon_cli_test: SKIP (build awavma-runtime first)")
        return 0
    try:
        valid = run(binary, "10")
    except OSError:
        print("runtime_horizon_cli_test: SKIP (runtime binary is not executable here)")
        return 0
    if "Error: invalid runtime option" in valid.stderr:
        raise SystemExit("valid horizon was rejected during option parsing")
    for value in ("0", "-1", "abc", "10junk", "nan", "inf"):
        result = run(binary, value)
        if result.returncode == 0 or "Error: invalid runtime option" not in result.stderr:
            raise SystemExit("invalid horizon was accepted: " + value)
    print("runtime_horizon_cli_test: PASS")


if __name__ == "__main__":
    main()
