#!/usr/bin/env python3
"""Executable calibration wiring checks with deterministic runner dependencies."""

from __future__ import annotations

import csv
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
WORKLOADS = ("sequential", "random", "hot", "moderate", "cold", "mixed", "changing")


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
    executable(root / "mock-bin/numactl", "#!/bin/sh\nprintf 'node 0 cpus: 0\\nnode 2 cpus: 2\\nnode distances:\\nnode   0   2\\n  0:  10  21\\n  2:  21  10\\n'\n")
    executable(root / "bin/environment-check", "#!/bin/sh\nprintf 'ok\\n'\n")
    executable(
        root / "bin/calibration-validate",
        "#!/bin/sh\ngrep -q '^VALID-CALIBRATION$' \"$1\" || exit 1\nprintf 'CALIBRATION_VALIDATED records=1\\n'\n",
    )
    executable(
        root / "scripts/verify_runtime_execution_profile.py",
        "#!/usr/bin/env python3\nimport sys\nif '--emit-fields' in sys.argv:\n print('PRODUCTION_REAL_MIGRATION,PRODUCTION_REAL_MIGRATION,true,true,true,1000,2,true,true,true,true,ACTIVE,active,true')\nelif '--emit-field' in sys.argv:\n print('/tmp/page-registration.sock' if sys.argv[-1] == 'page_registration_socket' else '/tmp/worker-evidence.sock')\n",
    )
    shutil.copy2(ROOT / "scripts/aggregate_experiment_results.py", root / "scripts/aggregate_experiment_results.py")
    (root / "scripts/aggregate_experiment_results.py").chmod(0o755)
    executable(root / "scripts/generate_multinuma_graphs.py", "#!/usr/bin/env python3\n")
    executable(
        root / "bin/benchmark",
        """#!/usr/bin/env python3
import pathlib, sys
args = sys.argv[1:]
path = pathlib.Path(args[args.index('--placement-evidence') + 1])
mode = args[args.index('--placement-mode') + 1]
header = 'schema_version,placement_mode,local_node,remote_node,requested_memory_node,numa_distance,total_pages,queryable_pages,expected_node_pages,local_pages,remote_pages,other_pages,unknown_pages,expected_node_ratio,observed_dominant_node,verification_status,verification_reason,memory_policy_restored\\n'
default = mode == 'default'
status = __import__('os').environ.get('FIXTURE_DEFAULT_STATUS', 'OBSERVED') if default else __import__('os').environ.get('FIXTURE_EXPLICIT_STATUS', 'PASS')
if default and __import__('os').environ.get('FIXTURE_DEFAULT_MALFORMED'):
    header = 'placement_mode,verification_status,memory_policy_restored\\n'
    row = f'{mode},{status},true\\n'
else:
    requested, expected, local, remote, ratio = (-1, 0, 10, 0, '0.0') if default else (2, 10, 0, 10, '1.0')
    row = f'1,{mode},0,2,{requested},21,10,10,{expected},{local},{remote},0,0,{ratio},0,{status},fixture,true\\n'
path.write_text(header + row)
(pathlib.Path(str(path) + '.start')).write_text(header + row)
output = pathlib.Path(args[args.index('--output') + 1])
pattern = args[args.index('--pattern') + 1]
if __import__('os').environ.get('FIXTURE_WORKLOAD_MISMATCH') == 'benchmark': pattern = 'mixed' if pattern != 'mixed' else 'random'
output.write_text(f'timestamp,pattern,threads,memory_mb,iterations,duration_sec,thread_node,memory_node,numa_nodes,operations,execution_time_sec,throughput_ops_sec\\n2026-01-01T00:00:00Z,{pattern},2,1024,0,30,0,2,2,300,30,10\\n')
if '/awavma/' in str(path):
    other = pathlib.Path(str(path).replace('/awavma/placement-awavma-', '/baseline/placement-baseline-remote-'))
    other.parent.mkdir(parents=True, exist_ok=True)
    other.write_text(header + row)
(path.parents[1] / 'benchmark-invocations.txt').open('a').write(' '.join(args) + '\\n')
(path.parents[1] / 'benchmark-pid.txt').write_text(str(__import__('os').getpid()))
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
pattern = args[args.index('--controlled-workload-pattern') + 1]
if __import__('os').environ.get('FIXTURE_WORKLOAD_MISMATCH') == 'runtime': pattern = 'mixed' if pattern != 'mixed' else 'random'
(runtime_root / 'runtime_execution_profile.csv').write_text(f'controlled_workload_pattern,controlled_workload_threads,controlled_workload_memory_bytes,controlled_workload_duration_seconds\\n{pattern},2,1073741824,30\\n')
(runtime_root.parents[2] / 'runtime-args.txt').write_text('\\n'.join(args) + '\\n')
(runtime_root.parents[2] / 'target-pid.txt').write_text(args[args.index('--pid') + 1])
time.sleep(.5)
""",
    )
    env = os.environ.copy()
    env["PATH"] = f"{root / 'mock-bin'}:{env['PATH']}"
    sysctl = root / "numa_balancing"
    sysctl.write_text("1\n", encoding="utf-8")
    env["AWAVMA_NUMA_BALANCING_PATH"] = str(sysctl)
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


