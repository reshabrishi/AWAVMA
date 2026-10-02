#!/usr/bin/env python3
"""Generate Phase 4D comparison graphs from comparator and metric summaries."""
from __future__ import annotations
import argparse, csv, math
from pathlib import Path
from xml.sax.saxutils import escape

GRAPHS = (
    ("elapsed_time_comparison", "elapsed_seconds", "Elapsed Time by Scenario", "Seconds"),
    ("throughput_comparison", "throughput", "Throughput by Scenario", "Operations per second"),
    ("latency_comparison", "latency_ms", "Latency by Scenario", "Milliseconds"),
)
SCENARIOS = (("baseline_default_mean", "baseline-default"), ("baseline_local_mean", "baseline-local"), ("baseline_remote_mean", "baseline-remote"), ("awavma_mean", "awavma"))

def number(value):
    try:
        parsed=float(value)
        return parsed if math.isfinite(parsed) else None
    except (TypeError, ValueError): return None
def chart(path, title, unit, values):
    maximum=max(values.values()) or 1.0
    bars=[]
    for index, (label, value) in enumerate(values.items()):
        x=80 + index * 175
        height=300 * value / maximum
        bars.append(f'<rect x="{x}" y="{390-height:.3f}" width="120" height="{height:.3f}" fill="#1f4e79"/><text x="{x}" y="420">{escape(label)}</text><text x="{x}" y="{380-height:.3f}">{value:.6g}</text>')
    path.write_text(f'<svg xmlns="http://www.w3.org/2000/svg" width="800" height="460"><text x="30" y="35" font-size="20">{escape(title)}</text><text x="30" y="60">{escape(unit)}</text>{"".join(bars)}</svg>\n', encoding="utf-8")
def rows(path):
    if not path.is_file(): return []
    with path.open(newline="", encoding="utf-8") as handle: return list(csv.DictReader(handle))
def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=root / "results/cloudlab/unified/cloudlab_comparator.csv")
    parser.add_argument("--summary-dir", type=Path, default=root / "results/cloudlab/unified/summaries")
    parser.add_argument("--output-dir", type=Path, default=root / "results/cloudlab/graphs")
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--diagnostic", type=Path, action="append", default=[])
    args=parser.parse_args()
    comparator=rows(args.input)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    reports=[]
    for graph_name, metric, title, unit in GRAPHS:
        output=args.output_dir / f"{graph_name}.svg"
        metric_rows=[row for row in comparator if row.get("metric") == metric and row.get("data_source") == "REAL" and row.get("collection_status") == "PASS" and row.get("comparable") == "true"]
        summary_rows=rows(args.summary_dir / f"{metric}_summary.csv")
        if len(metric_rows) != 1:
            reason="required real comparable fields unavailable" if not metric_rows else "multiple comparison sets require filtering"
            reports.append({"graph":graph_name,"metric":metric,"status":"SKIPPED","reason":reason,"summary_input":str(args.summary_dir / f"{metric}_summary.csv"),"output_file":str(output),"records_used":"0"})
            continue
        if not summary_rows or {row.get("scenario") for row in summary_rows} < {label for _, label in SCENARIOS}:
            reports.append({"graph":graph_name,"metric":metric,"status":"SKIPPED","reason":"required metric summary is unavailable or incomplete","summary_input":str(args.summary_dir / f"{metric}_summary.csv"),"output_file":str(output),"records_used":"0"})
            continue
        values={label:number(metric_rows[0].get(field)) for field, label in SCENARIOS}
        if any(value is None for value in values.values()):
            reports.append({"graph":graph_name,"metric":metric,"status":"SKIPPED","reason":"comparator contains non-finite scenario value","summary_input":str(args.summary_dir / f"{metric}_summary.csv"),"output_file":str(output),"records_used":"0"})
            continue
        chart(output, title, unit, values)
        reports.append({"graph":graph_name,"metric":metric,"status":"GENERATED","reason":"","summary_input":str(args.summary_dir / f"{metric}_summary.csv"),"output_file":str(output),"records_used":str(sum(1 for row in summary_rows if row.get("scenario") in values))})
    diagnostic=[]
    for path in args.diagnostic:
        diagnostic.extend(rows(path))
    diagnostic_specs=(
        ("diagnostic_cpu", "process_cpu_utilization_percent", "Process CPU Utilization", "Percent"),
        ("diagnostic_minor_faults", "interval_minor_faults", "Interval Minor Faults", "Faults"),
        ("diagnostic_major_faults", "interval_major_faults", "Interval Major Faults", "Faults"),
        ("diagnostic_access", "access_value", "Access Value", "Value"),
        ("diagnostic_classifier_score", "classification_score", "Classifier Score", "Score"),
        ("diagnostic_memory_score", "memory_score", "Memory Score", "Score"),
        ("diagnostic_thread_score", "thread_score", "Thread Score", "Score"),
        ("diagnostic_decision_margin", "decision_margin", "Decision Margin", "Score"),
        ("diagnostic_thread_node", "thread_current_node", "Thread NUMA Node", "Node"),
        ("diagnostic_memory_node", "memory_dominant_node", "Dominant Memory NUMA Node", "Node"),
    )
    for graph_name, field, title, unit in diagnostic_specs:
        output=args.output_dir / f"{graph_name}.svg"; values={str(i):number(row.get(field)) for i,row in enumerate(diagnostic)}
        values={key:value for key,value in values.items() if value is not None}
        if not values:
            reports.append({"graph":graph_name,"metric":field,"status":"SKIPPED","reason":"measured diagnostic series unavailable","summary_input":"diagnostic","output_file":str(output),"records_used":"0"})
        else:
            chart(output,title,unit,values)
            reports.append({"graph":graph_name,"metric":field,"status":"GENERATED","reason":"","summary_input":"diagnostic","output_file":str(output),"records_used":str(len(values))})
    availability={}
    for field in ("access_value","classification_score","memory_score","thread_score","decision_margin","confidence_gate","roi_gate","safety_gate"):
        availability[field]=sum(1 for row in diagnostic if row.get(field,"NA") not in ("", "NA", "-1"))
    if diagnostic:
        output=args.output_dir / "diagnostic_signal_availability.svg"; chart(output,"Diagnostic Signal Availability","Measured records",availability)
        reports.append({"graph":"diagnostic_signal_availability","metric":"availability","status":"GENERATED","reason":"","summary_input":"diagnostic","output_file":str(output),"records_used":str(len(diagnostic))})
        for field in ("classification","confidence_gate","roi_gate","safety_gate","migration_result","rejection_reason","decision"):
            counts={}
            for row in diagnostic:
                label=row.get(field,"NA") or "NA"; counts[label]=counts.get(label,0)+1
            if counts:
                output=args.output_dir / f"diagnostic_{field}_counts.svg"; chart(output,f"{field} Outcomes","Records",counts)
                reports.append({"graph":f"diagnostic_{field}_counts","metric":field,"status":"GENERATED","reason":"","summary_input":"diagnostic","output_file":str(output),"records_used":str(len(diagnostic))})
    summary=args.summary or args.output_dir / "graph_summary.csv"
    summary.parent.mkdir(parents=True, exist_ok=True)
    with summary.open("w", newline="", encoding="utf-8") as handle:
        writer=csv.DictWriter(handle, fieldnames=("graph","metric","status","reason","summary_input","output_file","records_used")); writer.writeheader(); writer.writerows(reports)
    if not any(row["status"] == "GENERATED" for row in reports): raise SystemExit("zero graphs generated; see graph summary")
if __name__ == "__main__": main()
