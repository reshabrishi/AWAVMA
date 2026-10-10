#!/usr/bin/env python3
"""Aggregate one explicit Phase 4D run, optionally using one baseline reference."""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import re
import ipaddress
from pathlib import Path

SCENARIOS = ("baseline-default", "baseline-local", "baseline-remote", "awavma")
METRICS = ("operations", "execution_time_seconds", "throughput_ops_per_second", "mean_wall_time_per_operation_seconds")
SUMMARY_FIELDS = ("schema_version", "experiment_id", "metric", "comparison_key", "scenario", "records", "mean", "median", "sample_stdev", "minimum", "maximum", "ci95_low", "ci95_high", "data_source", "provenance")
COMPARATOR_FIELDS = ("schema_version", "experiment_id", "metric", "comparison_key", "comparison", "baseline_scenario", "baseline_mean", "awavma_mean", "absolute_change", "percent_change", "records_per_scenario", "data_source", "collection_status", "comparable", "provenance")
ADDRESS_FIELD = re.compile(r"(^|_)(address|addr|ip|ipv4|ipv6|mac)(_|$)", re.I)
IPV4 = re.compile(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])")
MAC = re.compile(r"(?i)(?<![0-9a-f])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-f])")
T95 = {2:12.706, 3:4.303, 4:3.182, 5:2.776, 6:2.571, 7:2.447, 8:2.365, 9:2.306, 10:2.262, 11:2.228, 12:2.201, 13:2.179, 14:2.160, 15:2.145, 16:2.131, 17:2.120, 18:2.110, 19:2.101, 20:2.093, 21:2.086, 22:2.080, 23:2.074, 24:2.069, 25:2.064, 26:2.060, 27:2.056, 28:2.052, 29:2.048, 30:2.045}


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise ValueError(f"required artifact missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if any(ADDRESS_FIELD.search(field) for field in reader.fieldnames or []):
            raise ValueError(f"address-bearing field rejected: {path}")
        rows = list(reader)
    def address_value(value: str) -> bool:
        if IPV4.search(value) or MAC.search(value):
            return True
        for token in re.split(r"[\s,;]+", value):
            try:
                ipaddress.ip_address(token.strip("[]()"))
                return True
            except ValueError:
                pass
        return False
    if any(address_value(value or "") for row in rows for value in row.values()):
        raise ValueError(f"IP/MAC value rejected: {path}")
    return rows


def manifest_data(run: Path) -> dict:
    manifest = run / "manifest.json"
    if not manifest.is_file():
        raise ValueError(f"reference manifest missing: {manifest}")
    return json.loads(manifest.read_text(encoding="utf-8"))


def passing_reference(run: Path) -> dict:
    data = manifest_data(run)
    if data.get("data_source") != "REAL" or data.get("collection_status") != "PASS":
        raise ValueError("reference run is not a passing REAL run")
    if data.get("collection_mode") != "baseline" or data.get("protocol_version") != "phase4d-v1":
        raise ValueError("reference run is not a Phase 4D baseline reference")
    required = ("topology_sha256", "local_node", "remote_node", "numa_distance", "numa_balancing_during")
    if any(data.get(field) in (None, "") for field in required):
        raise ValueError("reference manifest lacks compatibility provenance")
    return data


def reject_duplicate(rows: list[dict[str, str]], source: str) -> None:
    seen = set()
    for row in rows:
        identity = (row.get("scenario", ""), row.get("repetition", ""))
        if not all(identity) or identity in seen:
            raise ValueError(f"duplicate or incomplete {source} identity: {identity}")
        seen.add(identity)


def finite(value: str) -> float:
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"non-finite metric value: {value}")
    return number


