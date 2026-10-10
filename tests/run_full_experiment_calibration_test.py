#!/usr/bin/env python3
"""Executable calibration wiring checks with deterministic runner dependencies."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def executable(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def make_root(base: Path) -> tuple[Path, dict[str, str]]:
    root = base / "root"
    for name in ("scripts", "bin", "config", "mock-bin"):
        (root / name).mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "scripts/run_full_experiment.sh", root / "scripts/run_full_experiment.sh")
    (root / "config/awavma.conf").write_text("", encoding="utf-8")
    executable(root / "mock-bin/make", "#!/bin/sh\nexit 0\n")
    executable(root / "mock-bin/seq", "#!/bin/sh\nif [ \"$2\" = 5 ]; then printf '1\\n'; else /usr/bin/seq \"$@\"; fi\n")
    executable(root / "mock-bin/numactl", "#!/bin/sh\nprintf 'node 0 cpus: 0\\nnode 2 cpus: 2\\n'\n")
    executable(root / "bin/environment-check", "#!/bin/sh\nprintf 'ok\\n'\n")
    executable(
        root / "bin/calibration-validate",
        "#!/bin/sh\ngrep -q '^VALID-CALIBRATION$' \"$1\" || exit 1\nprintf 'CALIBRATION_VALIDATED records=1\\n'\n",
    )
    executable(
        root / "scripts/verify_runtime_execution_profile.py",
        "#!/usr/bin/env python3\nimport sys\nif '--emit-fields' in sys.argv:\n print('PRODUCTION_REAL_MIGRATION,PRODUCTION_REAL_MIGRATION,true,true,true,1000,2,true,true,true,true,ACTIVE,active')\n",
    )
    executable(root / "scripts/aggregate_experiment_results.py", "#!/usr/bin/env python3\n")
    executable(root / "scripts/generate_multinuma_graphs.py", "#!/usr/bin/env python3\n")
    executable(
        root / "bin/benchmark",
        """#!/usr/bin/env python3
import pathlib, sys
args = sys.argv[1:]
path = pathlib.Path(args[args.index('--placement-evidence') + 1])
mode = args[args.index('--placement-mode') + 1]
header = 'placement_mode,local_node,requested_memory_node,numa_distance,total_pages,queryable_pages,local_pages,remote_pages,other_pages,unknown_pages,expected_node_pages,expected_node_ratio,verification_status,memory_policy_restored\\n'
row = f'{mode},0,2,21,10,10,0,10,0,0,10,1.0,PASS,true\\n'
path.write_text(header + row)
if '/awavma/' in str(path):
    other = pathlib.Path(str(path).replace('/awavma/placement-awavma-', '/baseline/placement-baseline-remote-'))
    other.parent.mkdir(parents=True, exist_ok=True)
    other.write_text(header + row)
(path.parents[1] / 'benchmark-invocations.txt').open('a').write(' '.join(args) + '\\n')
""",
    )
    executable(
        root / "bin/awavma-runtime",
        """#!/usr/bin/env python3
import pathlib, signal, sys, time
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
args = sys.argv[1:]
runtime_root = pathlib.Path(args[args.index('--root-dir') + 1])
runtime_root.mkdir(parents=True)
(runtime_root / 'runtime_execution_profile.csv').write_text('profile\\n')
(runtime_root.parents[2] / 'runtime-args.txt').write_text('\\n'.join(args) + '\\n')
time.sleep(.5)
""",
    )
    env = os.environ.copy()
    env["PATH"] = f"{root / 'mock-bin'}:{env['PATH']}"
    return root, env


def run(root: Path, env: dict[str, str], *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(root / "scripts/run_full_experiment.sh"), *args],
        cwd=root,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def only_run(output: Path) -> Path:
    runs = list((output / "raw").iterdir())
    assert len(runs) == 1, runs
    return runs[0]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="awavma-runner-calibration-") as temporary:
        base = Path(temporary)

        root, env = make_root(base / "valid")
        supplied = base / "supplied calibration.csv"
        supplied.write_text("VALID-CALIBRATION\n", encoding="utf-8")
        output = base / "valid-output"
        result = run(root, env, "--awavma-only", "--skip-graphs", "--calibration", str(supplied), "--output-dir", str(output))
        assert result.returncode == 0, (result.stdout, result.stderr)
        run_dir = only_run(output)
        copied = run_dir / "metadata/calibration.csv"
        assert copied.read_bytes() == supplied.read_bytes()
        digest = hashlib.sha256(supplied.read_bytes()).hexdigest()
        assert (run_dir / "metadata/calibration.sha256").read_text().split()[0] == digest
        runtime_args = (run_dir / "runtime-args.txt").read_text().splitlines()
        assert runtime_args.count("--calibration-artifact") == 1
        assert runtime_args[runtime_args.index("--calibration-artifact") + 1] == str(supplied)
        assert {"--production-real-migration", "-S", "-M", "-R"} <= set(runtime_args)
        manifest = json.loads((run_dir / "manifest.json").read_text())
        assert manifest["calibration_artifact"] == "metadata/calibration.csv"
        assert manifest["calibration_sha256"] == digest
        assert str(supplied) not in (run_dir / "manifest.json").read_text()

        root, env = make_root(base / "invalid")
        invalid = base / "invalid.csv"
        invalid.write_text("INVALID\n", encoding="utf-8")
        output = base / "invalid-output"
        result = run(root, env, "--awavma-only", "--calibration", str(invalid), "--output-dir", str(output))
        assert result.returncode != 0
        run_dir = only_run(output)
        assert not (run_dir / "benchmark-invocations.txt").exists()
        assert not (run_dir / "runtime-args.txt").exists()

        root, env = make_root(base / "missing")
        result = run(root, env, "--awavma-only", "--output-dir", str(base / "missing-output"))
        assert result.returncode != 0 and "--calibration FILE is required" in result.stderr

        root, env = make_root(base / "missing-full")
        result = run(root, env, "--output-dir", str(base / "missing-full-output"))
        assert result.returncode != 0 and "--calibration FILE is required" in result.stderr

        root, env = make_root(base / "baseline")
        output = base / "baseline-output"
        result = run(root, env, "--baseline-only", "--skip-graphs", "--calibration", str(supplied), "--output-dir", str(output))
        assert result.returncode == 0, (result.stdout, result.stderr)
        run_dir = only_run(output)
        invocations = [shlex.split(line) for line in (run_dir / "benchmark-invocations.txt").read_text().splitlines()]
        assert all("--calibration-artifact" not in args and "--production-real-migration" not in args for args in invocations)
        assert not (run_dir / "metadata/calibration.csv").exists()
        assert not (run_dir / "runtime-args.txt").exists()

    print("run_full_experiment_calibration_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
