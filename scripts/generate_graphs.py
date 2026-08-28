#!/usr/bin/env python3
"""Generate Phase 9 research graphics from AWAVMA CSV outputs.

The default backend is dependency-free SVG. It intentionally does not import
pandas, numpy, or matplotlib so graph generation remains usable on the current
host; those packages are optional for downstream PNG/PDF conversion.
"""

from __future__ import annotations

import argparse
import csv
import logging
import math
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

from graph_utils import bar_chart, dataset_type, exact_join, finite, line_chart, read_csv, safe_filename


FACTOR_NAMES = ("ACCESS", "THRESHOLD", "GAIN", "COST", "CPU", "SHARING")
DECISIONS = ("MOVE_MEMORY", "MOVE_THREAD", "NO_MIGRATION", "INSUFFICIENT_DECISION_SIGNAL")


class Reports:
    def __init__(self, root: Path, logger: logging.Logger):
        self.root = root
        self.logger = logger
        self.rows: list[dict[str, object]] = []

    def add(self, graph_id: str, name: str, source: str, used: int, skipped: int, data_type: str, status: str, reason: str = "", records_available: int | None = None, output_file: str = "", generation_time: float = 0.0) -> None:
        if records_available is None:
            records_available = used + skipped
        self.rows.append({
            "graph_id": graph_id,
            "graph_name": name,
            "source_file": source,
            "records_available": records_available,
            "records_used": used,
            "records_skipped": skipped,
            "data_type": data_type,
            "status": status,
            "reason": reason,
            "output_file": output_file,
            "generation_time": f"{generation_time:.9f}",
        })
        self.logger.info("graph=%s source=%s available=%d used=%d skipped=%d status=%s output=%s generation_time=%.9f reason=%s", graph_id, source, records_available, used, skipped, status, output_file, generation_time, reason)

    def write(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=("graph_id", "graph_name", "source_file", "data_type", "records_available", "records_used", "records_skipped", "status", "reason", "output_file", "generation_time"))
            writer.writeheader()
            writer.writerows(self.rows)