def validate_placement(root: Path, artifact: Path, mode: str) -> subprocess.CompletedProcess[str]:
    command = (
        f"source <(sed '/^validate_remote_equivalence()/,$d' {shlex.quote(str(root / 'scripts/run_full_experiment.sh'))}); "
        f"validate_placement {shlex.quote(str(artifact))} {shlex.quote(mode)}"
    )
    return subprocess.run(["bash", "-c", command], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def reference_run(base: Path, workload: str = "mixed") -> Path:
    run = base / "reference"
    (run / "baseline").mkdir(parents=True)
    topology = hashlib.sha256(b'node 0 cpus: 0\nnode 2 cpus: 2\nnode distances:\nnode   0   2\n  0:  10  21\n  2:  21  10\n').hexdigest()
    (run / "manifest.json").write_text(json.dumps({"data_source":"REAL","collection_status":"PASS","collection_mode":"baseline","protocol_version":"phase4d-v1","topology_sha256":topology,"local_node":0,"remote_node":2,"numa_distance":"21","numa_balancing_during":"0","workload":workload,"threads":2,"memory_bytes":1073741824,"duration_seconds":30}) + "\n", encoding="utf-8")
    measurement_fields = ("scenario", "repetition", "workload", "threads", "memory_mb", "duration_seconds", "registration_status", "benchmark_status", "runtime_status", "initial_placement_status", "final_placement_status", "status", "exit_code")
    benchmark_fields = ("scenario", "repetition", "pattern", "threads", "memory_mb", "duration_sec", "operations", "execution_time_sec", "throughput_ops_sec")
    with (run / "measurements.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=measurement_fields)
        writer.writeheader()
        for scenario in ("baseline-default", "baseline-local", "baseline-remote"):
            writer.writerow({"scenario":scenario, "repetition":"1", "workload":workload, "threads":"2", "memory_mb":"1024", "duration_seconds":"30", "registration_status":"NOT_REQUIRED", "benchmark_status":"PASS", "runtime_status":"NOT_REQUIRED", "initial_placement_status":"PASS", "final_placement_status":"PASS", "status":"MEASURED", "exit_code":"0"})
    with (run / "benchmark_results.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=benchmark_fields)
        writer.writeheader()
        for scenario in ("baseline-default", "baseline-local", "baseline-remote"):
            writer.writerow({"scenario":scenario, "repetition":"1", "pattern":workload, "threads":"2", "memory_mb":"1024", "duration_sec":"30", "operations":"300", "execution_time_sec":"30", "throughput_ops_sec":"10"})
    header = "placement_mode,local_node,requested_memory_node,numa_distance,total_pages,queryable_pages,local_pages,remote_pages,other_pages,unknown_pages,expected_node_pages,expected_node_ratio,verification_status,memory_policy_restored\n"
    (run / "baseline/placement-baseline-remote-1.start.csv").write_text(header + "remote,0,2,21,10,10,0,10,0,0,10,1.0,PASS,true\n", encoding="utf-8")
    return run


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="awavma-runner-calibration-") as temporary:
        base = Path(temporary)

        supplied = base / "supplied calibration.csv"
        supplied.write_text("VALID-CALIBRATION\n", encoding="utf-8")
        production_manifest = supplied.parent / "manifest.json"
        production_manifest.write_text('{"status":"PRODUCTION_CALIBRATION","overall_valid":true,"run_id":"fixture"}\n', encoding="utf-8")
        digest = hashlib.sha256(supplied.read_bytes()).hexdigest()
        for workload in WORKLOADS:
            root, env = make_root(base / f"valid-{workload}")
            output = base / f"valid-output-{workload}"
            result = run(root, env, "--awavma-only", "--reference-run", str(reference_run(base / f"valid-{workload}", workload)), "--skip-graphs", "--workload", workload, "--calibration", str(supplied), "--output-dir", str(output))
            assert result.returncode == 0, (workload, result.returncode, result.stdout, result.stderr)
            run_dir = only_run(output)
            copied = run_dir / "metadata/calibration.csv"
            assert copied.read_bytes() == supplied.read_bytes()
            assert (run_dir / "metadata/calibration.sha256").read_text().split()[0] == digest
            runtime_args = (run_dir / "runtime-args.txt").read_text().splitlines()
            assert runtime_args.count("--calibration-artifact") == 1
            assert runtime_args[runtime_args.index("--calibration-artifact") + 1] == str(copied)
            assert runtime_args[runtime_args.index("--calibration-manifest") + 1] == str(run_dir / "metadata/calibration-manifest.json")
            assert runtime_args[runtime_args.index("--controlled-workload-pattern") + 1] == workload
            assert runtime_args[runtime_args.index("--controlled-workload-threads") + 1] == "2"
            assert runtime_args[runtime_args.index("--controlled-workload-memory-bytes") + 1] == "1073741824"
            assert runtime_args[runtime_args.index("--controlled-workload-duration-seconds") + 1] == "30"
            assert {"--production-real-migration", "-S", "-M", "-R"} <= set(runtime_args)
            benchmark_args = shlex.split((run_dir / "benchmark-invocations.txt").read_text().splitlines()[0])
            assert benchmark_args[benchmark_args.index("--pattern") + 1] == workload
            assert benchmark_args[benchmark_args.index("--page-registration-socket") + 1] == "/tmp/page-registration.sock"
            assert benchmark_args[benchmark_args.index("--worker-evidence-socket") + 1] == "/tmp/worker-evidence.sock"
            assert "--page-registration-required" in benchmark_args
            assert (run_dir / "target-pid.txt").read_text() == (run_dir / "benchmark-pid.txt").read_text()
            manifest = json.loads((run_dir / "manifest.json").read_text())
            assert manifest["calibration_artifact"] == "metadata/calibration.csv"
            assert manifest["calibration_sha256"] == digest
            assert manifest["workload"] == workload
            assert manifest["calibration_manifest"] == "metadata/calibration-manifest.json"
            with (run_dir / "measurements.csv").open(newline="", encoding="utf-8") as handle:
                assert {row["workload"] for row in csv.DictReader(handle)} == {workload}
            with (run_dir / "benchmark_results.csv").open(newline="", encoding="utf-8") as handle:
                assert {row["pattern"] for row in csv.DictReader(handle)} == {workload}
            assert str(supplied) not in (run_dir / "manifest.json").read_text()

        for invalid_workload in ("unknown", " mixed ", "Mixed"):
            root, env = make_root(base / f"invalid-workload-{invalid_workload.strip() or 'spaces'}")
            result = run(root, env, "--workload", invalid_workload, "--awavma-only", "--output-dir", str(base / "invalid-workload-output"))
            assert result.returncode == 2 and f"unsupported workload: {invalid_workload}" in result.stderr

        for mismatch in ("benchmark", "runtime"):
            root, env = make_root(base / f"mismatch-{mismatch}")
            env["FIXTURE_WORKLOAD_MISMATCH"] = mismatch
            result = run(root, env, "--awavma-only", "--reference-run", str(reference_run(base / f"mismatch-{mismatch}", "sequential")), "--skip-graphs", "--workload", "sequential", "--calibration", str(supplied), "--output-dir", str(base / f"mismatch-output-{mismatch}"))
            expected = "workload/config differs across measurement, benchmark, and manifest" if mismatch == "benchmark" else "runtime controlled workload mismatch"
            assert result.returncode != 0 and expected in result.stderr

        root, env = make_root(base / "invalid")
        invalid = base / "invalid.csv"
        invalid.write_text("INVALID\n", encoding="utf-8")
        output = base / "invalid-output"
        result = run(root, env, "--awavma-only", "--reference-run", str(reference_run(base / "invalid")), "--calibration", str(invalid), "--output-dir", str(output))
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

        root, _ = make_root(base / "placement-validation")
        header = "schema_version,placement_mode,local_node,remote_node,requested_memory_node,numa_distance,total_pages,queryable_pages,expected_node_pages,local_pages,remote_pages,other_pages,unknown_pages,expected_node_ratio,observed_dominant_node,verification_status,verification_reason,memory_policy_restored\n"
        default = base / "default-observed.csv"
        default.write_text(header + "1,default,0,2,-1,21,10,7,0,4,3,0,3,0.0,0,OBSERVED,fixture,true\n", encoding="utf-8")
        assert validate_placement(root, default, "default").returncode == 0

        malformed = base / "default-malformed.csv"
        malformed.write_text("placement_mode,verification_status,memory_policy_restored\ndefault,OBSERVED,true\n", encoding="utf-8")
        assert validate_placement(root, malformed, "default").returncode != 0

        wrong_status = base / "default-wrong-status.csv"
        wrong_status.write_text(header + "1,default,0,2,-1,21,10,10,0,10,0,0,0,0.0,0,PASS,fixture,true\n", encoding="utf-8")
        assert validate_placement(root, wrong_status, "default").returncode != 0

        explicit_observed = base / "explicit-observed.csv"
        explicit_observed.write_text(header + "1,remote,0,2,2,21,10,10,10,0,10,0,0,1.0,2,OBSERVED,fixture,true\n", encoding="utf-8")
        assert validate_placement(root, explicit_observed, "remote").returncode != 0

        explicit_pass = base / "explicit-pass.csv"
        explicit_pass.write_text(header + "1,remote,0,2,2,21,10,10,10,0,10,0,0,1.0,2,PASS,fixture,true\n", encoding="utf-8")
        assert validate_placement(root, explicit_pass, "remote").returncode == 0

    print("run_full_experiment_calibration_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
