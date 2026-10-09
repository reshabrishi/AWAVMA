#!/usr/bin/env python3
"""Strict P5-C.2B schema, provenance, arithmetic, and design validator."""
import csv
import math
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

RAW_FIELDS = "schema_version validation_id run_index run_kind pattern placement worker_index interval_index load_operations_delta interval_ms registered_memory_load_rate metric_name metric_unit valid status".split()
SUMMARY_FIELDS = "schema_version validation_id run_index pattern placement worker_index interval_count window_interval_count window_count load_operations_delta interval_ms worker_run_median_registered_memory_load_rate metric_name metric_unit valid status".split()
MANIFEST_FIELDS = "schema_version format_version validation_id matrix_version hardware_fingerprint pattern placement local_node remote_node numa_distance workers memory_mb duration_ms startup_discard_ms seed_base benchmark_definition_version pattern_version intensity_percent numa_balancing_state warmup_runs measured_runs run_count raw_interval_count worker_run_count minimum_worker_run_units status metric_name metric_unit minimum maximum mean median population_stddev p10 p90 classification_authority runtime_authority expected_gain_authority production_migration_authority authoritative".split()
RUN_FIELDS = "schema_version validation_id run_index run_kind pattern placement intensity_percent workers memory_mb duration_ms startup_discard_ms seed local_node remote_node requested_memory_node numa_distance worker0_tid worker0_cpu worker0_package worker0_core worker0_siblings worker1_tid worker1_cpu worker1_package worker1_core worker1_siblings affinity_verified registration_ok registration_generation worker0_evidence_seen worker1_evidence_seen worker_evidence_ok start_total_pages start_queryable_pages start_expected_pages start_local_pages start_remote_pages start_other_pages start_unknown_pages start_status end_total_pages end_queryable_pages end_expected_pages end_local_pages end_remote_pages end_other_pages end_unknown_pages end_status benchmark_exit_code numa_balancing_original numa_balancing_disabled numa_balancing_restored valid reason authoritative".split()
SCHEMAS = {"raw_intervals.csv": RAW_FIELDS, "worker_run_summaries.csv": SUMMARY_FIELDS,
           "locality_manifest.csv": MANIFEST_FIELDS, "locality_validation_runs.csv": RUN_FIELDS}
PATTERNS = ("cold", "moderate", "hot")
PLACEMENTS = ("LOCAL", "REMOTE")


def fail(message):
    raise ValueError(message)


def read(path, fields):
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != fields:
            fail(f"{path.name}: schema mismatch")
        rows = list(reader)
    if any(None in row or None in row.values() for row in rows):
        fail(f"{path.name}: malformed row")
    return rows


def integer(row, field, minimum=0):
    try:
        value = int(row[field])
    except ValueError:
        fail(f"invalid integer {field}")
    if value < minimum:
        fail(f"out-of-range {field}")
    return value


def number(row, field):
    try:
        value = float(row[field])
    except ValueError:
        fail(f"invalid decimal {field}")
    if not math.isfinite(value):
        fail(f"non-finite decimal {field}")
    return value


def percentile(values, fraction):
    ordered = sorted(values)
    position = fraction * (len(ordered) - 1)
    lower = math.floor(position)
    return ordered[lower] + (ordered[min(lower + 1, len(ordered) - 1)] - ordered[lower]) * (position - lower)


def close(actual, expected, label):
    if not math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-12):
        fail(f"{label} mismatch: {actual} != {expected}")