def load(run: Path, scenarios: set[str]) -> list[dict[str, str]]:
    manifest = manifest_data(run)
    measurements = read_csv(run / "measurements.csv")
    benchmark_rows = read_csv(run / "benchmark_results.csv")
    reject_duplicate(measurements, "measurement")
    reject_duplicate(benchmark_rows, "benchmark")
    benchmark = {(row.get("scenario", ""), row.get("repetition", "")): row for row in benchmark_rows}
    selected = [row for row in measurements if row.get("scenario") in scenarios]
    if not selected:
        raise ValueError(f"no requested measurements in {run}")
    repetitions = {scenario: set() for scenario in scenarios}
    output = []
    measurement_ids = {(row.get("scenario", ""), row.get("repetition", "")) for row in selected}
    benchmark_ids = {identity for identity in benchmark if identity[0] in scenarios}
    if measurement_ids != benchmark_ids:
        raise ValueError("measurement and benchmark repetition sets differ")
    for row in selected:
        scenario, repetition = row.get("scenario", ""), row.get("repetition", "")
        repetitions[scenario].add(repetition)
        required = ("registration_status", "benchmark_status", "runtime_status", "initial_placement_status", "final_placement_status", "status")
        if any(not row.get(field) for field in required):
            raise ValueError(f"{scenario}/{repetition}: incomplete status fields")
        if row["status"] != "MEASURED" or row.get("exit_code") != "0":
            raise ValueError(f"{scenario}/{repetition}: failed measurement")
        expected_statuses = {"registration_status":"ACCEPTED", "benchmark_status":"PASS",
                             "runtime_status":"PASS", "initial_placement_status":"PASS",
                             "final_placement_status":"PASS"} if scenario == "awavma" else {
                             "registration_status":"NOT_REQUIRED", "benchmark_status":"PASS",
                             "runtime_status":"NOT_REQUIRED", "initial_placement_status":"PASS",
                             "final_placement_status":"PASS"}
        if any(row.get(field) != value for field, value in expected_statuses.items()):
            raise ValueError(f"{scenario}/{repetition}: unsuccessful measurement semantics")
        raw = benchmark.get((scenario, repetition))
        if raw is None:
            raise ValueError(f"{scenario}/{repetition}: benchmark result missing")
        expected = (row.get("workload", ""), row.get("threads", ""),
                    str(int(row.get("memory_mb", "")) * 1024 * 1024), row.get("duration_seconds", ""))
        actual = (raw.get("pattern", ""), raw.get("threads", ""),
                  str(int(raw.get("memory_mb", "")) * 1024 * 1024), raw.get("duration_sec", ""))
        declared = (str(manifest.get("workload", "")), str(manifest.get("threads", "")),
                    str(manifest.get("memory_bytes", "")), str(manifest.get("duration_seconds", "")))
        if expected != actual or expected != declared:
            raise ValueError(f"{scenario}/{repetition}: workload/config differs across measurement, benchmark, and manifest")
        if scenario == "awavma":
            profiles = [run / f"awavma/runtime/{repetition}/runtime_execution_profile.csv",
                        run / f"metadata/awavma-runtime-profile-{repetition}.csv"]
            profile_path = next((path for path in profiles if path.is_file()), None)
            if profile_path is None:
                raise ValueError(f"{scenario}/{repetition}: runtime profile missing")
            profiles_rows = read_csv(profile_path)
            if len(profiles_rows) != 1:
                raise ValueError(f"{scenario}/{repetition}: runtime profile must contain one row")
            profile = profiles_rows[0]
            runtime_actual = tuple(profile.get(field, "") for field in
                                   ("controlled_workload_pattern", "controlled_workload_threads",
                                    "controlled_workload_memory_bytes", "controlled_workload_duration_seconds"))
            if runtime_actual != expected:
                raise ValueError(f"{scenario}/{repetition}: runtime controlled workload mismatch")
        operations = finite(raw.get("operations", ""))
        execution = finite(raw.get("execution_time_sec", ""))
        throughput = finite(raw.get("throughput_ops_sec", ""))
        if operations <= 0 or execution <= 0 or throughput < 0:
            raise ValueError(f"{scenario}/{repetition}: invalid benchmark metrics")
        enriched = dict(row)
        enriched.update({"operations": str(operations), "execution_time_seconds": str(execution),
                         "throughput_ops_per_second": str(throughput),
                         "mean_wall_time_per_operation_seconds": str(execution / operations),
                         "provenance": f"{run.name}/measurements.csv+benchmark_results.csv:{scenario}/{repetition}"})
        output.append(enriched)
    counts = {scenario: len(values) for scenario, values in repetitions.items()}
    if any(not values for values in repetitions.values()) or len(set(counts.values())) != 1:
        raise ValueError(f"incomplete scenario repetitions: {counts}")
    return output


