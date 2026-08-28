#!/usr/bin/env python3
"""Generate before/after Phase 4-6 comparison CSV and concise report."""

from __future__ import annotations

import csv
import statistics
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
BASELINE = "SUBPROCESS_SERIAL_BASELINE"
CANDIDATE = "PHASE4_IN_PROCESS"


def read(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream)) if path.is_file() else []


def value(rows: list[dict[str, str]], key: str) -> float | None:
    values = []
    for row in rows:
        try:
            values.append(float(row[key]))
        except (KeyError, ValueError):
            pass
    return statistics.mean(values) if values else None


def text(value: float | None) -> str:
    return "NA" if value is None else f"{value:.3f}"


def main() -> int:
    raw = read(RESULTS / "phase46_pipeline_profile_raw.csv")
    runs = read(RESULTS / "phase46_pipeline_profile_runs.csv")
    comparison = []
    for apps in (1, 2, 4, 8):
        before_phase = [row for row in raw if row.get("variant") == BASELINE and row.get("run_type") == "MEASURED" and row.get("application_count") == str(apps) and row.get("phase") == "phase4" and row.get("status") == "OK"]
        after_phase = [row for row in raw if row.get("variant") == CANDIDATE and row.get("run_type") == "MEASURED" and row.get("application_count") == str(apps) and row.get("phase") == "phase4" and row.get("status") == "OK"]
        before_runs = [row for row in runs if row.get("variant") == BASELINE and row.get("run_type") == "MEASURED" and row.get("application_count") == str(apps) and row.get("status") == "MEASURED"]
        after_runs = [row for row in runs if row.get("variant") == CANDIDATE and row.get("run_type") == "MEASURED" and row.get("application_count") == str(apps) and row.get("status") == "MEASURED"]
        before_serial, after_serial = value(before_phase, "serial_wait_us"), value(after_phase, "serial_wait_us")
        before_pipeline, after_pipeline = value(before_runs, "pipeline_total_us"), value(after_runs, "pipeline_total_us")
        before_throughput, after_throughput = value(before_runs, "aggregate_throughput"), value(after_runs, "aggregate_throughput")
        before_cpu, after_cpu = value(before_runs, "runtime_cpu_percent"), value(after_runs, "runtime_cpu_percent")
        before_phase4, after_phase4 = value(before_phase, "total_phase_us"), value(after_phase, "total_phase_us")
        comparison.append({"application_count": str(apps), "before_serial_wait_us": text(before_serial), "after_serial_wait_us": text(after_serial),
                           "before_pipeline_wall_us": text(before_pipeline), "after_pipeline_wall_us": text(after_pipeline),
                           "before_phase4_us": text(before_phase4), "after_phase4_us": text(after_phase4),
                           "before_aggregate_throughput": text(before_throughput), "after_aggregate_throughput": text(after_throughput),
                           "throughput_change_percent": text((after_throughput - before_throughput) / before_throughput * 100.0 if before_throughput and after_throughput else None),
                           "before_runtime_cpu_percent": text(before_cpu), "after_runtime_cpu_percent": text(after_cpu),
                           "status": "MEASURED" if before_pipeline is not None and after_pipeline is not None else "UNAVAILABLE"})
    fields = tuple(comparison[0])
    with (RESULTS / "phase46_pipeline_profile_comparison.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(comparison)
    audit = read(RESULTS / "phase46_pipeline_profile_concurrency_audit.csv")
    lines = ["# Phase 4-6 Pipeline Investigation", "", "## Concurrency Audit", "", "| Phase | State model | Current subprocess? | Thread-safe? | Optimization chosen |", "|---|---|---|---|---|"]
    for row in audit:
        phase = row["component"]
        subprocess = "yes" if phase.startswith("Phase") else "n/a"
        chosen = "experimental in-process API rejected by reprofile" if phase == "Phase 4" else row["optimization_chosen"]
        lines.append(f"| {phase} | {row['state_model']} | {subprocess} | {row['thread_safety']} | {chosen} |")
    lines.extend(["", "## Scaling Before/After", "", "| Apps | Before Serial Wait | After Serial Wait | Before Pipeline | After Pipeline | Throughput Change |", "|---:|---:|---:|---:|---:|---:|"])
    for row in comparison:
        lines.append(f"| {row['application_count']} | {row['before_serial_wait_us']} | {row['after_serial_wait_us']} | {row['before_pipeline_wall_us']} | {row['after_pipeline_wall_us']} | {row['throughput_change_percent']}% |")
    lines.extend(["", "## Decision", "", "The Phase 4 direct-call experiment removes its measured subprocess launch wall time, but the complete candidate reprofile regressed total pipeline wall time and aggregate throughput at multi-application scale. It is not the runtime default; subprocess mode remains production behavior, while the explicit in-process switch remains available only for investigation. Phase 5 and Phase 6 remain subprocesses, the coordinator remains serialized, and scheduler, worker-default, migration, and feedback safety are unchanged."])
    (RESULTS / "phase46_pipeline_report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("Phase46 comparison report generated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