def source_name(root: Path, path: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def load(root: Path, relative: str, required: tuple[str, ...], reports: Reports, data_type: str = "REAL") -> tuple[list[dict[str, str]], int, str, str]:
    path = root / relative
    rows, skipped, reason = read_csv(path, required)
    source = source_name(root, path)
    if reason or not rows:
        reports.logger.info("input=%s rows=%d skipped=%d reason=%s", source, len(rows), skipped, reason or "no data")
    return rows, skipped, source, data_type


def valid_pairs(rows: list[dict[str, str]], x_column: str, y_column: str) -> tuple[list[tuple[float, float]], int]:
    values: list[tuple[float, float]] = []
    skipped = 0
    for row in rows:
        x = finite(row.get(x_column))
        y = finite(row.get(y_column))
        if x is None or y is None:
            skipped += 1
            continue
        values.append((x, y))
    return values, skipped


def grouped(rows: list[dict[str, str]], category_column: str, value_column: str) -> tuple[list[str], list[float], int]:
    values: dict[str, list[float]] = defaultdict(list)
    skipped = 0
    for row in rows:
        category = label(row.get(category_column))
        value = finite(row.get(value_column))
        if value is None:
            skipped += 1
            continue
        values[category].append(value)
    categories = sorted(values)
    means = [sum(values[key]) / len(values[key]) for key in categories]
    return categories, means, skipped


def label(value: str | None, fallback: str = "UNKNOWN") -> str:
    text = "" if value is None else str(value).strip()
    return text or fallback


def emit_line(reports: Reports, output: Path, graph_id: str, name: str, source: str, data_type: str, series: list[tuple[str, list[tuple[float, float]]]], x_label: str, y_label: str, used: int, skipped: int, title_suffix: str = "") -> None:
    title = name + (f" — {title_suffix}" if title_suffix else "")
    if not series or not any(series_values for _, series_values in series):
        reports.add(graph_id, name, source, 0, skipped, data_type, "NO_DATA", "no finite plottable values")
        return
    started = time.monotonic()
    if line_chart(output, title, x_label, y_label, series, source, data_type):
        reports.add(graph_id, name, source, used, skipped, data_type, "GENERATED", "", output_file=source_name(reports.root, output), generation_time=time.monotonic() - started)
    else:
        reports.add(graph_id, name, source, 0, skipped, data_type, "INVALID_INPUT", "SVG validation failed", output_file=source_name(reports.root, output), generation_time=time.monotonic() - started)


def emit_bar(reports: Reports, output: Path, graph_id: str, name: str, source: str, data_type: str, categories: list[str], series: list[tuple[str, list[float]]], x_label: str, y_label: str, used: int, skipped: int, title_suffix: str = "") -> None:
    title = name + (f" — {title_suffix}" if title_suffix else "")
    if not categories or not any(math.isfinite(value) for _, values in series for value in values):
        reports.add(graph_id, name, source, 0, skipped, data_type, "NO_DATA", "no finite plottable values")
        return
    started = time.monotonic()
    if bar_chart(output, title, x_label, y_label, categories, series, source, data_type):
        reports.add(graph_id, name, source, used, skipped, data_type, "GENERATED", "", output_file=source_name(reports.root, output), generation_time=time.monotonic() - started)
    else:
        reports.add(graph_id, name, source, 0, skipped, data_type, "INVALID_INPUT", "SVG validation failed", output_file=source_name(reports.root, output), generation_time=time.monotonic() - started)


def count_rows(root: Path, relative: str) -> int:
    rows, _, _ = read_csv(root / relative)
    return len(rows)


def generate_phase2(root: Path, graph_root: Path, reports: Reports) -> None:
    rows, skipped, source, data_type = load(root, "results/benchmark_results.csv", ("pattern", "threads", "memory_mb", "execution_time_sec", "throughput_ops_sec", "operations"), reports)
    categories, throughput, group_skipped = grouped(rows, "pattern", "throughput_ops_sec")
    emit_bar(reports, graph_root / "phase2/benchmark_throughput_by_pattern.svg", "G02-1", "Benchmark Throughput by Workload Pattern", source, data_type, categories, [("throughput_ops_sec", throughput)], "Workload pattern", "Throughput (operations/sec)", len(throughput), skipped + group_skipped)
    categories, execution, group_skipped = grouped(rows, "pattern", "execution_time_sec")
    emit_bar(reports, graph_root / "phase2/benchmark_execution_time.svg", "G02-2", "Benchmark Execution Time by Workload Pattern", source, data_type, categories, [("execution_time_sec", execution)], "Workload pattern", "Execution time (sec)", len(execution), skipped + group_skipped)
    threads = sorted({finite(row.get("threads")) for row in rows if finite(row.get("threads")) is not None})
    series = [(label(row.get("pattern")), [(finite(row.get("threads")), finite(row.get("throughput_ops_sec"))) for row in rows if finite(row.get("threads")) is not None and finite(row.get("throughput_ops_sec")) is not None]) for row in rows]
    by_pattern: dict[str, list[tuple[float, float]]] = defaultdict(list)
    for row in rows:
        x, y = finite(row.get("threads")), finite(row.get("throughput_ops_sec"))
        if x is not None and y is not None:
            by_pattern[label(row.get("pattern"))].append((x, y))
    if len(threads) > 1:
        emit_line(reports, graph_root / "phase2/benchmark_throughput_vs_threads.svg", "G02-3", "Benchmark Throughput vs Thread Count", source, data_type, sorted(by_pattern.items()), "Thread count", "Throughput (operations/sec)", sum(len(values) for values in by_pattern.values()), skipped)
    else:
        reports.add("G02-3", "Benchmark Throughput vs Thread Count", source, 0, len(rows), data_type, "SKIPPED", "only one thread count is available", records_available=len(rows))
    memories = sorted({finite(row.get("memory_mb")) for row in rows if finite(row.get("memory_mb")) is not None})
    by_pattern_time: dict[str, list[tuple[float, float]]] = defaultdict(list)
    for row in rows:
        x, y = finite(row.get("memory_mb")), finite(row.get("execution_time_sec"))
        if x is not None and y is not None:
            by_pattern_time[label(row.get("pattern"))].append((x, y))
    if len(memories) > 1:
        emit_line(reports, graph_root / "phase2/benchmark_execution_vs_memory.svg", "G02-4", "Benchmark Execution Time vs Memory Size", source, data_type, sorted(by_pattern_time.items()), "Memory size (MB)", "Execution time (sec)", sum(len(values) for values in by_pattern_time.values()), skipped)
    else:
        reports.add("G02-4", "Benchmark Execution Time vs Memory Size", source, 0, len(rows), data_type, "SKIPPED", "only one memory size is available", records_available=len(rows))
    categories, operations, group_skipped = grouped(rows, "pattern", "operations")
    emit_bar(reports, graph_root / "phase2/workload_pattern_comparison.svg", "G02-5", "Workload Pattern Operation Comparison", source, data_type, categories, [("operations", operations)], "Workload pattern", "Operations", len(operations), skipped + group_skipped)


def generate_phase3(root: Path, graph_root: Path, reports: Reports) -> None:
    rows, skipped, source, data_type = load(root, "results/monitoring_results.csv", ("elapsed_ms", "process_cpu_utilization_percent", "rss_kb", "vms_kb", "minor_page_faults", "major_page_faults", "thread_count", "process_numa_pages"), reports)
    for graph_id, filename, name, column, y_label in (
        ("G03-1", "cpu_utilization_over_time.svg", "CPU Utilization over Time", "process_cpu_utilization_percent", "Process CPU utilization (%)"),
        ("G03-2", "rss_over_time.svg", "RSS over Time", "rss_kb", "RSS (KB)"),
        ("G03-3", "virtual_memory_over_time.svg", "Virtual Memory over Time", "vms_kb", "Virtual memory (KB)"),
        ("G03-6", "thread_count_over_time.svg", "Thread Count over Time", "thread_count", "Threads"),
    ):
        pairs, pair_skipped = valid_pairs(rows, "elapsed_ms", column)
        emit_line(reports, graph_root / f"phase3/{filename}", graph_id, name, source, data_type, [(column, pairs)], "Elapsed time (ms)", y_label, len(pairs), skipped + pair_skipped)
    fault_series = []
    used = 0
    for column in ("minor_page_faults", "major_page_faults"):
        pairs, pair_skipped = valid_pairs(rows, "elapsed_ms", column)
        used = max(used, len(pairs))
        fault_series.append((column, pairs))
        skipped += pair_skipped
    emit_line(reports, graph_root / "phase3/page_faults_over_time.svg", "G03-4", "Page Faults over Time", source, data_type, fault_series, "Elapsed time (ms)", "Cumulative page faults", used, skipped)
    thread_rows, thread_skipped, thread_source, thread_type = load(root, "results/monitoring_threads.csv", ("elapsed_ms", "tid", "cpu_utilization_percent"), reports)
    by_tid: dict[str, list[tuple[float, float]]] = defaultdict(list)
    for row in thread_rows:
        x, y = finite(row.get("elapsed_ms")), finite(row.get("cpu_utilization_percent"))
        if x is not None and y is not None:
            by_tid[label(row.get("tid"))].append((x, y))
    emit_line(reports, graph_root / "phase3/thread_cpu_utilization_over_time.svg", "G03-5", "Thread CPU Utilization over Time", thread_source, thread_type, sorted(by_tid.items()), "Elapsed time (ms)", "Thread CPU utilization (%)", sum(len(values) for values in by_tid.values()), thread_skipped)
    numa_series: dict[str, list[tuple[float, float]]] = defaultdict(list)
    for row in rows:
        x = finite(row.get("elapsed_ms"))
        text = label(row.get("process_numa_pages"), "")
        if x is None or text in ("", "UNKNOWN"):
            continue
        for node, value in (part.split("=", 1) for part in text.split(";") if "=" in part):
            number = finite(value)
            if number is not None:
                numa_series[node].append((x, number))
    emit_line(reports, graph_root / "phase3/numa_residency_over_time.svg", "G03-7", "NUMA Residency over Time", source, data_type, sorted(numa_series.items()), "Elapsed time (ms)", "Pages", sum(len(values) for values in numa_series.values()), skipped)
    reports.add("G03-8", "Monitoring Sample Interval Comparison", source, 0, len(rows), data_type, "SKIPPED", "sample interval is not an explicit column and no multiple interval dataset is available", records_available=len(rows))


def generate_phase4(root: Path, graph_root: Path, reports: Reports) -> None:
    rows, skipped, source, data_type = load(root, "results/classification_results.csv", ("elapsed_ms", "current_class", "entity_id", "score", "status"), reports)
    counts = Counter(label(row.get("current_class")) for row in rows)
    categories = sorted(counts)
    emit_bar(reports, graph_root / "phase4/classification_distribution.svg", "G04-1", "Classification Distribution", source, data_type, categories, [("records", [float(counts[key]) for key in categories])], "Classification", "Records", sum(counts.values()), skipped, "UNAVAILABLE classifications remain explicit")
    by_class: dict[str, list[tuple[float, float]]] = defaultdict(list)
    for index, row in enumerate(rows):
        x = finite(row.get("elapsed_ms"))
        if x is not None:
            classes = ("HOT", "MODERATE", "COLD", "UNAVAILABLE")
            current = label(row.get("current_class"))
            for class_name in classes:
                by_class[class_name].append((x, 1.0 if current == class_name else 0.0))
    emit_line(reports, graph_root / "phase4/classification_over_time.svg", "G04-2", "Classification over Elapsed Time", source, data_type, [(key, value) for key, value in by_class.items() if any(y > 0 for _, y in value)], "Elapsed time (ms)", "Class indicator", sum(len(value) for value in by_class.values()), skipped)
    by_entity: dict[str, Counter[str]] = defaultdict(Counter)
    for row in rows:
        by_entity[label(row.get("entity_id"))][label(row.get("current_class"))] += 1
    entities = sorted(by_entity)
    class_names = sorted({class_name for values in by_entity.values() for class_name in values})
    emit_bar(reports, graph_root / "phase4/classification_by_entity.svg", "G04-3", "Classification by Entity", source, data_type, entities, [(class_name, [float(by_entity[entity][class_name]) for entity in entities]) for class_name in class_names], "Entity", "Records", sum(sum(values.values()) for values in by_entity.values()), skipped)
    score_pairs, score_skipped = valid_pairs(rows, "elapsed_ms", "score")
    emit_line(reports, graph_root / "phase4/decay_score_over_time.svg", "G04-4", "Classifier Score over Time", source, data_type, [("score", score_pairs)], "Elapsed time (ms)", "Classifier score", len(score_pairs), skipped + score_skipped)
    reports.add("G04-5", "Classification Score vs Threshold", source, 0, len(rows), data_type, "SKIPPED", "no finite classifier scores are available", records_available=len(rows)) if not score_pairs else emit_line(reports, graph_root / "phase4/score_vs_threshold.svg", "G04-5", "Classification Score vs Threshold", source, data_type, [("score", score_pairs)], "Elapsed time (ms)", "Score / threshold comparison", len(score_pairs), skipped + score_skipped)
    monitoring_rows, monitoring_skipped, monitoring_source, _ = load(root, "results/monitoring_results.csv", ("pid", "timestamp", "benchmark_pattern"), reports)
    matches, join_skipped = exact_join(rows, monitoring_rows, ("pid", "timestamp"))
    pattern_counts: dict[str, Counter[str]] = defaultdict(Counter)
    for classification_row, monitoring_row in matches:
        pattern_counts[label(monitoring_row.get("benchmark_pattern"))][label(classification_row.get("current_class"))] += 1
    patterns = sorted(pattern_counts)
    joined_classes = sorted({class_name for values in pattern_counts.values() for class_name in values})
    joined_source = f"{source} + {monitoring_source}"
    if matches and patterns:
        emit_bar(reports, graph_root / "phase4/classification_by_workload_pattern.svg", "G04-6", "Classification Comparison by Workload Pattern", joined_source, data_type, patterns, [(class_name, [float(pattern_counts[pattern][class_name]) for pattern in patterns]) for class_name in joined_classes], "Workload pattern", "Records", len(matches), skipped + monitoring_skipped + join_skipped)
    else:
        reports.add("G04-6", "Classification Comparison by Workload Pattern", joined_source, 0, skipped + monitoring_skipped + join_skipped, data_type, "SKIPPED", "CROSS_PHASE_JOIN_UNAVAILABLE: no exact pid+timestamp match")


def generate_phase5(root: Path, graph_root: Path, reports: Reports) -> None:
    rows, skipped, source, data_type = load(root, "results/decision_results.csv", ("app_id", "decision", "memory_score_raw", "thread_score_raw", "memory_bias", "thread_bias", "status"), reports)
    counts = Counter(label(row.get("decision")) for row in rows)
    categories = sorted(counts)
    emit_bar(reports, graph_root / "phase5/decision_distribution.svg", "G05-1", "Decision Distribution", source, data_type, categories, [("records", [float(counts[key]) for key in categories])], "Decision", "Records", sum(counts.values()), skipped)
    memory_pairs = [(float(index), value) for index, row in enumerate(rows) if (value := finite(row.get("memory_score_raw"))) is not None]
    thread_pairs = [(float(index), value) for index, row in enumerate(rows) if (value := finite(row.get("thread_score_raw"))) is not None]
    memory_skipped = len(rows) - len(memory_pairs)
    thread_skipped = len(rows) - len(thread_pairs)
    if not memory_pairs and not thread_pairs:
        reports.add("G05-2", "Decision Scores", source, 0, len(rows), data_type, "SKIPPED", "decision scores are unavailable", records_available=len(rows))
        reports.add("G05-8", "Decision Score Difference", source, 0, len(rows), data_type, "SKIPPED", "decision scores are unavailable", records_available=len(rows))
    else:
        # Timestamps are categorical in this CSV, so row order is used only after
        # retaining the legitimate record identity and finite score values.
        score_series = [("memory_score_raw", memory_pairs), ("thread_score_raw", thread_pairs)]
        emit_line(reports, graph_root / "phase5/decision_scores.svg", "G05-2", "Decision Scores", source, data_type, score_series, "Decision record order", "Score", len(memory_pairs) + len(thread_pairs), skipped + memory_skipped + thread_skipped)
        differences = []
        for row in rows:
            memory, thread = finite(row.get("memory_score_raw")), finite(row.get("thread_score_raw"))
            if memory is not None and thread is not None:
                differences.append((float(len(differences)), memory - thread))
        emit_line(reports, graph_root / "phase5/decision_score_difference.svg", "G05-8", "Memory Score minus Thread Score", source, data_type, [("score_difference", differences)], "Decision record order", "Memory score - thread score", len(differences), skipped)
    by_app: dict[str, list[tuple[float, float]]] = defaultdict(list)
    by_thread_app: dict[str, list[tuple[float, float]]] = defaultdict(list)
    for index, row in enumerate(rows):
        app = label(row.get("app_id"))
        memory, thread = finite(row.get("memory_bias")), finite(row.get("thread_bias"))
        if memory is not None:
            by_app[app].append((float(index), memory))
        if thread is not None:
            by_thread_app[app].append((float(index), thread))
    emit_line(reports, graph_root / "phase5/memory_bias_by_application.svg", "G05-5", "Memory Bias by Application", source, data_type, sorted(by_app.items()), "Decision record order", "Memory bias", sum(len(values) for values in by_app.values()), skipped)
    emit_line(reports, graph_root / "phase5/thread_bias_by_application.svg", "G05-6", "Thread Bias by Application", source, data_type, sorted(by_thread_app.items()), "Decision record order", "Thread bias", sum(len(values) for values in by_thread_app.values()), skipped)
    signals = Counter("available" if finite(row.get("f_access")) is not None else "unavailable" for row in rows)
    signal_categories = sorted(signals)
    emit_bar(reports, graph_root / "phase5/decision_signal_availability.svg", "G05-7", "Decision Signal Availability", source, data_type, signal_categories, [("records", [float(signals[key]) for key in signal_categories])], "Signal status", "Records", sum(signals.values()), skipped)
    score_by_app: dict[str, dict[str, list[float]]] = defaultdict(lambda: {"memory": [], "thread": []})
    for row in rows:
        app = label(row.get("app_id"))
        memory, thread = finite(row.get("memory_score_raw")), finite(row.get("thread_score_raw"))
        if memory is not None:
            score_by_app[app]["memory"].append(memory)
        if thread is not None:
            score_by_app[app]["thread"].append(thread)
    score_apps = sorted(score_by_app)
    score_memory = [sum(score_by_app[app]["memory"]) / len(score_by_app[app]["memory"]) if score_by_app[app]["memory"] else math.nan for app in score_apps]
    score_thread = [sum(score_by_app[app]["thread"]) / len(score_by_app[app]["thread"]) if score_by_app[app]["thread"] else math.nan for app in score_apps]
    if any(math.isfinite(value) for value in score_memory + score_thread):
        emit_bar(reports, graph_root / "phase5/decision_scores_by_application.svg", "G05-3", "Decision Scores by Application", source, data_type, score_apps, [("memory_score_raw", score_memory), ("thread_score_raw", score_thread)], "Application", "Score", sum(len(values["memory"]) + len(values["thread"]) for values in score_by_app.values()), skipped)
    else:
        reports.add("G05-3", "Decision Scores by Application", source, 0, len(rows), data_type, "SKIPPED", "no finite decision scores by application", records_available=len(rows))
    decision_by_app: dict[str, Counter[str]] = defaultdict(Counter)
    for row in rows:
        decision_by_app[label(row.get("app_id"))][label(row.get("decision"))] += 1
    decision_apps = sorted(decision_by_app)
    decision_names = sorted({decision for values in decision_by_app.values() for decision in values})
    emit_bar(reports, graph_root / "phase5/decision_distribution_by_application.svg", "G05-4", "Decision Distribution by Application", source, data_type, decision_apps, [(decision, [float(decision_by_app[app][decision]) for app in decision_apps]) for decision in decision_names], "Application", "Records", sum(sum(values.values()) for values in decision_by_app.values()), skipped)


def generate_phase6_phase7(root: Path, graph_root: Path, reports: Reports) -> None:
    validation_path = root / "results/validation_results.csv"
    migration_path = root / "results/migration_results.csv"
    validation_rows, validation_skipped, validation_source, validation_type = load(root, "results/validation_results.csv", ("final_decision", "confidence_score", "roi_score", "safety_score", "validation_score", "action"), reports)
    if not validation_rows:
        for graph_id, name in (("G06-1", "Validation Accepted vs Rejected"), ("G06-2", "Validation Confidence Scores"), ("G06-3", "Validation ROI Scores"), ("G06-4", "Validation Safety Scores"), ("G06-5", "Final Validation Scores"), ("G06-6", "Validation by Migration Action"), ("G06-7", "Validation Rejection Reasons")):
            reports.add(graph_id, name, validation_source, 0, validation_skipped, validation_type, "NO_DATA", "validation results CSV is unavailable")
    else:
        final_counts = Counter(label(row.get("final_decision")) for row in validation_rows)
        cats = sorted(final_counts)
        emit_bar(reports, graph_root / "phase6/validation_results.svg", "G06-1", "Validation Accepted vs Rejected", validation_source, validation_type, cats, [("records", [float(final_counts[key]) for key in cats])], "Validation result", "Records", sum(final_counts.values()), validation_skipped)
        for graph_id, filename, name, column in (("G06-2", "confidence_scores.svg", "Validation Confidence Scores", "confidence_score"), ("G06-3", "roi_scores.svg", "Validation ROI Scores", "roi_score"), ("G06-4", "safety_scores.svg", "Validation Safety Scores", "safety_score"), ("G06-5", "validation_scores.svg", "Final Validation Scores", "validation_score")):
            values = [(float(index), value) for index, row in enumerate(validation_rows) if (value := finite(row.get(column))) is not None]
            emit_line(reports, graph_root / f"phase6/{filename}", graph_id, name, validation_source, validation_type, [(column, values)], "Validation record order", "Score", len(values), validation_skipped)
        action_counts: dict[str, Counter[str]] = defaultdict(Counter)
        reasons = Counter()
        for row in validation_rows:
            action_counts[label(row.get("action"))][label(row.get("final_decision"))] += 1
            reasons[label(row.get("final_decision"))] += 1
        actions = sorted(action_counts)
        results = sorted({result for values in action_counts.values() for result in values})
        emit_bar(reports, graph_root / "phase6/validation_by_action.svg", "G06-6", "Validation Result by Migration Action", validation_source, validation_type, actions, [(result, [float(action_counts[action][result]) for action in actions]) for result in results], "Action", "Records", sum(reasons.values()), validation_skipped)
        reason_categories = sorted(reasons)
        emit_bar(reports, graph_root / "phase6/rejection_reasons.svg", "G06-7", "Validation Result / Rejection Reason Distribution", validation_source, validation_type, reason_categories, [("records", [float(reasons[key]) for key in reason_categories])], "Result or rejection reason", "Records", sum(reasons.values()), validation_skipped)
    migration_rows, migration_skipped, migration_source, migration_type = load(root, "results/migration_results.csv", ("result", "execution_time_ms", "action", "requested", "attempted", "successful", "failed", "app_id"), reports)
    if not migration_rows:
        for graph_id, name in (("G07-1", "Migration Results"), ("G07-2", "Migration Results by Action"), ("G07-3", "Migration Execution Time"), ("G07-4", "Migration Page Counts"), ("G07-5", "Migration Results by Application"), ("G07-6", "Thread CPU Affinity Migration"), ("G07-7", "NUMA Source to Destination Migration"), ("G07-8", "Migration Result Categories")):
            reports.add(graph_id, name, migration_source, 0, migration_skipped, migration_type, "NO_DATA", "migration results CSV is unavailable")
    else:
        results = Counter(label(row.get("result")) for row in migration_rows)
        cats = sorted(results)
        emit_bar(reports, graph_root / "phase7/migration_results.svg", "G07-1", "Migration Success / Failure / No-Action Distribution", migration_source, migration_type, cats, [("records", [float(results[key]) for key in cats])], "Migration result", "Records", sum(results.values()), migration_skipped)
        by_action: dict[str, Counter[str]] = defaultdict(Counter)
        for row in migration_rows:
            by_action[label(row.get("action"))][label(row.get("result"))] += 1
        actions = sorted(by_action)
        emit_bar(reports, graph_root / "phase7/migration_by_action.svg", "G07-2", "Migration Results by Action", migration_source, migration_type, actions, [(result, [float(by_action[action][result]) for action in actions]) for result in cats], "Action", "Records", sum(results.values()), migration_skipped)
        times = [(float(index), value) for index, row in enumerate(migration_rows) if (value := finite(row.get("execution_time_ms"))) is not None]
        emit_line(reports, graph_root / "phase7/migration_overhead.svg", "G07-3", "Migration Execution Time", migration_source, migration_type, [("execution_time_ms", times)], "Migration record order", "Execution time (ms)", len(times), migration_skipped)
        page_series = []
        for column in ("requested", "attempted", "successful", "failed"):
            page_series.append((column, [(float(index), value) for index, row in enumerate(migration_rows) if (value := finite(row.get(column))) is not None]))
        emit_line(reports, graph_root / "phase7/migration_page_counts.svg", "G07-4", "Migration Requested / Attempted / Migrated / Failed Counts", migration_source, migration_type, page_series, "Migration record order", "Pages", len(migration_rows), migration_skipped)
        by_app: dict[str, Counter[str]] = defaultdict(Counter)
        for row in migration_rows:
            by_app[label(row.get("app_id"))][label(row.get("result"))] += 1
        apps = sorted(by_app)
        emit_bar(reports, graph_root / "phase7/migration_by_application.svg", "G07-5", "Migration Results by Application", migration_source, migration_type, apps, [(result, [float(by_app[app][result]) for app in apps]) for result in cats], "Application", "Records", sum(results.values()), migration_skipped)
        reports.add("G07-6", "Thread CPU Affinity Migration", migration_source, 0, migration_skipped, migration_type, "SKIPPED", "CROSS_PHASE_JOIN_UNAVAILABLE: migration output has destination_cpu but no source CPU identifier")
        numa_routes = Counter()
        for row in migration_rows:
            source_node, destination_node = finite(row.get("source_node")), finite(row.get("destination_node"))
            if source_node is not None and destination_node is not None:
                numa_routes[f"{int(source_node)} -> {int(destination_node)}"] += 1
        route_categories = sorted(numa_routes)
        if route_categories:
            emit_bar(reports, graph_root / "phase7/numa_source_to_destination.svg", "G07-7", "NUMA Source to Destination Migration", migration_source, migration_type, route_categories, [("records", [float(numa_routes[key]) for key in route_categories])], "NUMA route", "Records", sum(numa_routes.values()), migration_skipped, "Same-NUMA-Node routes remain distinct from remote migration")
        else:
            reports.add("G07-7", "NUMA Source to Destination Migration", migration_source, 0, migration_skipped, migration_type, "SKIPPED", "no valid NUMA source/destination records; remote NUMA is not inferred")
        emit_bar(reports, graph_root / "phase7/migration_result_categories.svg", "G07-8", "Migration Result Categories", migration_source, migration_type, cats, [("records", [float(results[key]) for key in cats])], "Result category", "Records", sum(results.values()), migration_skipped)


def feedback_type(rows: list[dict[str, str]], source: str) -> str:
    if "tests/" in source or any(label(row.get("app_id")).startswith("CLI_") for row in rows):
        return "CONTROLLED_TEST"
    return "REAL"


def feedback_rows_from_fixture(root: Path, reports: Reports) -> tuple[list[dict[str, str]], int, str, str]:
    rows, skipped, source, _ = load(root, "tests/graph_inputs/feedback_adaptation.csv", ("cycle", "app_id", "memory_bias", "thread_bias", "reward", "effective_reward", "confidence", "relevance"), reports, "CONTROLLED_TEST")
    return rows, skipped, source, "CONTROLLED_TEST"


def generate_phase8(root: Path, graph_root: Path, reports: Reports) -> None:
    rows, skipped, source, _ = load(root, "results/feedback_results.csv", ("feedback_id", "app_id", "raw_reward", "effective_reward", "feedback_confidence", "historical_relevance", "feedback_class"), reports)
    data_type = feedback_type(rows, source)
    cycle_rows = rows
    cycle_series = []
    for column, name in (("raw_reward", "reward"), ("effective_reward", "effective reward"), ("feedback_confidence", "confidence"), ("historical_relevance", "historical relevance")):
        values = [(float(index), value) for index, row in enumerate(cycle_rows) if (value := finite(row.get(column))) is not None]
        cycle_series.append((name, values))
    emit_line(reports, graph_root / "phase8/feedback_reward.svg", "G08-6", "Feedback Reward over Cycles", source, data_type, [cycle_series[0]], "Feedback cycle", "Reward", len(cycle_series[0][1]), skipped)
    emit_line(reports, graph_root / "phase8/effective_reward_over_time.svg", "G08-7", "Effective Reward over Time", source, data_type, [cycle_series[1]], "Feedback cycle", "Effective reward", len(cycle_series[1][1]), skipped)
    emit_line(reports, graph_root / "phase8/feedback_confidence.svg", "G08-9", "Feedback Confidence over Time", source, data_type, [cycle_series[2]], "Feedback cycle", "Confidence", len(cycle_series[2][1]), skipped)
    emit_line(reports, graph_root / "phase8/historical_relevance.svg", "G08-10", "Historical Relevance over Time", source, data_type, [cycle_series[3]], "Feedback cycle", "Relevance", len(cycle_series[3][1]), skipped)
    classes = Counter(label(row.get("feedback_class")) for row in cycle_rows)
    class_categories = sorted(classes)
    emit_bar(reports, graph_root / "phase8/feedback_class_distribution.svg", "G08-8", "Feedback Class Distribution", source, data_type, class_categories, [("records", [float(classes[key]) for key in class_categories])], "Feedback class", "Records", sum(classes.values()), skipped)
    updated = [row for row in cycle_rows if label(row.get("update_status")) == "FEEDBACK_UPDATED"]
    bias_memory = [(float(index), value) for index, row in enumerate(updated) if (value := finite(row.get("new_memory_bias"))) is not None]
    bias_thread = [(float(index), value) for index, row in enumerate(updated) if (value := finite(row.get("new_thread_bias"))) is not None]
    emit_line(reports, graph_root / "phase8/memory_bias_over_time.svg", "G08-1", "Memory Bias over Feedback Cycles", source, data_type, [("memory_bias", bias_memory)], "Successful feedback cycle", "Memory bias", len(bias_memory), skipped)
    emit_line(reports, graph_root / "phase8/thread_bias_over_time.svg", "G08-2", "Thread Bias over Feedback Cycles", source, data_type, [("thread_bias", bias_thread)], "Successful feedback cycle", "Thread bias", len(bias_thread), skipped)
    weight_series = []
    for action in ("memory", "thread"):
        for factor in FACTOR_NAMES:
            column = f"new_{action}_{factor}"
            values = [(float(index), value) for index, row in enumerate(updated) if (value := finite(row.get(column))) is not None]
            weight_series.append((f"{action}_{factor}", values))
    emit_line(reports, graph_root / "phase8/factor_weights_over_time.svg", "G08-3", "Individual Factor Weights over Feedback Cycles", source, data_type, weight_series, "Successful feedback cycle", "Weight", sum(len(values) for _, values in weight_series), skipped)
    emit_line(reports, graph_root / "phase8/all_12_factor_weights.svg", "G08-4", "All 12 Phase 5 Factor Weights", source, data_type, weight_series, "Successful feedback cycle", "Weight", sum(len(values) for _, values in weight_series), skipped)
    delta_series = []
    for action in ("memory", "thread"):
        for factor in FACTOR_NAMES:
            column = f"delta_{action}_{factor}"
            values = [(float(index), value) for index, row in enumerate(updated) if (value := finite(row.get(column))) is not None]
            delta_series.append((f"{action}_{factor}", values))
    emit_line(reports, graph_root / "phase8/weight_changes_old_to_new.svg", "G08-5", "Factor Weight Changes Old to New", source, data_type, delta_series, "Successful feedback cycle", "Weight delta", sum(len(values) for _, values in delta_series), skipped)
    fixture_rows, fixture_skipped, fixture_source, fixture_type = feedback_rows_from_fixture(root, reports)
    if fixture_rows:
        apps = sorted({label(row.get("app_id")) for row in fixture_rows})
        app_series = []
        for app in apps:
            app_series.append((app, [(value, finite(row.get("memory_bias"))) for row in fixture_rows if label(row.get("app_id")) == app and (value := finite(row.get("cycle"))) is not None and finite(row.get("memory_bias")) is not None]))
        emit_line(reports, graph_root / "phase8/application_adaptation.svg", "G08-11", "Application-Specific Memory Bias Adaptation", fixture_source, fixture_type, app_series, "Controlled feedback cycle", "Memory bias", sum(len(values) for _, values in app_series), fixture_skipped, "CONTROLLED TEST")
        fixture_bias_memory: dict[str, list[tuple[float, float]]] = defaultdict(list)
        fixture_bias_thread: dict[str, list[tuple[float, float]]] = defaultdict(list)
        fixture_weights: list[tuple[str, list[tuple[float, float]]]] = []
        for row in fixture_rows:
            cycle = finite(row.get("cycle"))
            memory = finite(row.get("memory_bias"))
            thread = finite(row.get("thread_bias"))
            if cycle is None:
                continue
            if memory is not None:
                fixture_bias_memory[label(row.get("app_id"))].append((cycle, memory))
            if thread is not None:
                fixture_bias_thread[label(row.get("app_id"))].append((cycle, thread))
        for action in ("memory", "thread"):
            for factor in FACTOR_NAMES:
                fixture_weights.append((f"{action}_{factor}", [(cycle, value) for row in fixture_rows if (cycle := finite(row.get("cycle"))) is not None and (value := finite(row.get(f"{action}_{factor}"))) is not None]))
        emit_line(reports, graph_root / "phase8/controlled_memory_bias_over_time.svg", "G08-C1", "Memory Bias over Feedback Cycles", fixture_source, fixture_type, sorted(fixture_bias_memory.items()), "Controlled feedback cycle", "Memory bias", sum(len(values) for values in fixture_bias_memory.values()), fixture_skipped, "CONTROLLED TEST")
        emit_line(reports, graph_root / "phase8/controlled_thread_bias_over_time.svg", "G08-C2", "Thread Bias over Feedback Cycles", fixture_source, fixture_type, sorted(fixture_bias_thread.items()), "Controlled feedback cycle", "Thread bias", sum(len(values) for values in fixture_bias_thread.values()), fixture_skipped, "CONTROLLED TEST")
        emit_line(reports, graph_root / "phase8/controlled_all_12_factor_weights.svg", "G08-C3", "All 12 Factor Weights over Feedback Cycles", fixture_source, fixture_type, fixture_weights, "Controlled feedback cycle", "Weight", sum(len(values) for _, values in fixture_weights), fixture_skipped, "CONTROLLED TEST")
    else:
        reports.add("G08-11", "Application-Specific Memory Bias Adaptation", fixture_source, 0, fixture_skipped, fixture_type, "NO_DATA", "no controlled APP_A/APP_B adaptation fixture is available")


def generate_combined(root: Path, graph_root: Path, reports: Reports) -> None:
    files = (("Phase 2 benchmark", "results/benchmark_results.csv"), ("Phase 3 monitoring", "results/monitoring_results.csv"), ("Phase 4 classification", "results/classification_results.csv"), ("Phase 5 decisions", "results/decision_results.csv"), ("Phase 6 validation", "results/validation_results.csv"), ("Phase 7 migration", "results/migration_results.csv"), ("Phase 8 feedback", "results/feedback_results.csv"))
    categories = [name for name, _ in files]
    counts = [float(count_rows(root, path)) for _, path in files]
    # The checked-in feedback CSV is generated from the explicitly controlled
    # CLI fixture, so it is not counted as a real-pipeline stage here.
    counts[6] = 0.0
    emit_bar(reports, graph_root / "combined/pipeline_counts.svg", "G-C04", "AWAVMA Pipeline Record Counts", "results/phase2-8 CSV files", "REAL", categories, [("records including headers excluded", counts)], "Pipeline stage", "Data records", int(sum(counts)), 0, "Counts are file-local; datasets are not row-joined")
    statuses = ["benchmark records", "monitoring records", "classification records", "INSUFFICIENT_DECISION_SIGNAL", "validation results unavailable", "migration results unavailable", "feedback results are controlled fixture data"]
    status_values = [counts[0], counts[1], counts[2], float(sum(1 for row in read_csv(root / "results/decision_results.csv")[0] if label(row.get("decision")) == "INSUFFICIENT_DECISION_SIGNAL")), 0.0, 0.0, 0.0]
    emit_bar(reports, graph_root / "combined/real_pipeline_status.svg", "G-C05", "Real Pipeline — Current Instrumentation", "results/phase2-8 CSV files", "REAL", statuses, [("observed count", status_values)], "Pipeline status", "Records / status count", int(sum(status_values)), 0, "Not a performance graph")
    reports.add("G-C01", "Workload to Classification to Decision to Migration", "results/*.csv", 0, 0, "REAL", "SKIPPED", "CROSS_PHASE_JOIN_UNAVAILABLE: no legitimate cross-phase migration identifier is available")
    reports.add("G-C02", "Application Decision to Migration to Feedback", "results/*.csv", 0, 0, "REAL", "SKIPPED", "CROSS_PHASE_JOIN_UNAVAILABLE: feedback records are controlled and migration results are absent")
    fixture = root / "tests/graph_inputs/feedback_adaptation.csv"
    fixture_rows, fixture_skipped, fixture_source, fixture_type = load(root, "tests/graph_inputs/feedback_adaptation.csv", ("cycle", "app_id", "memory_bias", "reward"), reports, "CONTROLLED_TEST")
    by_app = defaultdict(list)
    for row in fixture_rows:
        cycle, bias = finite(row.get("cycle")), finite(row.get("memory_bias"))
        if cycle is not None and bias is not None:
            by_app[label(row.get("app_id"))].append((cycle, bias))
    emit_line(reports, graph_root / "combined/feedback_weight_bias_adaptation.svg", "G-C03", "Feedback Cycle to Bias Adaptation", fixture_source, fixture_type, sorted(by_app.items()), "Controlled feedback cycle", "Memory bias", sum(len(values) for values in by_app.values()), fixture_skipped, "CONTROLLED TEST")
    state_rows, state_skipped, state_source, state_type = load(root, "state/application_state.csv", ("app_id", "last_seen", "status"), reports)
    state_counts = Counter(label(row.get("status")) for row in state_rows)
    state_categories = sorted(state_counts)
    if state_categories:
        emit_bar(reports, graph_root / "combined/application_state_status.svg", "G-C06", "Application-Specific State Status", state_source, state_type, state_categories, [("applications", [float(state_counts[key]) for key in state_categories])], "Application state", "Applications", sum(state_counts.values()), state_skipped, "Current state snapshot; no temporal evolution inferred")
    else:
        reports.add("G-C06", "Application-Specific State Status", state_source, 0, state_skipped, state_type, "NO_DATA", "application state CSV is unavailable")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate AWAVMA Phase 9 SVG graphs")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output-dir", type=Path, default=None)
    parser.add_argument("--summary", type=Path, default=None)
    parser.add_argument("--log", type=Path, default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    graph_root = (args.output_dir or root / "graphs").resolve()
    summary = (args.summary or root / "results/graph_generation_summary.csv").resolve()
    log_path = (args.log or root / "logs/graph_generation.log").resolve()
    graph_root.mkdir(parents=True, exist_ok=True)
    for phase in ("phase2", "phase3", "phase4", "phase5", "phase6", "phase7", "phase8", "combined"):
        (graph_root / phase).mkdir(parents=True, exist_ok=True)
    # The graphs directory is a generated artifact; remove stale SVGs so a
    # skipped graph from an earlier run cannot be mistaken for current output.
    for stale in graph_root.rglob("*.svg"):
        stale.unlink()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(filename=log_path, level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    logger = logging.getLogger("awavma.phase9")
    start = time.monotonic()
    reports = Reports(root, logger)
    logger.info("Phase 9 graph generation started root=%s backend=dependency-free-svg", root)
    generate_phase2(root, graph_root, reports)
    generate_phase3(root, graph_root, reports)
    generate_phase4(root, graph_root, reports)
    generate_phase5(root, graph_root, reports)
    generate_phase6_phase7(root, graph_root, reports)
    generate_phase8(root, graph_root, reports)
    generate_combined(root, graph_root, reports)
    reports.write(summary)
    elapsed = time.monotonic() - start
    logger.info("Phase 9 graph generation completed graphs=%d elapsed_seconds=%.6f", len(reports.rows), elapsed)
    generated = sum(1 for row in reports.rows if row["status"] == "GENERATED")
    skipped = sum(1 for row in reports.rows if row["status"] != "GENERATED")
    print(f"Phase 9 graph generation completed: generated={generated} skipped_or_unavailable={skipped} elapsed_seconds={elapsed:.6f}")
    print(f"Summary: {summary}")
    print(f"Graphs: {graph_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
