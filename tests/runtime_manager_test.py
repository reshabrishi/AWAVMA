#!/usr/bin/env python3
import csv
import os
import subprocess


def main():
    subprocess.run(
        ["./bin/application_manager", "--once", "--results-dir", "results/runtime_python"],
        check=True,
    )
    table = "results/runtime_application_table.csv"
    results = "results/runtime_manager_results.csv"
    assert os.path.exists(table)
    assert os.path.exists(results)
    with open(table, newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert all(row["app_id"].startswith("APP_") for row in rows)
    assert len({row["app_id"] for row in rows}) == len(rows)
    with open(results, newline="") as stream:
        result_rows = list(csv.DictReader(stream))
    assert len(result_rows) == len(rows)
    print("runtime manager Python tests: PASS")


if __name__ == "__main__":
    main()
