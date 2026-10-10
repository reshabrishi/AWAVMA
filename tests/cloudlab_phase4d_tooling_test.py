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
    public = ("--check-only", "--tests-only", "--baseline-only", "--awavma-only", "--skip-graphs", "--workload", "--output-dir", "--calibration", "--reference-run")
    require(runner, *public, "metadata, build, tests, environment check, preflight", "run_required_tests", "environment-check", "test-environment-capabilities", "test-runtime-execution-profile", "--production-real-migration", "--calibration-artifact \"$CALIBRATION_COPY\"", "--calibration-manifest", "--controlled-workload-pattern", "calibration-validate", "cmp -s", "sha256sum", "runtime_execution_profile.csv", "verify_runtime_execution_profile.py", "! -e \"$runtime_root\"", "--placement-mode", "--placement-evidence", "validate_placement", "validate_remote_equivalence")
    assert "--detect-numa" not in runner and "--results-dir" not in runner
    require(aggregate, "--run-dir", "--reference-run", "COMPARATOR_FIELDS", "SUMMARY_FIELDS", "operations", "execution_time_seconds", "throughput_ops_per_second", "mean_wall_time_per_operation_seconds", "primary_awavma_vs_remote")
    require(graphs, "execution_time_comparison", "throughput_comparison", "operation_comparison", "wall_time_per_operation", "summary_input", "zero graphs generated")
    require(makefile, "phase4d-check", "phase4d-tests", "test-phase4d-tooling")
    require(docs, "metadata -> build -> tests -> environment check -> preflight -> baseline -> AWAVMA -> aggregate -> graphs -> manifests", *public)
    print("cloudlab_phase4d_tooling_test: PASS")

if __name__ == "__main__": main()
