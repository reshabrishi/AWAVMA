#!/usr/bin/env python3
"""Regression coverage for legacy benchmark placement patterns."""
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BENCHMARK = ROOT / "bin" / "benchmark"


def run(*arguments):
    return subprocess.run([str(BENCHMARK), *arguments], text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main():
    source = (ROOT / "src" / "benchmark.c").read_text()
    collector = (ROOT / "tools" / "collect_numa_calibration.sh").read_text()

    # The legacy pattern remains placement-oriented and therefore must not
    # become a second authority in a controlled run.
    assert "config->thread_node = config->memory_node = first;" in source
    assert "local pattern requires identical thread and memory nodes" in source
    assert "controlled placement conflicts with --memory-node" in source
    assert 'WORKLOADS=(sequential random hot moderate cold mixed changing)' in collector
    assert 'WORKLOADS=(sequential random hot moderate cold mixed changing local)' not in collector
    assert '"worker-evidence-socket", required_argument' in source
    assert 'config->worker_evidence_socket = optarg' in source
    assert '--worker-evidence-socket PATH' in source

    help_output = run("--help")
    assert help_output.returncode == 0
    assert "worker-evidence-socket" in help_output.stdout

    # The option is recognized and still cannot bypass P2 registration.
    missing_registration = run("--worker-evidence-socket", "/tmp/test.sock")
    assert missing_registration.returncode != 0

    # Direct legacy local placement remains usable on a single-NUMA host.
    direct = run("--threads", "1", "--memory", "1", "--iterations", "1", "--pattern", "local")
    assert direct.returncode == 0, direct.stderr
    assert "Pattern          : local" in direct.stdout
    print("benchmark_cli_regression_test: PASS")


if __name__ == "__main__":
    main()
