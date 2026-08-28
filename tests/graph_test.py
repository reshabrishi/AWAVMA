#!/usr/bin/env python3
"""Phase 9 graph-generation tests; uses only Python's standard library."""

from __future__ import annotations

import csv
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from xml.etree import ElementTree


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/generate_graphs.py"


def result(name: str, expected: str, actual: str, passed: bool) -> None:
    print(f"{name} | {expected} | {actual} | {'PASS' if passed else 'FAIL'}")
    if not passed:
        raise AssertionError(name)


def summary_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="awavma-graph-test-") as directory:
        temporary = Path(directory)
        output = temporary / "graphs"
        summary = temporary / "summary.csv"
        log = temporary / "graph.log"
        command = [sys.executable, str(SCRIPT), "--root", str(ROOT), "--output-dir", str(output), "--summary", str(summary), "--log", str(log)]
        first = subprocess.run(command, capture_output=True, text=True)
        result("G01", "generator exits successfully", str(first.returncode), first.returncode == 0)
        result("G02", "output directory created", str(output.is_dir()), output.is_dir())
        rows = summary_rows(summary)
        by_id = {row["graph_id"]: row for row in rows}
        expected_graphs = {
            "G02-1": "GENERATED",
            "G03-1": "GENERATED",
            "G04-1": "GENERATED",
            "G05-1": "GENERATED",
            "G06-1": "NO_DATA",
            "G07-1": "NO_DATA",
            "G08-9": "GENERATED",
            "G08-4": "GENERATED",
            "G08-11": "GENERATED",
            "G-C04": "GENERATED",
        }
        for graph_id, expected in expected_graphs.items():
            actual = by_id.get(graph_id, {}).get("status", "MISSING")
            result(graph_id, expected, actual, actual == expected)
        required_files = [
            output / "phase2/benchmark_throughput_by_pattern.svg",
            output / "phase3/cpu_utilization_over_time.svg",
            output / "phase4/classification_distribution.svg",
            output / "phase5/decision_distribution.svg",
            output / "phase8/all_12_factor_weights.svg",
            output / "phase8/application_adaptation.svg",
        ]
        for index, path in enumerate(required_files, 3):
            valid = path.is_file() and path.stat().st_size > 0
            if valid:
                try:
                    root = ElementTree.parse(path).getroot()
                    valid = float(root.attrib["width"]) > 0 and float(root.attrib["height"]) > 0
                    content = path.read_text(encoding="utf-8").lower()
                    valid = valid and re.search(r"(?<![a-z])(nan|inf(?:inity)?)(?![a-z])", content) is None
                except (OSError, ValueError, KeyError, ElementTree.ParseError):
                    valid = False
            result(f"G{index:02d}", "validated SVG exists and opens", str(path), valid)
        missing = temporary / "missing.csv"
        missing.write_text("a,b\n1,2\n", encoding="utf-8")
        sys.path.insert(0, str(ROOT / "scripts"))
        from graph_utils import finite, read_csv

        _, _, reason = read_csv(missing, ("required",))
        result("G12", "missing-column input handled", "missing columns" in reason, "missing columns" in reason)
        empty = temporary / "empty.csv"
        empty.write_text("", encoding="utf-8")
        _, _, reason = read_csv(empty, ("required",))
        result("G13", "empty input handled", bool(reason), bool(reason))
        result("G14", "NaN/Inf unavailable", "None", finite("NaN") is None and finite("Inf") is None)
        result("G15", "-1 unavailable, not zero", "None", finite("-1.000000") is None)
        result("G16", "controlled and real datasets separated", "CONTROLLED_TEST", by_id["G08-11"]["data_type"] == "CONTROLLED_TEST" and by_id["G02-1"]["data_type"] == "REAL")
        second_summary = temporary / "summary-second.csv"
        second_command = [sys.executable, str(SCRIPT), "--root", str(ROOT), "--output-dir", str(output), "--summary", str(second_summary), "--log", str(temporary / "second.log")]
        second = subprocess.run(second_command, capture_output=True, text=True)
        first_rows = [{key: value for key, value in row.items() if key != "generation_time"} for row in summary_rows(summary)]
        second_rows = [{key: value for key, value in row.items() if key != "generation_time"} for row in summary_rows(second_summary)] if second_summary.is_file() else []
        deterministic = second.returncode == 0 and first_rows == second_rows
        result("G17", "repeated graph metadata is deterministic", "identical except measured generation time", deterministic)
        result("G18", "summary CSV generated", str(len(rows)), summary.is_file() and len(rows) > 0 and {"records_available", "output_file", "generation_time"}.issubset(rows[0]))
        from graph_utils import exact_join

        joined, _ = exact_join([{"pid": "7", "timestamp": "t1"}], [{"pid": "7", "timestamp": "t1", "value": "ok"}], ("pid", "timestamp"))
        result("G19", "matching identifiers join", "one match", len(joined) == 1)
        not_joined, _ = exact_join([{"pid": "7", "timestamp": "t1"}], [{"pid": "8", "timestamp": "t1", "value": "wrong"}], ("pid", "timestamp"))
        result("G20", "incorrect identifiers do not join", "zero matches", len(not_joined) == 0)

        phase_root = temporary / "phase-output-root"
        for relative in ("results", "history", "logs", "state"):
            (phase_root / relative).mkdir(parents=True, exist_ok=True)
        shutil.copytree(ROOT / "tests/graph_inputs", phase_root / "tests/graph_inputs")
        for relative in ("benchmark_results.csv", "monitoring_results.csv", "monitoring_threads.csv", "classification_results.csv", "decision_results.csv", "feedback_results.csv"):
            shutil.copy2(ROOT / "results" / relative, phase_root / "results" / relative)
        shutil.copy2(ROOT / "state/application_state.csv", phase_root / "state/application_state.csv")
        validation_command = [str(ROOT / "bin/validation"), "--input", str(ROOT / "tests/validation_inputs/validation_fixtures.csv"), "--config", str(ROOT / "tests/validation_inputs/compaction.conf"), "--output", str(phase_root / "results/validation_results.csv"), "--history", str(phase_root / "history/validation.csv"), "--log", str(phase_root / "logs/validation.log")]
        migration_command = [str(ROOT / "bin/migration"), "--input", str(ROOT / "tests/migration_inputs/authorization.csv"), "--output", str(phase_root / "results/migration_results.csv"), "--history", str(phase_root / "history/migration.csv"), "--log", str(phase_root / "logs/migration.log"), "--state", str(phase_root / "state/migration.csv")]
        validation_run = subprocess.run(validation_command, capture_output=True, text=True)
        migration_run = subprocess.run(migration_command, capture_output=True, text=True)
        phase_summary = phase_root / "phase-summary.csv"
        phase_run = subprocess.run([sys.executable, str(SCRIPT), "--root", str(phase_root), "--output-dir", str(phase_root / "graphs"), "--summary", str(phase_summary), "--log", str(phase_root / "graph.log")], capture_output=True, text=True)
        phase_rows = {row["graph_id"]: row for row in summary_rows(phase_summary)} if phase_summary.is_file() else {}
        result("G21", "validation output visualized", "G06-1/G06-5 GENERATED", validation_run.returncode == 0 and phase_run.returncode == 0 and phase_rows.get("G06-1", {}).get("status") == "GENERATED" and phase_rows.get("G06-5", {}).get("status") == "GENERATED")
        result("G22", "migration output visualized", "G07-1/G07-3 GENERATED", migration_run.returncode == 0 and phase_run.returncode == 0 and phase_rows.get("G07-1", {}).get("status") == "GENERATED" and phase_rows.get("G07-3", {}).get("status") == "GENERATED")
        result("G23", "pipeline count graph generated", "G-C04 GENERATED", phase_rows.get("G-C04", {}).get("status") == "GENERATED")
        phase_svg_files = list((phase_root / "graphs").rglob("*.svg"))
        structurally_valid = bool(phase_svg_files) and all(ElementTree.parse(path).getroot().tag.endswith("svg") and path.stat().st_size > 0 for path in phase_svg_files)
        result("G24", "all phase-output SVGs structurally valid", str(len(phase_svg_files)), structurally_valid)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError:
        raise SystemExit(1)
