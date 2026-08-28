#!/usr/bin/env python3
"""Measure Phase 5/6 subprocess operations without changing runtime policy."""

from __future__ import annotations

import argparse
import os
import statistics
import tempfile
from collections import Counter

import run_phase46_pipeline_profile as phase46


RESULTS = phase46.RESULTS
APPS = (1, 2, 4, 8)
BASELINE = "SUBPROCESS_SERIAL_BASELINE"
RAW_FIELDS = ("run_id", "variant", "run_type", "run_index", "application_count", "worker_count",
              "app_id", "pid", "phase", "component", "operation_category", "duration_us", "status")
SUMMARY_FIELDS = ("variant", "application_count", "measurement_context", "phase", "component", "operation_category", "count",
                  "mean_us", "median_us", "min_us", "max_us", "stddev_us", "status")
PERSISTENCE_FIELDS = ("variant", "application_count", "phase", "component", "count", "mean_us",
                      "median_us", "share_of_phase56_profiled_percent", "status")
RUN_FIELDS = ("run_id", "variant", "run_type", "run_index", "application_count", "worker_count",
              "pipeline_total_us", "pipeline_service_rate_apps_sec", "aggregate_throughput",
              "runtime_cpu_percent", "runtime_rss_kb", "runtime_vms_kb", "status", "notes")


def number(value: str | None) -> float | None:
    try:
        return float(value) if value not in (None, "", "NA") else None
    except ValueError:
        return None


def text(value: float | None) -> str:
    return "NA" if value is None else f"{value:.3f}"


def phase_component(component: str) -> tuple[str, str] | None:
    if component == "phase5_child_api_total":
        return "phase5", "phase_envelope"
    if component == "phase6_child_validate_log":
        return "phase6", "phase_envelope"
    if component.startswith("p5_"):
        return "phase5", "persistence" if component in {
            "p5_state_path_prepare", "p5_state_load_or_initialize", "p5_output_history_write",
            "p5_history_cleanup", "p5_application_state_persist", "p5_logging",
        } else "compute"
    if component.startswith("p6_"):
        return "phase6", "persistence" if component in {
            "p6_output_history_write", "p6_human_log_write", "p6_history_cleanup",
        } else "logging_envelope" if component == "p6_logging_total" else "validation"
    return None


def rows_from_events(run_id: str, run_type: str, run_index: int, app_count: int,
                     events: list[dict[str, str]]) -> list[dict[str, str]]:
    output = []
    for event in events:
        classified = phase_component(event.get("component", ""))
        duration = number(event.get("duration_us"))
        if event.get("source") != "child" or classified is None or duration is None:
            continue
        phase, category = classified
        output.append({"run_id": run_id, "variant": BASELINE, "run_type": run_type,
                       "run_index": str(run_index), "application_count": str(app_count),
                       "worker_count": str(phase46.WORKERS), "app_id": event.get("application", "NA"),
                       "pid": event.get("pid", "-1"), "phase": phase,
                       "component": event["component"], "operation_category": category,
                       "duration_us": text(duration), "status": event.get("status", "UNKNOWN")})
    return output


