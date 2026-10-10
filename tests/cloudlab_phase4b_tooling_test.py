#!/usr/bin/env python3
"""Compatibility static checks for the superseding Phase 4D tooling."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def require(text: str, *needles: str) -> None:
    for needle in needles:
        if needle not in text:
            raise AssertionError(f"missing Phase4D contract: {needle}")


def main() -> int:
    runner = (ROOT / "scripts/run_full_experiment.sh").read_text(encoding="utf-8")
    aggregate = (ROOT / "scripts/aggregate_experiment_results.py").read_text(encoding="utf-8")
    graphs = (ROOT / "scripts/generate_multinuma_graphs.py").read_text(encoding="utf-8")
    docs = (ROOT / "docs/cloudlab_preparation.md").read_text(encoding="utf-8")
    require(runner, "collection_status", "ENV_LIMITED=3", "--baseline-only", "--awavma-only", "run_required_tests")
    if "LOCAL_NODE=0" in runner or "REMOTE_NODE=1" in runner:
        raise AssertionError("NUMA IDs must not be hardcoded")
    require(aggregate, "--run-dir", "--reference-run", "comparison_key", "COMPARATOR_FIELDS", "collection_status")
    require(graphs, "required real comparable fields unavailable", "multiple comparison sets require filtering", "summary_input", '"SKIPPED"')
    require(docs, "cloudlab_comparator.csv", "--output-dir", "baseline -> AWAVMA")
    print("cloudlab_phase4d_compatibility_test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
