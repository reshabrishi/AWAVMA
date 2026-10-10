#!/usr/bin/env python3
"""Executable integration coverage for the P4-C NUMA-balancing transaction."""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def executable(path, text):
    path.write_text(text, encoding="utf-8")
    path.chmod(0o755)


def fixture(directory, original="1", benchmark_failure=False, cost_failure=False, sleep=False,
            changed_during=False, restore_failure=False):
    root = Path(directory) / "repo"
    (root / "tools").mkdir(parents=True)
    (root / "bin").mkdir()
    (root / "commands").mkdir()
    shutil.copy2(ROOT / "tools/collect_numa_calibration.sh", root / "tools")
    shutil.copy2(ROOT / "tools/validate_numa_calibration.py", root / "tools")
    shutil.copy2(ROOT / "tools/calibration_manifest_trust.py", root / "tools")
    sysctl = root / "numa_balancing"
    sysctl.write_text(original + "\n", encoding="ascii")
    executable(root / "commands/make", "#!/bin/sh\nexit 0\n")
    executable(root / "commands/git", "#!/bin/sh\nexit 0\n")
    executable(root / "commands/sudo", "#!/bin/sh\nprintf invoked >\"$P4C_SUDO_MARKER\"\nexit 1\n")
    executable(root / "commands/numactl", "#!/bin/sh\nprintf 'available: 2 nodes (0-1)\\n'\n")
    benchmark = """#!/bin/sh
artifact=
mode=
while [ $# -gt 0 ]; do
  case "$1" in
    --placement-evidence) artifact=$2; shift 2 ;;
    --placement-mode) mode=$2; shift 2 ;;
    *) shift ;;
  esac
done
%s
requested=0; [ "$mode" = remote ] && requested=1
printf 'schema_version,placement_mode,local_node,remote_node,requested_memory_node,numa_distance,total_pages,queryable_pages,expected_node_pages,local_pages,remote_pages,other_pages,unknown_pages,expected_node_ratio,observed_dominant_node,verification_status,verification_reason,memory_policy_restored\\n1,%%s,0,1,%%s,20,256,256,256,256,0,0,0,1,0,PASS,verified,true\\n' "$mode" "$requested" >"$artifact"
%s
""" % ("sleep 30" if sleep else ":", "exit 1" if benchmark_failure else "exit 0")
    if changed_during:
        benchmark = benchmark.replace("exit 0\n", 'printf "1\\n" >"$P4C_NUMA_BALANCING_PATH"\nexit 0\n')
    executable(root / "bin/benchmark", benchmark)
    cost = """#!/bin/sh
output=
warmup=
run_id=
while [ $# -gt 0 ]; do
  case "$1" in
    --output) output=$2; shift 2 ;;
    --warmup) warmup=$2; shift 2 ;;
    --run-id) run_id=$2; shift 2 ;;
    *) shift ;;
  esac
done
printf '%%s,%%s,4096,4096,%s,%s,1,0,1,20,4096,%s,%s\\n' "$run_id" "$warmup" >>"$output"
%s
exit %d
""" % ((('0', '4096', 'false', 'FAILED') if cost_failure else
          ('4096', '0', 'true', '')) +
         ('rm -f "$P4C_NUMA_BALANCING_PATH"; mkdir "$P4C_NUMA_BALANCING_PATH"' if restore_failure else ':',
          1 if cost_failure else 0))
    executable(root / "bin/p4c-migration-cost-collector", cost)
    env = os.environ.copy()
    env.update({"PATH": str(root / "commands") + os.pathsep + env["PATH"],
                "P4C_NUMA_BALANCING_PATH": str(sysctl),
                "P4C_OUTPUT_DIR": str(root / "output"), "P4C_DURATION_SECONDS": "0",
                "P4C_MEMORY_MB": "1", "P4C_COST_MAX_ATTEMPTS": "1"})
    return root, sysctl, env


def run_case(original="1", **kwargs):
    temporary = tempfile.TemporaryDirectory()
    root, sysctl, env = fixture(temporary.name, original, **kwargs)
    result = subprocess.run([root / "tools/collect_numa_calibration.sh", "--smoke"],
                            env=env, text=True, capture_output=True)
    manifests = list((root / "output/smoke").glob("p4c-*/manifest.json"))
    manifest = json.loads(manifests[0].read_text()) if manifests else None
    return temporary, result, sysctl, manifest, root


