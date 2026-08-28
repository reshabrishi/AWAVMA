#!/usr/bin/env python3
"""Write the Phase 5/6 baseline investigation report from measured CSV files."""

from __future__ import annotations

import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"


def read(name: str) -> list[dict[str, str]]:
    path = RESULTS / name
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream)) if path.is_file() else []


def main() -> int:
    summary = read("phase56_subprocess_profile_summary.csv")
    persistence = read("phase56_persistence_breakdown.csv")
    comparison = read("phase56_candidate_comparison.csv")
    def mean(apps: str, phase: str, component: str) -> str:
        row = next((item for item in summary if item.get("application_count") == apps and item.get("phase") == phase
                    and item.get("component") == component and item.get("measurement_context") == "MEASURED"), None)
        return row.get("mean_us", "NA") if row else "NA"

    phase5_eight = mean("8", "phase5", "phase5_child_api_total")
    phase6_eight = mean("8", "phase6", "phase6_child_validate_log")
    lines = [
        "# Phase 5/6 Subprocess and Persistence Investigation",
        "",
        "## Methodology",
        "",
        "- Production subprocess Phase 4/5/6 path; Phase 4 explicitly uses `subprocess` mode.",
        "- Application counts: 1, 2, 4, 8; workers: 2; discovery interval: 250 ms.",
        "- Workload: calibrated 17,841,231 iterations/thread, one thread/app, 8 MiB/app.",
        "- Three warm-ups and five measured runs per application count.",
        "- Paired/interleaved ordering is not applicable: no candidate cleared the baseline-only optimization gate.",
        "- Measurements are monotonic child-process scopes emitted under `AWAVMA_PROFILE`; no synthetic values.",
        "- A controlled valid-input fixture records Phase 5 computation and all Phase 6 gates that the safety-first runtime workload legitimately short-circuits.",
        "",
        "## Operation Timing",
        "",
        "| Context | Apps | Phase | Operation | Mean us | Median us | Samples |",
        "|---|---:|---|---|---:|---:|---:|",
    ]
    for row in sorted((row for row in summary if row.get("status") == "MEASURED"),
                      key=lambda item: (int(item["application_count"]), item["phase"], item["component"])):
        lines.append(f"| {row.get('measurement_context', 'MEASURED')} | {row['application_count']} | {row['phase']} | {row['component']} | {row['mean_us']} | {row['median_us']} | {row['count']} |")
    lines.extend([
        "",
        "## Persistence Breakdown",
        "",
        "Shares use the matching phase child envelope, so nested Phase 6 logging scopes are not double-counted as independent totals.",
        "",
        "| Apps | Phase | Persistence operation | Mean us | Envelope share |",
        "|---:|---|---|---:|---:|",
    ])
    for row in sorted((row for row in persistence if row.get("status") == "MEASURED"),
                      key=lambda item: (int(item["application_count"]), item["phase"], item["component"])):
        lines.append(f"| {row['application_count']} | {row['phase']} | {row['component']} | {row['mean_us']} | {row['share_of_phase56_profiled_percent']}% |")
    lines.extend([
        "",
        "## Safety Classification",
        "",
        "| Phase | Classification | Reason | Decision |",
        "|---|---|---|---|",
        "| Phase 5 | PER_APPLICATION_SAFE with isolated paths | State/history/log/output must remain isolated by app_id; same-tree writes are unsafe. | Retain subprocess baseline; no concurrency change. |",
        "| Phase 6 | SERIALIZED_REQUIRED in-process | `Validation_Init`, active configuration, and logger state are process-global. | Retain subprocess isolation and serialized coordinator. |",
        "",
        "## Candidate Decision",
        "",
        f"No Phase 5/6 candidate was introduced. At eight apps, Phase 5's child envelope averages {phase5_eight} us and Phase 6's averages {phase6_eight} us; Phase 5 state/history/output operations and Phase 6 logging dominate their respective profiled child work, while the valid-input Phase 6 gates are sub-microsecond. Reducing those costs safely would require a persistence or lifecycle semantic change, so the data does not justify an in-process or concurrency candidate. A future candidate requires a design that preserves per-app persistence isolation, Phase 6 lifecycle semantics, and whole-system throughput.",
        "",
        "| Apps | Comparison status | Notes |",
        "|---:|---|---|",
    ])
    for row in comparison:
        lines.append(f"| {row['application_count']} | {row['status']} | {row['notes']} |")
    lines.extend([
        "",
        "## Integrity",
        "",
        "No scheduler, worker default, Phase 4 production mode, migration execution, feedback behavior, persistence format, or validation gate policy changed for this investigation.",
        "",
    ])
    (RESULTS / "phase56_investigation_report.md").write_text("\n".join(lines), encoding="utf-8")
    print("Phase56 investigation report generated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
