#!/usr/bin/env python3
"""Static Phase 4D tooling contract checks; collection is never invoked."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def require(text, *items):
    for item in items:
        assert item in text, f"missing Phase 4D contract: {item}"

def main():
    runner = (ROOT / "scripts/run_full_experiment.sh").read_text(encoding="utf-8")
    aggregate = (ROOT / "scripts/aggregate_experiment_results.py").read_text(encoding="utf-8")
    graphs = (ROOT / "scripts/generate_multinuma_graphs.py").read_text(encoding="utf-8")
    docs = (ROOT / "docs/cloudlab_preparation.md").read_text(encoding="utf-8")
    makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
    public = ("--check-only", "--tests-only", "--baseline-only", "--awavma-only", "--skip-graphs", "--output-dir")
    require(runner, *public, "metadata, build, tests, environment check, preflight", "run_required_tests", "environment-check")
    assert "--detect-numa" not in runner and "--results-dir" not in runner
    require(aggregate, "ADAPTERS", "phase2", "phase8", "ADDRESS_FIELD", "COMPARATOR_SCHEMA", "elapsed_seconds_summary.csv", "throughput_summary.csv", "latency_ms_summary.csv")
    require(graphs, "elapsed_time_comparison", "throughput_comparison", "latency_comparison", "summary_input", "zero graphs generated")
    require(makefile, "phase4d-check", "phase4d-tests", "test-phase4d-tooling")
    require(docs, "metadata -> build -> tests -> environment check -> preflight -> baseline -> AWAVMA -> aggregate -> graphs -> manifests", *public)
    print("cloudlab_phase4d_tooling_test: PASS")

if __name__ == "__main__": main()