def main():
    for original in ("0", "1"):
        temporary, result, sysctl, manifest, root = run_case(original)
        try:
            assert result.returncode == 0, result.stderr
            assert sysctl.read_text().strip() == original
            assert manifest["schema_version"] == 4
            assert manifest["numa_balancing_original"] == original
            assert manifest["numa_balancing_during"] == "0"
            assert manifest["numa_balancing_restore_status"] == "RESTORED"
            assert manifest["timing_valid"] and manifest["cost_migration_valid"]
            assert manifest["numa_balancing_transaction_valid"] and manifest["overall_valid"]
            assert (root / "output/smoke/manifest.json").is_file()
        finally:
            temporary.cleanup()

    for options in ({"benchmark_failure": True}, {"cost_failure": True}):
        temporary, result, sysctl, manifest, root = run_case(**options)
        try:
            assert result.returncode != 0 and sysctl.read_text().strip() == "1"
            assert not manifest["overall_valid"]
            assert not (root / "output/smoke/manifest.json").exists()
        finally:
            temporary.cleanup()

    temporary, result, sysctl, manifest, root = run_case(changed_during=True)
    try:
        assert result.returncode != 0 and sysctl.read_text().strip() == "1"
        assert "NUMA_BALANCING_CHANGED_DURING_COLLECTION" in result.stderr
        assert not manifest["numa_balancing_transaction_valid"] and not manifest["overall_valid"]
    finally:
        temporary.cleanup()

    temporary, result, sysctl, manifest, root = run_case(restore_failure=True)
    try:
        assert result.returncode != 0 and sysctl.is_dir()
        assert "NUMA_BALANCING_RESTORE_FAILED" in result.stderr
        assert manifest["numa_balancing_restore_status"] == "FAILED"
        assert not manifest["numa_balancing_transaction_valid"] and not manifest["overall_valid"]
    finally:
        temporary.cleanup()

    # A signal while timing is active must still restore the original value.
    for original in ("0", "1"):
        with tempfile.TemporaryDirectory() as directory:
            root, sysctl, env = fixture(directory, original=original, sleep=True)
            process = subprocess.Popen([root / "tools/collect_numa_calibration.sh", "--smoke"], env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            deadline = time.time() + 5
            while (sysctl.read_text().strip() != "0" or not list((root / "output/smoke").glob("p4c-*/logs/*.log"))) and time.time() < deadline:
                time.sleep(0.02)
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=5)
            manifests = list((root / "output/smoke").glob("p4c-*/manifest.json"))
            assert process.returncode != 0 and sysctl.read_text().strip() == original
            assert manifests and not json.loads(manifests[0].read_text())["overall_valid"]

    # Invalid control values and non-writable controls fail before artifacts/work.
    with tempfile.TemporaryDirectory() as directory:
        root, sysctl, env = fixture(directory, original="2")
        result = subprocess.run([root / "tools/collect_numa_calibration.sh", "--smoke"], env=env,
                                capture_output=True, text=True)
        assert result.returncode != 0 and not (root / "output").exists()

    # An environment override can never become a privileged arbitrary write.
    with tempfile.TemporaryDirectory() as directory:
        root, sysctl, env = fixture(directory)
        marker = root / "sudo-invoked"
        env.update({"P4C_NUMA_BALANCING_PATH": str(root / "arbitrary-control"),
                    "P4C_SUDO_MARKER": str(marker)})
        result = subprocess.run([root / "tools/collect_numa_calibration.sh", "--smoke"], env=env,
                                capture_output=True, text=True)
        assert result.returncode != 0 and not marker.exists()

    collector = (ROOT / "tools/collect_numa_calibration.sh").read_text()
    assert '"$NUMA_BALANCING_PATH" == "$CANONICAL_NUMA_BALANCING_PATH"' in collector
    assert 'sudo -n sh -c' in collector

    print("p4c_numa_balancing_integration_test: PASS")


if __name__ == "__main__":
    main()
