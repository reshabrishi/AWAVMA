#!/usr/bin/env python3
"""Create final integrated AWAVMA report and dependency-free SVG evidence."""

from __future__ import annotations

import csv
import math
import statistics
from pathlib import Path

from graph_utils import bar_chart, finite, validate_svg


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
GRAPHS = ROOT / "graphs/final-integrated-performance"
WORKLOADS = ("sequential", "random", "hot", "moderate", "cold", "mixed", "changing", "local")
APPS = (1, 2, 4, 8)


def read(name: str) -> list[dict[str, str]]:
    path = RESULTS / name
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream)) if path.is_file() else []


def text(value: float | None) -> str:
    return "NA" if value is None else f"{value:.3f}"


def no_data(path: Path, title: str, reason: str) -> bool:
    path.write_text("\n".join((
        '<svg xmlns="http://www.w3.org/2000/svg" width="1100" height="650">', '<rect width="100%" height="100%" fill="#fff"/>',
        f'<text x="60" y="50" font-family="Arial" font-size="24">{title}</text>',
        '<text x="550" y="300" text-anchor="middle" font-family="Arial" font-size="30">NO_DATA</text>',
        f'<text x="550" y="330" text-anchor="middle" font-family="Arial" font-size="14">{reason}</text>', '</svg>', "")), encoding="utf-8")
    return validate_svg(path)