def write(path: Path, fields: tuple[str, ...] | list[str], rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--reference-run", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--comparison-output", type=Path, required=True)
    parser.add_argument("--summary-dir", type=Path, required=True)
    args = parser.parse_args()

    current_scenarios = {row.get("scenario", "") for row in read_csv(args.run_dir / "measurements.csv")}
    baseline_scenarios = set(SCENARIOS[:3])
    if "awavma" in current_scenarios and not {"baseline-default", "baseline-local", "baseline-remote"} <= current_scenarios:
        if args.reference_run is None:
            raise SystemExit("AWAVMA-only aggregation requires --reference-run")
        reference_manifest = passing_reference(args.reference_run)
        current_manifest = manifest_data(args.run_dir)
        compatibility = ("protocol_version", "topology_sha256", "local_node", "remote_node", "numa_distance", "numa_balancing_during", "workload", "threads", "memory_bytes", "duration_seconds")
        if any(current_manifest.get(field) != reference_manifest.get(field) for field in compatibility):
            raise SystemExit("reference run is incompatible with current topology/NUMA distance/policy/protocol")
        if not current_manifest.get("calibration_sha256"):
            raise SystemExit("AWAVMA run lacks calibration provenance")
        rows = load(args.reference_run, set(SCENARIOS[:3])) + load(args.run_dir, {"awavma"})
    elif current_scenarios == baseline_scenarios:
        rows = load(args.run_dir, baseline_scenarios)
    else:
        if current_scenarios != set(SCENARIOS):
            raise SystemExit(f"full aggregation requires all scenarios: {sorted(current_scenarios)}")
        rows = load(args.run_dir, set(SCENARIOS))
    experiment_id = next((row.get("experiment_id", "") for row in rows if row.get("scenario") == "awavma"), args.run_dir.name)
    key_fields = ("workload", "threads", "memory_mb", "duration_seconds")
    if len({tuple(row.get(field, "") for field in key_fields) for row in rows}) != 1:
        raise SystemExit("scenario configuration mismatch")
    key = "|".join(rows[0].get(field, "") for field in key_fields)
    write(args.output, list(rows[0]), rows)

    summaries, comparisons = [], []
    summary_scenarios = SCENARIOS if "awavma" in current_scenarios else SCENARIOS[:3]
    for metric in METRICS:
        values = {scenario: [finite(row[metric]) for row in rows if row["scenario"] == scenario] for scenario in summary_scenarios}
        if not all(values.values()) or len({len(v) for v in values.values()}) != 1:
            raise SystemExit(f"incomplete metric {metric}")
        means = {scenario: statistics.mean(numbers) for scenario, numbers in values.items()}
        for scenario, numbers in values.items():
            mean = means[scenario]
            stdev = statistics.stdev(numbers) if len(numbers) > 1 else 0.0
            critical = T95[len(numbers)] if 2 <= len(numbers) <= 30 else 1.96
            margin = critical * stdev / math.sqrt(len(numbers))
            summaries.append({"schema_version":"6", "experiment_id":experiment_id, "metric":metric, "comparison_key":key,
                              "scenario":scenario, "records":str(len(numbers)), "mean":str(mean), "median":str(statistics.median(numbers)),
                              "sample_stdev":str(stdev), "minimum":str(min(numbers)), "maximum":str(max(numbers)),
                              "ci95_low":str(mean-margin), "ci95_high":str(mean+margin), "data_source":"REAL", "provenance":"explicit-run-only"})
        if "awavma" not in values:
            continue
        for baseline, label in (("baseline-remote", "primary_awavma_vs_remote"), ("baseline-local", "secondary_awavma_vs_local"), ("baseline-default", "secondary_awavma_vs_default")):
            base, awavma = means[baseline], means["awavma"]
            comparisons.append({"schema_version":"6", "experiment_id":experiment_id, "metric":metric, "comparison_key":key,
                                "comparison":label, "baseline_scenario":baseline, "baseline_mean":str(base), "awavma_mean":str(awavma),
                                "absolute_change":str(awavma-base), "percent_change":str((awavma-base)/base*100) if base else "",
                                "records_per_scenario":str(len(values["awavma"])), "data_source":"REAL", "collection_status":str(manifest_data(args.run_dir).get("collection_status", "")),
                                "comparable":"true", "provenance":"explicit-run-only"})
    write(args.comparison_output, COMPARATOR_FIELDS, comparisons)
    for metric in METRICS:
        write(args.summary_dir / f"{metric}_summary.csv", SUMMARY_FIELDS, [row for row in summaries if row["metric"] == metric])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
