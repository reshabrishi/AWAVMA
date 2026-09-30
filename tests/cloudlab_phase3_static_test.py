#!/usr/bin/env python3
"""Static contract checks for the CloudLab Phase 3 collection pipeline.

This deliberately does not execute the runner, aggregator, or graph generator.
"""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(text: str, *needles: str) -> None:
    for needle in needles:
        if needle not in text:
            raise AssertionError(f"missing required contract: {needle}")


def main() -> int:
    runner = (ROOT / "scripts/run_full_experiment.sh").read_text(encoding="utf-8")
    aggregate = (ROOT / "scripts/aggregate_experiment_results.py").read_text(encoding="utf-8")
    graphs = (ROOT / "scripts/generate_multinuma_graphs.py").read_text(encoding="utf-8")
    docs = (ROOT / "docs/cloudlab_preparation.md").read_text(encoding="utf-8")

    require(runner, "--check-only", "--tests-only", "manifest.json", "RUN_DIR=", "discover_nodes", "run_required_tests")
    if "LOCAL_NODE=0" in runner or "REMOTE_NODE=1" in runner:
        raise AssertionError("runner must not hardcode NUMA node IDs")
    require(aggregate, "WIDE_SCHEMA", "COMPARATOR_SCHEMA", "ADDRESS_FIELD", "read_run", '"REAL"')
    require(graphs, "graph_summary.csv", '"SKIPPED"', "summary_input", "data_source")
    require(docs, "--check-only", "--tests-only", "cloudlab_comparator.csv", "graph_summary.csv")
    print("cloudlab_phase3_static_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