def check(directory):
    if not directory.is_dir():
        fail("artifact directory missing")
    tables = {name: read(directory / name, fields) for name, fields in SCHEMAS.items()}
    manifests, runs, raw, summaries = (tables["locality_manifest.csv"], tables["locality_validation_runs.csv"],
                                       tables["raw_intervals.csv"], tables["worker_run_summaries.csv"])
    if len(manifests) != 6 or {(r["pattern"], r["placement"]) for r in manifests} != {(p, q) for p in PATTERNS for q in PLACEMENTS}:
        fail("manifest must contain exactly six context rows")
    first = manifests[0]
    fixed = {"schema_version": "1", "format_version": "1", "matrix_version": "matrix-b-v1", "workers": "2",
             "seed_base": "12345", "benchmark_definition_version": "benchmark-locality-intensity-v1",
             "pattern_version": "locality-patterns-v1", "intensity_percent": "100", "numa_balancing_state": "disabled",
             "minimum_worker_run_units": "12", "metric_name": "registered_memory_load_rate", "metric_unit": "ops/ms",
             "classification_authority": "UNAVAILABLE", "runtime_authority": "DISABLED",
             "expected_gain_authority": "DISABLED", "production_migration_authority": "DISABLED"}
    shared = ("validation_id", "hardware_fingerprint", "local_node", "remote_node", "numa_distance", "warmup_runs",
              "measured_runs", "run_count", "raw_interval_count", "authoritative")
    for row in manifests:
        if any(row[key] != value for key, value in fixed.items()) or any(row[key] != first[key] for key in shared):
            fail("mixed hardware, topology, protocol, or manifest contract")
    identifier = first["validation_id"]
    warmups, measured, authoritative = integer(first, "warmup_runs"), integer(first, "measured_runs", 1), integer(first, "authoritative")
    expected_runs = 6 * (warmups + measured)
    if authoritative == 1:
        if (warmups, measured, expected_runs) != (2, 7, 54) or (first["memory_mb"], first["duration_ms"], first["startup_discard_ms"]) != ("256", "20000", "2000"):
            fail("authoritative design must be exactly 54 runs")
    elif authoritative == 0:
        if (warmups, measured) not in {(1, 1), (2, 7)}:
            fail("non-authoritative design must be smoke or an overridden full matrix")
        if (warmups, measured) == (1, 1) and (first["memory_mb"], first["duration_ms"], first["startup_discard_ms"]) != ("64", "5000", "1000"):
            fail("smoke controls mismatch")
    else:
        fail("invalid authoritative value")
    if integer(first, "run_count") != expected_runs or len(runs) != expected_runs:
        fail("run count mismatch")
    indexes = [integer(row, "run_index") for row in runs]
    if indexes != list(range(expected_runs)):
        fail("run indexes must be deterministic, ordered, and unique")
    counts = Counter((r["pattern"], r["placement"], r["run_kind"]) for r in runs)
    for pattern in PATTERNS:
        for placement in PLACEMENTS:
            if counts[pattern, placement, "WARMUP"] != warmups or counts[pattern, placement, "MEASURED"] != measured:
                fail("incomplete matrix cell")
    if authoritative and (Counter(r["placement"] for r in runs) != {"LOCAL": 27, "REMOTE": 27} or
                          Counter(r["pattern"] for r in runs) != {p: 18 for p in PATTERNS} or
                          Counter(r["run_kind"] for r in runs) != {"WARMUP": 12, "MEASURED": 42}):
        fail("authoritative matrix margins mismatch")
    topology = (integer(first, "local_node"), integer(first, "remote_node"), integer(first, "numa_distance", 1))
    if topology[0] == topology[1]:
        fail("local and remote topology must be distinct")
    run_by_index = {}
    for row in runs:
        index = integer(row, "run_index")
        run_by_index[index] = row
        requested = topology[0] if row["placement"] == "LOCAL" else topology[1]
        if row["validation_id"] != identifier or (integer(row, "local_node"), integer(row, "remote_node"), integer(row, "numa_distance", 1)) != topology:
            fail("mixed run topology or identifier")
        if integer(row, "requested_memory_node") != requested or integer(row, "seed") != 12345 + index:
            fail("requested placement or deterministic seed mismatch")
        if row["intensity_percent"] != "100" or row["workers"] != "2" or row["memory_mb"] != first["memory_mb"] or row["startup_discard_ms"] != first["startup_discard_ms"] or row["duration_ms"] != first["duration_ms"]:
            fail("run fixed controls mismatch")
        if row["numa_balancing_disabled"] != "1" or row["numa_balancing_restored"] != "1":
            fail("NUMA transaction was not restored")
        valid = row["valid"] == "1"
        if valid:
            required_true = ("affinity_verified", "registration_ok", "worker0_evidence_seen", "worker1_evidence_seen", "worker_evidence_ok")
            if any(row[field] != "1" for field in required_true) or integer(row, "registration_generation", 1) < 1:
                fail("valid run lacks required evidence")
            if row["start_status"] != "PASS" or row["end_status"] != "PASS" or row["benchmark_exit_code"] != "0" or row["reason"] != "OK":
                fail("valid run outcome mismatch")
        elif row["reason"] == "OK":
            fail("invalid run lacks diagnostic reason")
        if row["authoritative"] != str(authoritative):
            fail("mixed run authority")
        for prefix in ("start", "end"):
            if row[f"{prefix}_status"] != "PASS":
                continue
            total = integer(row, f"{prefix}_total_pages", 1)
            if integer(row, f"{prefix}_queryable_pages") != total or integer(row, f"{prefix}_expected_pages") != total or integer(row, f"{prefix}_other_pages") or integer(row, f"{prefix}_unknown_pages"):
                fail("residency evidence is incomplete")
            expected_pages = integer(row, f"{prefix}_{'local' if row['placement'] == 'LOCAL' else 'remote'}_pages")
            if expected_pages != total:
                fail("residency is not on requested node")
    valid_intervals = defaultdict(list)
    raw_counts = Counter()
    for row in raw:
        index, worker = integer(row, "run_index"), integer(row, "worker_index")
        run = run_by_index.get(index)
        if run is None or worker > 1 or row["validation_id"] != identifier or (row["run_kind"], row["pattern"], row["placement"]) != (run["run_kind"], run["pattern"], run["placement"]):
            fail("raw interval does not join to its run")
        operations, milliseconds = integer(row, "load_operations_delta"), integer(row, "interval_ms", 1)
        close(number(row, "registered_memory_load_rate"), operations / milliseconds, "raw rate")
        if row["metric_name"] != "registered_memory_load_rate" or row["metric_unit"] != "ops/ms":
            fail("raw metric mismatch")
        raw_counts[index, worker] += 1
        if row["valid"] == "1":
            if run["run_kind"] != "MEASURED" or row["status"] != "OK" or not 50 <= milliseconds <= 250:
                fail("valid interval violates measured cadence contract")
            valid_intervals[index, worker].append((operations, milliseconds))
    if len(raw) != integer(first, "raw_interval_count"):
        fail("raw interval count mismatch")
    unit_values = defaultdict(list)
    seen_summaries = set()
    if len(summaries) != 12 * measured:
        fail("worker-run summary count mismatch")
    for row in summaries:
        index, worker = integer(row, "run_index"), integer(row, "worker_index")
        run = run_by_index.get(index); key = (index, worker)
        if key in seen_summaries or worker > 1 or run is None or run["run_kind"] != "MEASURED":
            fail("duplicate or non-measured worker-run summary")
        seen_summaries.add(key)
        if (row["validation_id"], row["pattern"], row["placement"]) != (identifier, run["pattern"], run["placement"]):
            fail("summary does not join to run")
        intervals = valid_intervals[key]
        windows = []
        for offset in range(0, len(intervals) - 4, 5):
            group = intervals[offset:offset + 5]
            windows.append(sum(v[0] for v in group) / sum(v[1] for v in group))
        if integer(row, "window_interval_count") != 5 or integer(row, "interval_count") != len(intervals) or integer(row, "window_count") != len(windows):
            fail("K=5 window construction mismatch")
        if integer(row, "load_operations_delta") != sum(v[0] for v in intervals) or integer(row, "interval_ms") != sum(v[1] for v in intervals):
            fail("summary interval totals mismatch")
        if row["valid"] != "1":
            if row["status"] == "OK" or number(row, "worker_run_median_registered_memory_load_rate") != 0.0:
                fail("invalid summary has OK status")
            continue
        if not windows or row["status"] != "OK" or run["valid"] != "1":
            fail("invalid worker-run replicate")
        replicate = statistics.median(windows)
        close(number(row, "worker_run_median_registered_memory_load_rate"), replicate, "worker-run median")
        unit_values[run["pattern"], run["placement"]].append(replicate)
    for row in manifests:
        values = unit_values[row["pattern"], row["placement"]]
        if integer(row, "worker_run_count") != len(values):
            fail("manifest worker-run count mismatch")
        if authoritative and (not 12 <= len(values) <= 14 or row["status"] != "DESCRIPTIVE_VALID"):
            fail("authoritative context requires 12-14 valid units")
        if not authoritative:
            expected_units = 2 if (warmups, measured) == (1, 1) else 14
            expected_status = "SMOKE_VALID" if expected_units == 2 else "NON_AUTHORITATIVE_VALID"
            if len(values) != expected_units or row["status"] != expected_status:
                fail("non-authoritative context unit/status mismatch")
        expected = {"minimum": min(values), "maximum": max(values), "mean": statistics.fmean(values),
                    "median": statistics.median(values), "population_stddev": statistics.pstdev(values),
                    "p10": percentile(values, 0.1), "p90": percentile(values, 0.9)}
        for field, value in expected.items():
            close(number(row, field), value, f"manifest {field}")
    print(f"LOCALITY_VALIDATED id={identifier} runs={expected_runs} authority={'AUTHORITATIVE_DESCRIPTIVE' if authoritative else 'SMOKE'} classification_authority=UNAVAILABLE runtime_authority=DISABLED")


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2:
            fail("usage: check_p5_locality_validation.py DIRECTORY")
        check(Path(sys.argv[1]))
    except (OSError, ValueError) as error:
        print(f"LOCALITY_INVALID reason={error}", file=sys.stderr)
        sys.exit(1)
