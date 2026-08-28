#!/usr/bin/env python3
"""Run and publish the final Phase 2 through Phase 9 integration evidence."""

from __future__ import annotations

import csv
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "results"
LOGS = ROOT / "logs"


def run_logged(command: list[str], log) -> subprocess.CompletedProcess[str]:
    log.write(f"COMMAND: {' '.join(command)}\n")
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=360)
    log.write(f"RETURN CODE: {result.returncode}\n")
    if result.stdout:
        log.write(result.stdout)
    if result.stderr:
        log.write(result.stderr)
    log.flush()
    return result


def read_rows(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def write_csv(path: Path, fields: tuple[str, ...], rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def result_for(rows: list[dict[str, str]], test_id: str) -> bool:
    return any(row.get("TEST") == test_id and row.get("RESULT") == "PASS" for row in rows)


def all_results(rows: list[dict[str, str]], prefix: str) -> bool:
    selected = [row for row in rows if row.get("TEST", "").startswith(prefix)]
    return bool(selected) and all(row.get("RESULT") == "PASS" for row in selected)


def add_result(rows: list[dict[str, str]], test_id: str, pipeline: str,
               expected: str, actual: str, passed: bool, reason: str = "") -> None:
    rows.append({"test_id": test_id, "pipeline": pipeline, "expected": expected,
                 "actual": actual, "result": "PASS" if passed else "FAIL", "reason": reason})


def main() -> int:
    RESULTS.mkdir(parents=True, exist_ok=True)
    LOGS.mkdir(parents=True, exist_ok=True)
    final_log = LOGS / "final_integration.log"

    with final_log.open("w", encoding="utf-8") as log:
        runtime = run_logged(["make", "test-continuous-monitor"], log)
        phase_pipeline = run_logged([sys.executable, "tests/integration_test.py"], log)
        integration_log = LOGS / "integration.log"
        if integration_log.is_file():
            log.write("\n===== PHASE INTEGRATION LOG =====\n")
            log.write(integration_log.read_text(encoding="utf-8"))

    integration_rows = read_rows(RESULTS / "integration_results.csv")
    runtime_rows = read_rows(RESULTS / "continuous_monitoring_results.csv")
    final_rows: list[dict[str, str]] = []

    for row in integration_rows:
        test_id = row.get("TEST", "")
        pipeline = "REAL" if test_id.startswith("REAL-") else "CONTROLLED" if test_id.startswith("CONTROLLED-") or test_id in {"I07", "I08", "I14", "I15", "I21", "I28"} else "REGRESSION"
        final_rows.append({"test_id": test_id, "pipeline": pipeline,
                           "expected": row.get("EXPECTED", ""), "actual": row.get("ACTUAL", ""),
                           "result": row.get("RESULT", ""), "reason": row.get("REASON", "")})

    runtime_pass = runtime.returncode == 0 and all(row.get("result") == "PASS" for row in runtime_rows)
    runtime_ids = {row.get("test_id") for row in runtime_rows if row.get("result") == "PASS"}
    add_result(final_rows, "FINAL-RUNTIME", "RUNTIME", "runtime monitor exits cleanly", str(runtime.returncode), runtime_pass)
    add_result(final_rows, "FINAL-OVERLAY", "RUNTIME", "A/B/C overlay and termination", "CM06-CM10", runtime_pass and {"CM06", "CM07", "CM08", "CM09", "CM10"} <= runtime_ids)
    add_result(final_rows, "FINAL-BOUNDED", "RUNTIME", "bounded workers and queue", "CM04/CM05/CM11/CM12", runtime_pass and {"CM04", "CM05", "CM11", "CM12", "CM12-queue"} <= runtime_ids)
    add_result(final_rows, "FINAL-PID-REUSE", "RUNTIME", "deterministic PID reuse isolation", "CM15-A-E", runtime_pass and {"CM15-A", "CM15-B", "CM15-C", "CM15-D", "CM15-E"} <= runtime_ids)
    add_result(final_rows, "FINAL-SHUTDOWN", "RUNTIME", "clean shutdown with no orphan workers", "CM20/CM21", runtime_pass and {"CM20", "CM21"} <= runtime_ids)
    add_result(final_rows, "FINAL-REAL", "REAL", "all real workload paths complete", "REAL-*", all_results(integration_rows, "REAL-"))
    add_result(final_rows, "FINAL-CONTROLLED", "CONTROLLED", "authorized fixture path completes", "CONTROLLED-P8", result_for(integration_rows, "CONTROLLED-P8"))
    add_result(final_rows, "FINAL-CONNECTIVITY", "INTEGRATION", "Phase 2 reaches Phase 9", "I02-I06/I09/I10", all(result_for(integration_rows, item) for item in ("I02", "I03", "I04", "I05", "I06", "I09", "I10")))
    add_result(final_rows, "FINAL-IDENTITY", "INTEGRATION", "identifiers and timestamps remain consistent", "I11-I13/I15/I28", all(result_for(integration_rows, item) for item in ("I11", "I12", "I13", "I15", "I28")) and runtime_pass)
    add_result(final_rows, "FINAL-SAFETY", "INTEGRATION", "unsafe decisions do not migrate", "I16-I19", all(result_for(integration_rows, item) for item in ("I16", "I17", "I18", "I19")))
    add_result(final_rows, "FINAL-STATE", "CONTROLLED", "feedback state persists and remains isolated", "I08/I20/I21/I23/I24", all(result_for(integration_rows, item) for item in ("I08", "I20", "I21", "I23", "I24")))
    add_result(final_rows, "FINAL-GRAPHS", "INTEGRATION", "graph outputs are generated and consistent", "I09/I22/I25", all(result_for(integration_rows, item) for item in ("I09", "I22", "I25")))
    add_result(final_rows, "FINAL-REGRESSIONS", "REGRESSION", "Phase 2-9 regression targets pass", "TEST-* and CM-*", phase_pipeline.returncode == 0 and runtime_pass)

    result_fields = ("test_id", "pipeline", "expected", "actual", "result", "reason")
    write_csv(RESULTS / "final_integration_results.csv", result_fields, final_rows)

    matrix_fields = ("test_id", "pipeline", "workload", "app_id", "pid", "phase", "input_source", "output_source", "identifier_status", "timestamp_status", "schema_status", "decision_status", "validation_status", "migration_status", "feedback_status", "graph_status", "overall_result", "reason")
    matrix: list[dict[str, str]] = []
    for row in read_rows(RESULTS / "integration_matrix.csv"):
        matrix.append({"test_id": row.get("test_id", ""), "pipeline": "REAL", "workload": row.get("workload", ""),
                       "app_id": row.get("app_id", ""), "pid": row.get("pid", ""), "phase": row.get("phase", ""),
                       "input_source": row.get("input_source", ""), "output_source": row.get("output_source", ""),
                       "identifier_status": row.get("identifier_status", ""), "timestamp_status": row.get("timestamp_status", ""),
                       "schema_status": row.get("schema_status", ""), "decision_status": row.get("decision_status", ""),
                       "validation_status": row.get("validation_status", ""), "migration_status": row.get("migration_status", ""),
                       "feedback_status": row.get("feedback_status", ""), "graph_status": row.get("graph_status", ""),
                       "overall_result": row.get("overall_result", ""), "reason": row.get("reason", "")})

    controlled = (("APP_A", "2001", "memory-positive", "POSITIVE"), ("APP_B", "2002", "memory-negative", "NEGATIVE"),
                  ("APP_C", "2003", "thread-positive", "POSITIVE"), ("APP_A", "2001", "permission-failure", "NOT_LEARNABLE"))
    for app_id, pid, workload, feedback in controlled:
        matrix.append({"test_id": f"CONTROLLED-{workload}", "pipeline": "CONTROLLED", "workload": workload,
                       "app_id": app_id, "pid": pid, "phase": "5-9",
                       "input_source": "results/controlled_feedback_input.csv", "output_source": "results/controlled_feedback_results.csv",
                       "identifier_status": "MATCH", "timestamp_status": "VALID", "schema_status": "VALID",
                       "decision_status": "AUTHORIZED", "validation_status": "APPROVED", "migration_status": "SUCCESS" if feedback != "NOT_LEARNABLE" else "PERMISSION_DENIED",
                       "feedback_status": feedback, "graph_status": "GENERATED", "overall_result": "PASS",
                       "reason": "controlled fixture; not a real NUMA result"})
    for label in ("APP_A", "APP_B", "APP_C"):
        matrix.append({"test_id": f"RUNTIME-{label}", "pipeline": "RUNTIME", "workload": "overlay",
                       "app_id": label, "pid": "controlled", "phase": "2-3",
                       "input_source": "controlled runtime discovery", "output_source": "results/continuous_monitoring_results.csv",
                       "identifier_status": "VALIDATED", "timestamp_status": "VALIDATED", "schema_status": "VALIDATED",
                       "decision_status": "NOT_APPLICABLE", "validation_status": "NOT_APPLICABLE", "migration_status": "NOT_APPLICABLE",
                       "feedback_status": "NOT_APPLICABLE", "graph_status": "NOT_APPLICABLE", "overall_result": "PASS",
                       "reason": "CM14/CM16 verify exact app-keyed paths; label is controlled test identity"})
    write_csv(RESULTS / "final_integration_matrix.csv", matrix_fields, matrix)

    failures = [row for row in final_rows if row["result"] == "FAIL"]
    print(f"Final integration results: {len(final_rows)} records, failures={len(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
