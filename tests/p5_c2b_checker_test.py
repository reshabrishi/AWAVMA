#!/usr/bin/env python3
import csv
import math
import statistics
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECKER = ROOT / "tools/check_p5_locality_validation.py"
RAW = "schema_version validation_id run_index run_kind pattern placement worker_index interval_index load_operations_delta interval_ms registered_memory_load_rate metric_name metric_unit valid status".split()
SUMMARY = "schema_version validation_id run_index pattern placement worker_index interval_count window_interval_count window_count load_operations_delta interval_ms worker_run_median_registered_memory_load_rate metric_name metric_unit valid status".split()
MANIFEST = "schema_version format_version validation_id matrix_version hardware_fingerprint pattern placement local_node remote_node numa_distance workers memory_mb duration_ms startup_discard_ms seed_base benchmark_definition_version pattern_version intensity_percent numa_balancing_state warmup_runs measured_runs run_count raw_interval_count worker_run_count minimum_worker_run_units status metric_name metric_unit minimum maximum mean median population_stddev p10 p90 classification_authority runtime_authority expected_gain_authority production_migration_authority authoritative".split()
RUN = "schema_version validation_id run_index run_kind pattern placement intensity_percent workers memory_mb duration_ms startup_discard_ms seed local_node remote_node requested_memory_node numa_distance worker0_tid worker0_cpu worker0_package worker0_core worker0_siblings worker1_tid worker1_cpu worker1_package worker1_core worker1_siblings affinity_verified registration_ok registration_generation worker0_evidence_seen worker1_evidence_seen worker_evidence_ok start_total_pages start_queryable_pages start_expected_pages start_local_pages start_remote_pages start_other_pages start_unknown_pages start_status end_total_pages end_queryable_pages end_expected_pages end_local_pages end_remote_pages end_other_pages end_unknown_pages end_status benchmark_exit_code numa_balancing_original numa_balancing_disabled numa_balancing_restored valid reason authoritative".split()


def write(path, fields, rows):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader(); writer.writerows(rows)


def percentile(values, fraction):
    ordered = sorted(values); position = fraction * (len(values) - 1); lower = math.floor(position)
    return ordered[lower] + (ordered[min(lower + 1, len(values) - 1)] - ordered[lower]) * (position - lower)


