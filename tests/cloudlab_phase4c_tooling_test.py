#!/usr/bin/env python3
"""Compatibility checks for the superseding Phase 4D tooling."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def require(text, *items):
    for item in items:
        assert item in text, f"missing Phase 4D contract: {item}"

def main():
    runner = (ROOT / "scripts/run_full_experiment.sh").read_text()
    aggregate = (ROOT / "scripts/aggregate_experiment_results.py").read_text()
    graphs = (ROOT / "scripts/generate_multinuma_graphs.py").read_text()
    docs = (ROOT / "docs/cloudlab_preparation.md").read_text()
    require(runner, "--check-only", "--tests-only", "--baseline-only", "--awavma-only", "--skip-graphs", "--output-dir", "run_required_tests", "environment-check", "stage_order")
    assert "LOCAL_NODE=0" not in runner and "REMOTE_NODE=1" not in runner
    require(aggregate, "--run-dir", "--reference-run", "COMPARATOR_FIELDS", "SUMMARY_FIELDS", "operations", "execution_time_seconds", "throughput_ops_per_second")
    require(graphs, "execution_time_comparison", "summary_input", '"SKIPPED"', "zero graphs generated")
    require(docs, "--tests-only", "--baseline-only", "--awavma-only", "--output-dir", "metadata -> build -> tests -> environment check -> preflight -> baseline -> AWAVMA -> aggregate -> graphs -> manifests")
    print("cloudlab_phase4d_compatibility_test: PASS")

if __name__ == "__main__": main()