def main() -> int:
    single = read("final_integrated_performance_summary.csv")
    overhead = read("final_integrated_performance_overhead.csv")
    scaling = read("final_integrated_multiapp_scaling.csv")
    multi_summary = read("final_integrated_multiapp_summary.csv")
    fairness = read("final_integrated_multiapp_fairness.csv")
    runs = read("final_integrated_multiapp_runs.csv")
    multi_raw = read("final_integrated_multiapp_raw.csv")
    multi_events = read("final_integrated_multiapp_profile_events.csv")
    statuses = read("final_integrated_status_counts.csv")
    regressions = read("final_integrated_regression.csv")
    GRAPHS.mkdir(parents=True, exist_ok=True)
    graph_rows = []

    def single_value(workload: str, mode: str, metric: str) -> float | None:
        row = next((item for item in single if item.get("workload") == workload and item.get("mode") == mode and item.get("metric") == metric and item.get("status") == "MEASURED"), None)
        return finite(row.get("mean")) if row else None

    def single_stat(workload: str, mode: str, metric: str, field: str) -> float | None:
        row = next((item for item in single if item.get("workload") == workload and item.get("mode") == mode and item.get("metric") == metric and item.get("status") == "MEASURED"), None)
        return finite(row.get(field)) if row else None

    def scale(mode: str, apps: int, metric: str) -> float | None:
        row = next((item for item in scaling if item.get("mode") == mode and item.get("application_count") == str(apps) and item.get("worker_count") == "2" and item.get("status") == "MEASURED"), None)
        return finite(row.get(metric)) if row else None

    def summary_value(scope: str, apps: int, mode: str, metric: str) -> float | None:
        row = next((item for item in multi_summary if item.get("scope") == scope and item.get("application_count") == str(apps)
                    and item.get("worker_count") == "2" and item.get("mode") == mode and item.get("metric") == metric and item.get("status") == "MEASURED"), None)
        return finite(row.get("mean")) if row else None

    def summary_stat(scope: str, apps: int, mode: str, metric: str, field: str) -> float | None:
        row = next((item for item in multi_summary if item.get("scope") == scope and item.get("application_count") == str(apps)
                    and item.get("worker_count") == "2" and item.get("mode") == mode and item.get("metric") == metric and item.get("status") == "MEASURED"), None)
        return finite(row.get(field)) if row else None

    def raw_stat(metric: str, apps: int = 8, median: bool = False) -> float | None:
        data = [finite(row.get(metric)) for row in multi_raw if row.get("run_type") == "MEASURED" and row.get("mode") == "INTEGRATED"
                and row.get("application_count") == str(apps) and row.get("worker_count") == "2" and row.get("status") == "MEASURED"]
        data = [value for value in data if value is not None]
        return (statistics.median(data) if median else sum(data) / len(data)) if data else None

    def event_mean(component: str, apps: int = 8) -> float | None:
        data = [finite(row.get("duration_us")) for row in multi_events if row.get("run_type") == "MEASURED" and row.get("mode") == "INTEGRATED"
                and row.get("application_count") == str(apps) and row.get("worker_count") == "2" and row.get("component") == component and row.get("status") == "OK"]
        data = [value for value in data if value is not None]
        return sum(data) / len(data) if data else None

    def graph(identifier: str, filename: str, title: str, labels: list[str], series: list[tuple[str, list[float | None]]], source: str, ylabel: str) -> None:
        path = GRAPHS / filename
        usable = [(name, [value if value is not None else math.nan for value in values]) for name, values in series]
        valid = labels and any(math.isfinite(value) for _, values in usable for value in values)
        ok = valid and bar_chart(path, title, "Workload" if len(labels) == len(WORKLOADS) else "Applications", ylabel, labels, usable, source, "REAL")
        if not ok:
            no_data(path, title, "finite measured values unavailable")
        graph_rows.append({"graph_id": identifier, "graph_name": filename, "status": "GENERATED" if ok else "NO_DATA",
                           "reason": "" if ok else "finite measured values unavailable", "output_file": str(path.relative_to(ROOT))})

    graph("FIP-G01", "baseline_vs_final_execution_time.svg", "Baseline vs Final AWAVMA Execution Time", list(WORKLOADS),
          [("baseline", [single_value(w, "BASELINE", "benchmark_elapsed_ms") for w in WORKLOADS]), ("final AWAVMA", [single_value(w, "INTEGRATED", "benchmark_elapsed_ms") for w in WORKLOADS])], "results/final_integrated_performance_summary.csv", "Mean milliseconds")
    graph("FIP-G02", "final_execution_overhead.svg", "Final Execution-Time Overhead", list(WORKLOADS),
          [("overhead", [finite(next((row.get("percent_change") for row in overhead if row.get("workload") == w and row.get("metric") == "benchmark_elapsed_ms"), None)) for w in WORKLOADS])], "results/final_integrated_performance_overhead.csv", "Percent; positive is slower")
    final_overheads = [finite(next((row.get("percent_change") for row in overhead if row.get("workload") == w and row.get("metric") == "benchmark_elapsed_ms"), None)) for w in WORKLOADS]
    stage1 = [21.74, 20.38, 13.68, 6.37, 29.17, 6.36, 3.74, 24.98]
    graph("FIP-G03", "historical_overhead_comparison.svg", "Historical Overhead Comparison", ["Stage 1", "Stage 2", "Stage 3"],
          [("observed overhead", [sum(stage1) / len(stage1), 6.411, sum(value for value in final_overheads if value is not None) / len([value for value in final_overheads if value is not None]) if any(value is not None for value in final_overheads) else None])], "results/final_integrated_performance_overhead.csv; documented historical stages", "Percent")
    graph("FIP-G04", "final_phase_latency_breakdown.svg", "Final Phase Latency Breakdown (8 Apps)", ["8 apps"],
          [(label, [summary_value("APPLICATION", 8, "INTEGRATED", metric)]) for label, metric in (("Phase 3", "mean_phase3_us"), ("Serial wait", "mean_pipeline_serial_wait_us"), ("Phase 4", "mean_phase4_us"), ("Phase 5", "mean_phase5_us"), ("Phase 6", "mean_phase6_us"))], "results/final_integrated_multiapp_summary.csv", "Mean microseconds")
    graph("FIP-G05", "discovery_contribution.svg", "Discovery Contribution", [str(app) for app in APPS],
          [("total discovery", [summary_value("RUN", app, "INTEGRATED", "discovery_total_us") for app in APPS])], "results/final_integrated_multiapp_summary.csv", "Mean microseconds per run")
    graph("FIP-G06", "serial_wait_vs_application_count.svg", "Serial Wait vs Application Count", [str(app) for app in APPS],
          [("serial wait", [scale("INTEGRATED", app, "mean_serial_wait_us") for app in APPS])], "results/final_integrated_multiapp_scaling.csv", "Mean microseconds")
    graph("FIP-G07", "queue_wait_vs_application_count.svg", "Queue Wait vs Application Count", [str(app) for app in APPS],
          [("queue wait", [scale("INTEGRATED", app, "mean_queue_wait_us") for app in APPS])], "results/final_integrated_multiapp_scaling.csv", "Mean microseconds")
    graph("FIP-G08", "aggregate_throughput_vs_application_count.svg", "Aggregate Throughput vs Application Count", [str(app) for app in APPS],
          [("baseline", [scale("BASELINE", app, "aggregate_throughput") for app in APPS]), ("final AWAVMA", [scale("INTEGRATED", app, "aggregate_throughput") for app in APPS])], "results/final_integrated_multiapp_scaling.csv", "Operations per second")
    graph("FIP-G09", "runtime_cpu_vs_application_count.svg", "Runtime CPU vs Application Count", [str(app) for app in APPS],
          [("runtime CPU", [scale("INTEGRATED", app, "runtime_cpu_percent") for app in APPS])], "results/final_integrated_multiapp_scaling.csv", "Mean percent")
    graph("FIP-G10", "runtime_rss_vs_application_count.svg", "Runtime RSS vs Application Count", [str(app) for app in APPS],
          [("runtime RSS", [scale("INTEGRATED", app, "runtime_rss_kb") for app in APPS])], "results/final_integrated_multiapp_scaling.csv", "Mean KiB")
    graph("FIP-G11", "fairness_vs_application_count.svg", "Fairness vs Application Count", [str(app) for app in APPS],
          [("Jain index", [scale("INTEGRATED", app, "fairness_index") for app in APPS])], "results/final_integrated_multiapp_scaling.csv", "Jain index")
    graph("FIP-G12", "accepted_vs_rejected_optimization_summary.svg", "Accepted vs Rejected Optimization Summary", ["discovery accepted", "Phase 4 rejected"],
          [("percent effect", [65.4, -26.017])], "controlled cadence study; Phase 4 candidate comparison", "Percent")
    write_path = RESULTS / "final_integrated_performance_graph_summary.csv"
    with write_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=("graph_id", "graph_name", "status", "reason", "output_file"))
        writer.writeheader()
        writer.writerows(graph_rows)

    lines = ["# Final Integrated Runtime Performance Report", "", "## A. Exact Production Configuration", "",
             "- Phase 4/5/6: subprocess; coordinator: serialized; scheduler: FIFO; workers: 2.",
             "- Discovery: 250 ms; monitor: 100 ms; evaluation: 1000 ms; Phase 4 in-process mode was not used.",
             "", "## B. Methodology", "", "- Single app: eight workloads, 2 threads, 8 MiB, 1,000,000 iterations, seed 12345; five warm-ups and five deterministic interleaved baseline/integrated pairs.",
             "- Multi-app: mixed, one thread/app, 8 MiB/app, fixed calibrated 17,841,231 iterations/thread; 1/2/4/8 apps, two workers, three warm-ups and five deterministic interleaved pairs.",
             "- Values are observed measurements, not statistical-significance claims. The short single-app workload can legitimately leave some 1000-ms evaluation/pipeline fields unavailable.",
             "", "## C. Regression Results", "", "| Command | Result | Notes |", "|---|---|---|"]
    if regressions:
        for row in regressions:
            lines.append(f"| {row.get('command', 'NA')} | {row.get('result', 'NA')} | {row.get('notes', '')} |")
    else:
        lines.append("| Pending | NOT RUN | Run `scripts/record_final_regression.py` before final publication. |")
    lines.extend([
        "", "## D. Single-App Results", "", "| Workload | Baseline Mean ms | Final AWAVMA Mean ms | Overhead % | Final Median ms | Final Stddev ms | Throughput Change % |", "|---|---:|---:|---:|---:|---:|---:|",
    ])
    for workload in WORKLOADS:
        elapsed = next((row for row in overhead if row.get("workload") == workload and row.get("metric") == "benchmark_elapsed_ms"), {})
        throughput = next((row for row in overhead if row.get("workload") == workload and row.get("metric") == "benchmark_throughput_ops_sec"), {})
        lines.append(f"| {workload} | {elapsed.get('baseline_mean', 'NA')} | {elapsed.get('integrated_mean', 'NA')} | {elapsed.get('percent_change', 'NA')} | {text(single_stat(workload, 'INTEGRATED', 'benchmark_elapsed_ms', 'median'))} | {text(single_stat(workload, 'INTEGRATED', 'benchmark_elapsed_ms', 'stddev'))} | {throughput.get('percent_change', 'NA')} |")
    lines.extend(["", "## E. Multi-App Results", "", "| Apps | Aggregate Baseline Throughput | AWAVMA Throughput | Change % | Runtime CPU | Serial Wait us | Queue Wait us |", "|---:|---:|---:|---:|---:|---:|---:|"])
    for app in APPS:
        before, after = scale("BASELINE", app, "aggregate_throughput"), scale("INTEGRATED", app, "aggregate_throughput")
        change = (after - before) / before * 100.0 if before and after is not None else None
        lines.append(f"| {app} | {text(before)} | {text(after)} | {text(change)} | {text(scale('INTEGRATED', app, 'runtime_cpu_percent'))} | {text(scale('INTEGRATED', app, 'mean_serial_wait_us'))} | {text(scale('INTEGRATED', app, 'mean_queue_wait_us'))} |")
    lines.extend(["", "## Final Fairness", "", "| Apps | Jain Index | Min Cycles/App | Max Cycles/App | Worst Evaluation Gap ms | Starvation? |", "|---:|---:|---:|---:|---:|---|"])
    for app in APPS:
        selected = [row for row in fairness if row.get("application_count") == str(app) and row.get("worker_count") == "2" and "mode=INTEGRATED" in row.get("configuration", "")]
        cycles = [finite(row.get("completed_cycles")) for row in selected]; cycles = [value for value in cycles if value is not None]
        gaps = [finite(row.get("max_evaluation_interval_ms")) for row in selected]; gaps = [value for value in gaps if value is not None]
        starvation = "POTENTIAL" if any(row.get("starvation_evidence") == "POTENTIAL" for row in selected) else "NO EVIDENCE"
        lines.append(f"| {app} | {text(scale('INTEGRATED', app, 'fairness_index'))} | {text(min(cycles) if cycles else None)} | {text(max(cycles) if cycles else None)} | {text(max(gaps) if gaps else None)} | {starvation} |")
    lines.extend(["", "## F. Resource Usage", "", "| Apps | CPU Mean % | CPU Peak % | RSS Mean KiB | RSS Peak KiB | VMS Mean KiB | VMS Peak KiB |", "|---:|---:|---:|---:|---:|---:|---:|"])
    for app in APPS:
        selected = [row for row in runs if row.get("run_type") == "MEASURED" and row.get("mode") == "INTEGRATED" and row.get("application_count") == str(app) and row.get("status") == "MEASURED"]
        def run_stat(metric: str, maximum: bool = False) -> float | None:
            data = [finite(row.get(metric)) for row in selected]; data = [value for value in data if value is not None]
            return max(data) if maximum and data else (sum(data) / len(data) if data else None)
        lines.append(f"| {app} | {text(run_stat('runtime_cpu_percent'))} | {text(run_stat('runtime_cpu_peak_percent', True))} | {text(run_stat('runtime_rss_kb'))} | {text(run_stat('runtime_rss_kb', True))} | {text(run_stat('runtime_vms_kb'))} | {text(run_stat('runtime_vms_kb', True))} |")
    lines.extend(["", "## G. Pipeline Breakdown", "", "| Component | Mean us | Median us | Notes |", "|---|---:|---:|---|"])
    coordinator_total = event_mean("coordinator_application_total")
    phase4, phase5, phase6 = (summary_value("APPLICATION", 8, "INTEGRATED", metric) for metric in ("mean_phase4_us", "mean_phase5_us", "mean_phase6_us"))
    coordinator_overhead = max(0.0, coordinator_total - sum(value or 0.0 for value in (phase4, phase5, phase6))) if coordinator_total is not None else None
    pipeline_rows = (("Discovery", summary_value("RUN", 8, "INTEGRATED", "discovery_total_us"), summary_stat("RUN", 8, "INTEGRATED", "discovery_total_us", "median"), "Run-level total; not additive with per-scan work."),
                     ("Queue wait", summary_value("APPLICATION", 8, "INTEGRATED", "mean_queue_wait_us"), summary_stat("APPLICATION", 8, "INTEGRATED", "mean_queue_wait_us", "median"), "Queue-to-worker delay."),
                     ("Worker execution", raw_stat("mean_worker_execution_us"), raw_stat("mean_worker_execution_us", median=True), "Worker wrapper, includes Phase 3."),
                     ("Phase 3", summary_value("APPLICATION", 8, "INTEGRATED", "mean_phase3_us"), summary_stat("APPLICATION", 8, "INTEGRATED", "mean_phase3_us", "median"), "Monitor execution."),
                     ("Serial-entry wait", summary_value("APPLICATION", 8, "INTEGRATED", "mean_pipeline_serial_wait_us"), summary_stat("APPLICATION", 8, "INTEGRATED", "mean_pipeline_serial_wait_us", "median"), "Serialized coordinator admission."),
                     ("Phase 4 subprocess wall", phase4, summary_stat("APPLICATION", 8, "INTEGRATED", "mean_phase4_us", "median"), "Parent wall time."), ("Phase 5 subprocess wall", phase5, summary_stat("APPLICATION", 8, "INTEGRATED", "mean_phase5_us", "median"), "Parent wall time."),
                     ("Phase 6 subprocess wall", phase6, summary_stat("APPLICATION", 8, "INTEGRATED", "mean_phase6_us", "median"), "Parent wall time."), ("Coordinator overhead", coordinator_overhead, None, "Coordinator wrapper minus Phase 4-6 means."),
                     ("Completion latency", summary_value("APPLICATION", 8, "INTEGRATED", "mean_completion_latency_us"), summary_stat("APPLICATION", 8, "INTEGRATED", "mean_completion_latency_us", "median"), "Submit to completed job."))
    for label, mean_value, median_value, note in pipeline_rows:
        lines.append(f"| {label} | {text(mean_value)} | {text(median_value)} | {note} Nested scopes are not additive. |")
    discovery_runs = [row for row in runs if row.get("run_type") == "MEASURED" and row.get("mode") == "INTEGRATED" and row.get("application_count") == "8" and row.get("status") == "MEASURED"]
    scan_counts = [finite(row.get("discovery_scan_count")) for row in discovery_runs]; scan_counts = [value for value in scan_counts if value is not None]
    discovery_totals = [finite(row.get("discovery_total_us")) for row in discovery_runs]; discovery_totals = [value for value in discovery_totals if value is not None]
    scan_mean = sum(discovery_totals) / sum(scan_counts) if scan_counts and sum(scan_counts) else None
    cycle_totals = [finite(row.get("duration_us")) for row in multi_events if row.get("run_type") == "MEASURED" and row.get("mode") == "INTEGRATED" and row.get("application_count") == "8" and row.get("worker_count") == "2" and row.get("component") == "runtime_cycle_total"]
    cycle_totals = [value for value in cycle_totals if value is not None]
    discovery_contribution = sum(discovery_totals) / sum(cycle_totals) * 100.0 if discovery_totals and cycle_totals else None
    lines.extend(["", "## H. Discovery Results", "", "- Production discovery remains 250 ms. The earlier controlled cadence study measured 829.2 to 286.6 ms/run total discovery time (65.4% reduction), controlled CPU 68.3% to 26.5%, and 249.4 ms mean detection latency. It is retained separately from this short-run final measurement.",
                  f"- Final 8-app runs: mean scans/run {text(sum(scan_counts) / len(scan_counts) if scan_counts else None)}, mean scan time {text(scan_mean)} us, mean total discovery time/run {text(sum(discovery_totals) / len(discovery_totals) if discovery_totals else None)} us, and {text(discovery_contribution)}% of summed runtime-cycle wall time (non-additive attribution).",
                  "", "## I. Phase 4-6 Findings", "", "- Production remains serialized subprocess execution. Parent subprocess wall minus child API work is termed subprocess startup/uninstrumented residual, not pure fork cost.", "- Completed Phase 5/6 investigation at eight apps: Phase 5 child envelope about 4691 us, dominated by persistence; Phase 6 child envelope about 371 us, dominated by logging/lifecycle; controlled valid gate compute is sub-microsecond.",
                  "", "## J. Accepted Optimizations", "", "- Discovery cadence of 250 ms is accepted. The bounded two-worker FIFO Phase 3 pool is retained.",
                  "", "## K. Rejected Optimizations", "", "- Phase 4 in-process direct call: local Phase 4 work improved about 12.6 ms to 0.9 ms, but whole-pipeline throughput changed -7.332%, -23.822%, -21.803%, and -26.017% at 1/2/4/8 apps. REJECTED FOR PRODUCTION.",
                  "", "## L. Scheduler/Worker Conclusion", "", "- Two workers remain the default. Final queue/fairness evidence is below; Priority + Aging + FIFO remains unjustified absent saturation, deferrals, or starvation.", "", "| Apps | Max Queue Depth | Deferrals | Saturation | Max Queue Wait us |", "|---:|---:|---:|---:|---:|"])
    for app in APPS:
        lines.append(f"| {app} | {text(scale('INTEGRATED', app, 'max_queue_depth'))} | {text(scale('INTEGRATED', app, 'deferred_jobs'))} | {text(scale('INTEGRATED', app, 'saturation_events'))} | {text(scale('INTEGRATED', app, 'max_queue_wait_us'))} |")
    lines.extend([
                  "", "## M. Runtime Outcome Counts", "", "| Outcome | Count | Status |", "|---|---:|---|"])
    for row in statuses:
        lines.append(f"| {row['outcome']} | {row['count']} | {row['status']} |")
    lines.extend(["", "## N. Environment Limitations", "", "- CPUs: 8; NUMA nodes: node0 only; libnuma available; numa.h and numaif.h unavailable.", "- Remote NUMA migration was not demonstrated; perf/cache counters are permission-limited; the benchmark binary is frozen because benchmark source needs unavailable numa.h.",
                  "", "## O. Bottleneck Ranking", ""])
    ranked = [(label, value) for label, value, _, _ in pipeline_rows if label != "Discovery" and value is not None]
    ranked.sort(key=lambda item: item[1], reverse=True)
    for index, (label, value) in enumerate(ranked, 1):
        lines.append(f"{index}. {label}: {text(value)} us (final 8-app production measurement).")
    lines.extend([
                  "", "## P. Final Phase 10 Runtime Conclusion", "", "The accepted AWAVMA runtime overhead and scalability were characterized on the current single-NUMA-node environment. Comparative benefit over conventional multi-node NUMA placement remains to be evaluated on suitable 2+ node hardware."])
    (RESULTS / "final_integrated_performance_report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    pooled = [value for value in final_overheads if value is not None]
    phase10 = [
        "# Phase 10 Final Runtime Checkpoint", "",
        "This report consolidates the original Phase 10 launcher/serialized measurement, monitoring profiling, discovery cadence optimization, integrated runtime, multi-app scaling, Phase 4-6 and Phase 5/6 investigations, and the final accepted re-profile.",
        "", "## Historical Stages", "",
        "- Stage 1: original launcher/serialized measurement reported sequential +21.74%, random +20.38%, hot +13.68%, moderate +6.37%, cold +29.17%, mixed +6.36%, changing +3.74%, local +24.98%. It was not the full production runtime.",
        "- Stage 2: first integrated runtime pooled observed overhead was +6.411% before the accepted sequence was consolidated.",
        f"- Stage 3: final accepted production re-profile observed an unweighted mean workload overhead of {text(sum(pooled) / len(pooled) if pooled else None)}%. It is reported as observed, not a significance claim; methodologies are not treated as directly causal.",
        "", "## Accepted Architecture", "",
        "- Production remains Phase 4/5/6 subprocesses, a serialized coordinator, FIFO scheduling, two Phase 3 workers, monitor interval 100 ms, evaluation interval 1000 ms, and discovery interval 250 ms.",
        "- Discovery cadence is the accepted optimization: the separate controlled study reduced total discovery time from 829.2 to 286.6 ms/run (65.4%) with 249.4 ms mean detection latency. The final run retains it without conflating methodologies.",
        "", "## Phase Investigations", "",
        "- Phase 4 in-process direct call was rejected for production: local work fell about 12.6 ms to 0.9 ms, while whole-pipeline throughput regressed -7.332%, -23.822%, -21.803%, and -26.017% at 1/2/4/8 apps.",
        "- Phase 5/6 remain subprocesses. At eight apps, Phase 5 child work was about 4691 us and persistence-dominated; Phase 6 child work was about 371 us and logging/lifecycle-dominated; controlled valid gate compute was sub-microsecond.",
        "", "## Final Evidence", "",
        "- Final source data: `final_integrated_performance_raw.csv`, `final_integrated_multiapp_raw.csv`, final summaries, graph manifest, and regression ledger.",
        "- Final report: `final_integrated_performance_report.md`, including resource cost, fairness, queue/scheduler evidence, pipeline attribution, outcome counts, and bottleneck ranking.",
        "- Phase 7 was not enabled without legitimate migration metadata. Phase 8 was not executed without valid later before/after observations.",
        "", "## Boundary", "",
        "The accepted AWAVMA runtime overhead and scalability were characterized on the current one-node NUMA environment. No comparative NUMA-placement or remote-locality claim is made; that work requires suitable 2+ node hardware and a traditional NUMA baseline.",
    ]
    (RESULTS / "phase10_final_runtime_report.md").write_text("\n".join(phase10) + "\n", encoding="utf-8")
    print(f"Final integrated graphs: generated={sum(row['status'] == 'GENERATED' for row in graph_rows)} no_data={sum(row['status'] == 'NO_DATA' for row in graph_rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