def fixture(directory, smoke=False):
    warmups, measured, authority = (1, 1, 0) if smoke else (2, 7, 1)
    memory, duration, discard = ("64", "5000", "1000") if smoke else ("256", "20000", "2000")
    runs, raw, summaries, units = [], [], [], {}; index = 0
    for pattern_number, pattern in enumerate(("cold", "moderate", "hot"), 1):
        for placement_number, placement in enumerate(("LOCAL", "REMOTE"), 1):
            units[pattern, placement] = []
            for run_number in range(warmups + measured):
                kind = "WARMUP" if run_number < warmups else "MEASURED"
                row = {field: "" for field in RUN}
                row.update(schema_version="1", validation_id="fixture", run_index=str(index), run_kind=kind, pattern=pattern,
                            placement=placement, intensity_percent="100", workers="2", memory_mb=memory, duration_ms=duration,
                            startup_discard_ms=discard, seed=str(12345 + index), local_node="0", remote_node="1",
                           requested_memory_node="0" if placement == "LOCAL" else "1", numa_distance="20",
                           worker0_tid="100", worker0_cpu="0", worker0_package="0", worker0_core="0", worker0_siblings="0,32",
                           worker1_tid="101", worker1_cpu="2", worker1_package="0", worker1_core="1", worker1_siblings="2,34",
                           affinity_verified="1", registration_ok="1", registration_generation="9", worker0_evidence_seen="1",
                           worker1_evidence_seen="1", worker_evidence_ok="1", start_total_pages="64", start_queryable_pages="64",
                           start_expected_pages="64", start_local_pages="64" if placement == "LOCAL" else "0",
                           start_remote_pages="64" if placement == "REMOTE" else "0", start_other_pages="0", start_unknown_pages="0",
                           start_status="PASS", end_total_pages="64", end_queryable_pages="64", end_expected_pages="64",
                           end_local_pages="64" if placement == "LOCAL" else "0", end_remote_pages="64" if placement == "REMOTE" else "0",
                           end_other_pages="0", end_unknown_pages="0", end_status="PASS", benchmark_exit_code="0",
                           numa_balancing_original="1", numa_balancing_disabled="1", numa_balancing_restored="1",
                           valid="1", reason="OK", authoritative=str(authority))
                runs.append(row)
                if kind == "MEASURED":
                    for worker in range(2):
                        rate = pattern_number * 10 + placement_number + run_number / 10 + worker / 100
                        operations = round(rate * 100)
                        for interval in range(5):
                            raw.append(dict(zip(RAW, ["1", "fixture", str(index), kind, pattern, placement, str(worker),
                                str(interval + 1), str(operations), "100", str(operations / 100), "registered_memory_load_rate",
                                "ops/ms", "1", "OK"])))
                        summaries.append(dict(zip(SUMMARY, ["1", "fixture", str(index), pattern, placement, str(worker), "5", "5", "1",
                            str(operations * 5), "500", str(operations / 100), "registered_memory_load_rate", "ops/ms", "1", "OK"])))
                        units[pattern, placement].append(operations / 100)
                index += 1
    manifests = []
    for pattern in ("cold", "moderate", "hot"):
        for placement in ("LOCAL", "REMOTE"):
            values = units[pattern, placement]
            manifests.append(dict(zip(MANIFEST, ["1", "1", "fixture", "matrix-b-v1", "arch=x86_64|family=6|model=1|nodes=2",
                pattern, placement, "0", "1", "20", "2", memory, duration, discard, "12345", "benchmark-locality-intensity-v1",
                "locality-patterns-v1", "100", "disabled", str(warmups), str(measured), str(len(runs)), str(len(raw)), str(len(values)),
                "12", "SMOKE_VALID" if smoke else "DESCRIPTIVE_VALID", "registered_memory_load_rate", "ops/ms", str(min(values)),
                str(max(values)), str(statistics.fmean(values)), str(statistics.median(values)), str(statistics.pstdev(values)),
                str(percentile(values, .1)), str(percentile(values, .9)), "UNAVAILABLE", "DISABLED", "DISABLED", "DISABLED", str(authority)])))
    write(directory / "raw_intervals.csv", RAW, raw); write(directory / "worker_run_summaries.csv", SUMMARY, summaries)
    write(directory / "locality_manifest.csv", MANIFEST, manifests); write(directory / "locality_validation_runs.csv", RUN, runs)
    return runs


with tempfile.TemporaryDirectory() as temporary:
    directory = Path(temporary); runs = fixture(directory)
    subprocess.run(["python3", str(CHECKER), str(directory)], check=True)
    runs[1]["seed"] = "12345"; write(directory / "locality_validation_runs.csv", RUN, runs)
    assert subprocess.run(["python3", str(CHECKER), str(directory)], capture_output=True).returncode == 1
with tempfile.TemporaryDirectory() as temporary:
    directory = Path(temporary); fixture(directory, smoke=True)
    subprocess.run(["python3", str(CHECKER), str(directory)], check=True)

source = (ROOT / "src/p5_c2b_locality_main.c").read_text(encoding="utf-8")
assert '"--memory-node"' not in source
for required in ('"--worker-cpus"', '"--placement-mode"', '"--page-registration-required"',
                 "options->duration_ms + 10000U", "numa_begin", "numa_restore", "read_placement"):
    assert required in source
assert not any((ROOT / path).read_text(encoding="utf-8").find("p5_c2b") >= 0 for path in
               ("src/p5_c2a_collector.c", "src/p5_c2a_collector_main.c", "include/p5_c2a_collector.h"))
print("P5C2B_CHECKER_AND_SOURCE_CONTRACT: PASS")
