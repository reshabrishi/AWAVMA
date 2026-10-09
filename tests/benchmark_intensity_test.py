#!/usr/bin/env python3
"""Focused controlled-activity pacing checks without NUMA or runtime dependencies."""
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BENCHMARK = ROOT / "bin" / "benchmark"


def run(*args):
    return subprocess.run([str(BENCHMARK), *args], text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=False)


def operations(output):
    return int(re.search(r"Operations\s+: (\d+)", output).group(1))


def checksum(output):
    return re.search(r"Checksum\s+: (\d+)", output).group(1)


def check(label, condition):
    assert condition, label
    print(f"{label}: PASS")


def main():
    source = (ROOT / "src" / "benchmark.c").read_text()

    check("INT01", all(run("--iterations", "1", "--intensity-percent", value).returncode == 0
                       for value in ("1", "10", "40", "100")))
    check("INT02", run("--iterations", "1", "--intensity-percent", "0").returncode != 0)
    check("INT03", run("--iterations", "1", "--intensity-percent", "101").returncode != 0)
    check("INT04", run("--iterations", "1", "--intensity-percent", "bad").returncode != 0)
    check("INT05", ".intensity_percent = 100" in source)
    check("INT06", "if (!config->intensity_mode_requested) while (true)" in source)
    check("INT07", "clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME" in source)
    check("INT08", "sleep_until(&period_end)" in source and "INTENSITY_PERIOD_MS" in source)
    check("INT09", "#define INTENSITY_PERIOD_MS 100ULL" in source)
    check("INT10", "checksum ^= data[index]" in source and "operations++;" in source)

    low = run("--threads", "1", "--memory", "1", "--duration", "0.7", "--intensity-percent", "10")
    mid = run("--threads", "1", "--memory", "1", "--duration", "0.7", "--intensity-percent", "40")
    high = run("--threads", "1", "--memory", "1", "--duration", "0.7", "--intensity-percent", "100")
    check("INT11", low.returncode == 0 and mid.returncode == 0 and operations(low.stdout) < operations(mid.stdout))
    check("INT12", high.returncode == 0 and operations(mid.stdout) < operations(high.stdout))

    default = run("--threads", "1", "--memory", "1", "--iterations", "1000", "--pattern", "sequential")
    explicit = run("--threads", "1", "--memory", "1", "--iterations", "1000", "--pattern", "sequential", "--intensity-percent", "100")
    check("INT13", default.returncode == 0 and explicit.returncode == 0 and
          operations(default.stdout) == operations(explicit.stdout) and checksum(default.stdout) == checksum(explicit.stdout))
    random_default = run("--threads", "1", "--memory", "1", "--iterations", "1000", "--pattern", "random")
    random_explicit = run("--threads", "1", "--memory", "1", "--iterations", "1000", "--pattern", "random", "--intensity-percent", "100")
    check("INT14", random_default.returncode == 0 and random_explicit.returncode == 0 and
          checksum(random_default.stdout) == checksum(random_explicit.stdout))
    check("INT15", "registration_generation = accepted_generation" in source)
    check("C1G02_BENCHMARK_PRESERVES_ACK_GENERATION", "registration_generation = accepted_generation" in source)
    check("C1G03_WORKER_CONTEXT_RECEIVES_GENERATION", "workers[i].registration_generation = registration_generation" in source)
    check("C1G05_NO_PRODUCTION_LITERAL_GENERATION", ".registration_generation = 1" not in source)
    check("C1G01_BENCHMARK_REQUESTS_REAL_GENERATION", "message.client_generation = start_time_ticks" in source)
    with tempfile.TemporaryDirectory() as temporary:
        failed_registration = run("--iterations", "1", "--page-registration-required",
                                  "--page-registration-socket", str(Path(temporary) / "registration.sock"),
                                  "--worker-evidence-socket", str(Path(temporary) / "evidence.sock"),
                                  "--page-registration-timeout-ms", "1")
        check("C1G07_REGISTRATION_FAILURE_PREVENTS_WORKER_EVIDENCE", failed_registration.returncode != 0)
    check("INT16", "p5_thread_activity" not in source)
    check("INT17", "thread_confidence" not in source)
    check("INT18", "Migration_Execute" not in source and "move_pages" not in source)
    check("INT19_DEFAULT_NO_OPTION_USES_LEGACY_MODE",
          ".intensity_mode_requested = false" in source and
          "if (!config->intensity_mode_requested) while (true)" in source)
    check("INT20_EXPLICIT_100_USES_CONTROLLED_MODE",
          "config->intensity_mode_requested = true" in source and
          "if (!config->intensity_mode_requested) while (true)" in source)
    check("INT21_EXPLICIT_100_HAS_100MS_PERIOD_BOUNDARY",
          "struct timespec period_end = timespec_add_ms(period_start, INTENSITY_PERIOD_MS);" in source)
    check("INT22_EXPLICIT_100_HAS_NO_INTENTIONAL_SLEEP",
          "if (config->intensity_percent < 100)" in source)
    check("INT23_EXPLICIT_100_USES_PERIOD_ALIGNED_PUBLICATION",
          "period_start = period_end;" in source and
          "if (config->worker_evidence_socket != NULL)" in source)
    check("INT24_DEFAULT_100_AND_EXPLICIT_100_USE_SAME_MEMORY_ACCESS_SEMANTICS",
          default.returncode == 0 and explicit.returncode == 0 and
          operations(default.stdout) == operations(explicit.stdout) and checksum(default.stdout) == checksum(explicit.stdout))
    check("INT25_LOW_MID_HIGH_ALL_USE_SAME_CONTROLLED_PERIOD_FRAMEWORK",
          "struct timespec active_deadline" in source and
          "INTENSITY_CHECK_BATCH" in source)
    check("INT26_LEGACY_CALLERS_UNCHANGED", default.returncode == 0 and random_default.returncode == 0)


if __name__ == "__main__":
    main()
