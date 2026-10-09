#!/usr/bin/env python3
"""Read-only structural summary for a retained P4-C full-run directory."""

import argparse
import csv
import json
import math
from pathlib import Path


def read_csv(path):
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def truth(row, key):
    return row.get(key) == "true"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("run_directory", type=Path)
    args = parser.parse_args()
    root = args.run_directory
    manifest_path = root / "manifest.json"
    timing_path = root / "raw_timing.csv"
    cost_path = root / "raw_cost.csv"
    errors = []

    for path in (manifest_path, timing_path, cost_path):
        if not path.is_file():
            errors.append(f"missing:{path.name}")
    if errors:
        print("STRUCTURAL_ERRORS=" + ",".join(errors))
        return 1

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    timing = read_csv(timing_path)
    costs = read_csv(cost_path)
    valid_timing = [row for row in timing if truth(row, "measurement_valid")]
    valid_warmups = [row for row in valid_timing if truth(row, "warmup")]
    valid_measured = [row for row in valid_timing if row.get("warmup") == "false"]

    def exact_cost(row):
        try:
            elapsed = float(row.get("elapsed_ms", ""))
            return (truth(row, "measurement_valid") and math.isfinite(elapsed) and elapsed > 0 and
                    all(int(row.get(key, "")) == 4096 for key in
                        ("requested_pages", "attempted_pages", "migrated_pages")) and
                    int(row.get("failed_pages", "")) == 0)
        except ValueError:
            return False

    exact_costs = [row for row in costs if exact_cost(row)]
    cost_warmups = [row for row in exact_costs if truth(row, "warmup")]
    cost_measured = [row for row in exact_costs if row.get("warmup") == "false"]
    benchmark_failures = [row for row in timing if row.get("benchmark_exit_status") != "0"]
    placement_failures = [row for row in timing if row.get("verification_status") != "PASS"]
    restore_failures = [row for row in timing if row.get("memory_policy_restored") != "true"]

    topology = {(row.get("local_node"), row.get("remote_node"), row.get("numa_distance"))
                for row in valid_timing}
    cost_topology = {(row.get("source_node"), row.get("destination_node"), row.get("distance"), row.get("page_size"))
                     for row in exact_costs}
    impossible = [value for value in topology if value[0] == value[1] or not all(value)]

    if manifest.get("mode") != "full": errors.append("manifest_mode_not_full")
    if manifest.get("status") != "NOT_PRODUCTION_CALIBRATION": errors.append("unexpected_manifest_status")
    if manifest.get("cost_migration_valid") is not True: errors.append("cost_migration_invalid")
    if len(timing) != 126: errors.append(f"timing_rows:{len(timing)}")
    if len(valid_warmups) != 28: errors.append(f"valid_timing_warmups:{len(valid_warmups)}")
    if len(valid_measured) != 98: errors.append(f"valid_timing_measured:{len(valid_measured)}")
    if len(cost_warmups) != 2: errors.append(f"valid_cost_warmups:{len(cost_warmups)}")
    if len(cost_measured) != 7: errors.append(f"valid_cost_measured:{len(cost_measured)}")
    if len(topology) != 1 or impossible: errors.append("timing_topology_inconsistent")
    if len(cost_topology) != 1: errors.append("cost_topology_inconsistent")
    if benchmark_failures: errors.append(f"benchmark_failures:{len(benchmark_failures)}")
    if placement_failures: errors.append(f"placement_failures:{len(placement_failures)}")
    if restore_failures: errors.append(f"policy_restore_failures:{len(restore_failures)}")

    print(f"manifest_mode={manifest.get('mode')}")
    print(f"manifest_status={manifest.get('status')}")
    print(f"raw_timing_rows={len(timing)}")
    print(f"raw_cost_rows={len(costs)}")
    print(f"valid_timing_warmups={len(valid_warmups)}")
    print(f"valid_timing_measured={len(valid_measured)}")
    print(f"exact_4096_cost_rows={len(exact_costs)}")
    print(f"valid_cost_warmups={len(cost_warmups)}")
    print(f"valid_cost_measured={len(cost_measured)}")
    print(f"timing_topology={sorted(topology)}")
    print(f"cost_topology={sorted(cost_topology)}")
    print(f"benchmark_failures={len(benchmark_failures)}")
    print(f"placement_failures={len(placement_failures)}")
    print(f"policy_restore_failures={len(restore_failures)}")
    print("structural_status=" + ("PASS" if not errors else "FAIL:" + ",".join(errors)))
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