def summary(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    result = []
    usable = [row for row in rows if row["run_type"] in ("MEASURED", "FIXTURE")]
    groups = {(row["variant"], row["application_count"], row["run_type"], row["phase"], row["component"], row["operation_category"])
              for row in usable}
    for group in sorted(groups, key=lambda value: (int(value[1]), value[2], value[3], value[4])):
        selected = [number(row["duration_us"]) for row in rows
                    if tuple(row[key] for key in ("variant", "application_count", "run_type", "phase", "component", "operation_category")) == group
                    and row["status"] == "OK"]
        values = [value for value in selected if value is not None]
        result.append({"variant": group[0], "application_count": group[1], "measurement_context": group[2],
                       "phase": group[3], "component": group[4], "operation_category": group[5], "count": str(len(values)),
                       "mean_us": text(statistics.mean(values) if values else None),
                       "median_us": text(statistics.median(values) if values else None),
                       "min_us": text(min(values) if values else None), "max_us": text(max(values) if values else None),
                       "stddev_us": text(statistics.stdev(values) if len(values) > 1 else None),
                       "status": "MEASURED" if values else "UNAVAILABLE"})
    return result


def persistence_breakdown(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    measured = [row for row in rows if row["run_type"] == "MEASURED" and row["status"] == "OK"]
    result = []
    groups = {(row["variant"], row["application_count"], row["phase"], row["component"]) for row in measured
              if row["operation_category"] == "persistence" and row["component"] != "p6_logging_total"}
    for variant, app_count, phase, component in sorted(groups, key=lambda value: (int(value[1]), value[2], value[3])):
        values = [number(row["duration_us"]) for row in measured
                  if (row["variant"], row["application_count"], row["phase"], row["component"]) == (variant, app_count, phase, component)]
        values = [value for value in values if value is not None]
        envelopes = [number(row["duration_us"]) for row in measured
                     if (row["variant"], row["application_count"], row["phase"], row["operation_category"]) ==
                     (variant, app_count, phase, "phase_envelope")]
        envelopes = [value for value in envelopes if value is not None]
        result.append({"variant": variant, "application_count": app_count, "phase": phase,
                       "component": component, "count": str(len(values)),
                       "mean_us": text(statistics.mean(values) if values else None),
                       "median_us": text(statistics.median(values) if values else None),
                       "share_of_phase56_profiled_percent": text(sum(values) / sum(envelopes) * 100.0 if envelopes else None),
                       "status": "MEASURED" if values else "UNAVAILABLE"})
    return result


def comparison() -> list[dict[str, str]]:
    return [{"application_count": str(count), "baseline_variant": BASELINE, "candidate_variant": "NA",
             "baseline_pipeline_wall_us": "NA", "candidate_pipeline_wall_us": "NA",
             "baseline_aggregate_throughput": "NA", "candidate_aggregate_throughput": "NA",
             "status": "NO_CANDIDATE", "notes": "Baseline-only investigation; no Phase 5/6 candidate introduced."}
            for count in APPS]


def operation_fixture_rows() -> list[dict[str, str]]:
    """Exercise valid Phase 5 and Phase 6 paths absent from the safety-first runtime data."""
    events: list[dict[str, str]] = []
    with tempfile.TemporaryDirectory(prefix="awavma-phase56-fixture-") as temporary:
        root = phase46.Path(temporary)
        decision_profile, validation_profile = root / "decision.profile.csv", root / "validation.profile.csv"
        environment = os.environ.copy()
        environment["AWAVMA_PROFILE_PATH"] = str(decision_profile)
        decision = phase46.subprocess.run([
            str(phase46.ROOT / "bin/phase46-profile/decision"), "--input",
            str(phase46.ROOT / "tests/decision_inputs/memory_wins.csv"), "--output", str(root / "decision.csv"),
            "--state-dir", str(root / "state"), "--history-dir", str(root / "history"), "--log", str(root / "decision.log"),
        ], cwd=phase46.ROOT, env=environment, capture_output=True, text=True)
        if decision.returncode:
            raise RuntimeError(f"Phase 5 operation fixture failed: {decision.stderr.strip()}")
        environment["AWAVMA_PROFILE_PATH"] = str(validation_profile)
        validation = phase46.subprocess.run([
            str(phase46.ROOT / "bin/phase46-profile/validation"), "--input",
            str(phase46.ROOT / "tests/validation_inputs/validation_fixtures.csv"), "--config",
            str(phase46.ROOT / "tests/validation_inputs/compaction.conf"), "--output", str(root / "validation.csv"),
            "--history", str(root / "validation-history.csv"), "--log", str(root / "validation.log"),
        ], cwd=phase46.ROOT, env=environment, capture_output=True, text=True)
        if validation.returncode:
            raise RuntimeError(f"Phase 6 operation fixture failed: {validation.stderr.strip()}")
        for profile in (decision_profile, validation_profile):
            events.extend({"source": "child", **row} for row in phase46.read_csv(profile))
    return rows_from_events("phase56-operation-fixture", "FIXTURE", 0, 0, events)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--measured", type=int, default=5)
    parser.add_argument("--timeout-seconds", type=float, default=30.0)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--summarize-only", action="store_true")
    args = parser.parse_args()
    if args.quick:
        args.warmups, args.measured = 0, 1
    RESULTS.mkdir(parents=True, exist_ok=True)
    phase46.VARIANT, phase46.PHASE4_MODE = BASELINE, "subprocess"
    if args.summarize_only:
        raw = phase46.read_csv(RESULTS / "phase56_subprocess_profile_raw.csv")
        for row in raw:
            classified = phase_component(row.get("component", ""))
            if classified is not None:
                row["phase"], row["operation_category"] = classified
        phase46.write_csv(RESULTS / "phase56_subprocess_profile_raw.csv", RAW_FIELDS, raw)
        phase46.write_csv(RESULTS / "phase56_subprocess_profile_summary.csv", SUMMARY_FIELDS, summary(raw))
        phase46.write_csv(RESULTS / "phase56_persistence_breakdown.csv", PERSISTENCE_FIELDS, persistence_breakdown(raw))
        phase46.write_csv(RESULTS / "phase56_candidate_comparison.csv", tuple(comparison()[0]), comparison())
        return 0
    build = phase46.subprocess.run(["make", "profile-awavma-runtime", "profile-phase46-binaries"], cwd=phase46.ROOT)
    if build.returncode:
        return build.returncode
    raw, runs = [], []
    for app_count in APPS:
        for index in range(1, args.warmups + args.measured + 1):
            run_type = "WARMUP" if index <= args.warmups else "MEASURED"
            run_id = f"phase56-{BASELINE}-a{app_count}-{run_type.lower()}-{index}"
            print(f"Phase56 run {run_id}", flush=True)
            _, events, run = phase46.run_one(run_id, run_type, index, app_count, args.timeout_seconds)
            raw.extend(rows_from_events(run_id, run_type, index, app_count, events))
            runs.append(run)
    raw.extend(operation_fixture_rows())
    phase46.write_csv(RESULTS / "phase56_subprocess_profile_raw.csv", RAW_FIELDS, raw)
    phase46.write_csv(RESULTS / "phase56_subprocess_profile_summary.csv", SUMMARY_FIELDS, summary(raw))
    phase46.write_csv(RESULTS / "phase56_persistence_breakdown.csv", PERSISTENCE_FIELDS, persistence_breakdown(raw))
    phase46.write_csv(RESULTS / "phase56_subprocess_profile_runs.csv", RUN_FIELDS, runs)
    phase46.write_csv(RESULTS / "phase56_candidate_comparison.csv",
                      tuple(comparison()[0]), comparison())
    counts = Counter(row["status"] for row in raw if row["run_type"] == "MEASURED")
    phase46.write_csv(RESULTS / "phase56_status_counts.csv", ("status", "count"),
                      [{"status": status, "count": str(count)} for status, count in sorted(counts.items())])
    print(f"Phase56 subprocess profile: raw_rows={len(raw)} runs={len(runs)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
