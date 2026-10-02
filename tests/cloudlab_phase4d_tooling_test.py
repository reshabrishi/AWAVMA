#!/usr/bin/env python3
"""Static Phase 4D tooling contract checks; collection is never invoked."""
from pathlib import Path
import re

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
    require(runner, *public, "metadata, build, tests, environment check, preflight", "run_required_tests", "environment-check", "IN_PROGRESS", "wait_for_signal", "--target-ready-file", "--include-in-progress-run")
    assert "--detect-numa" not in runner and "--results-dir" not in runner
    awavma_block = runner.split('if [[ "$scenario" == awavma ]]; then', 1)[1].split('    else', 1)[0]
    runtime_command = next(line for line in awavma_block.splitlines() if '"$ROOT/bin/awavma-runtime"' in line)
    require(runtime_command, "--migration-safety-enabled", "--migration-execution-enabled", "--benefit-calibration", "--pid", "--ready-file", "--target-ready-file")
    assert "MOVE_MEMORY" not in runtime_command
    assert awavma_block.count('"$ROOT/bin/benchmark"') == 1
    assert awavma_block.count('"$ROOT/bin/awavma-runtime"') == 1
    baseline_block = runner.split('        CHILD_PIDS=()\n    else\n', 1)[1].split('    fi\n', 1)[0]
    assert "--migration-execution-enabled" not in baseline_block
    calibration_required = re.search(r'required = "([^"]+)"', runner)
    assert calibration_required, "missing calibration preflight fields"
    for field in ("source_node", "target_node", "throughput_gain_percent", "state", "validation_status", "environment_check_status"):
        assert field in calibration_required.group(1), f"missing calibration preflight field: {field}"
    require(runner, '"VALIDATED_PRODUCTION"', '"PASS"', '"READY"', 'gain + 0 <= 0',
            '$(column["state"]) != "VALIDATED_PRODUCTION"',
            '$(column["validation_status"]) != "PASS"',
            '$(column["environment_check_status"]) != "READY"')
    require(runner,
            'numactl --cpunodebind="$LOCAL_NODE" --membind="$LOCAL_NODE" true',
            'numactl --cpunodebind="$REMOTE_NODE" --membind="$LOCAL_NODE" true',
            'validate_calibration_route "$REMOTE_NODE" "$LOCAL_NODE"',
            '"$REMOTE_NODE" "$LOCAL_NODE" >&2')
    assert 'validate_calibration_route "$LOCAL_NODE" "$REMOTE_NODE"' not in runner
    assert '"$LOCAL_NODE" "$REMOTE_NODE" >&2' not in runner
    require(runner,
            'baseline-local) directory="$RUN_DIR/baseline"; placement=(numactl "--cpunodebind=$LOCAL_NODE" "--membind=$LOCAL_NODE"); thread_node=$LOCAL_NODE; memory_node=$LOCAL_NODE ;;',
            'baseline-remote) directory="$RUN_DIR/baseline"; placement=(numactl "--cpunodebind=$REMOTE_NODE" "--membind=$LOCAL_NODE"); thread_node=$REMOTE_NODE; memory_node=$LOCAL_NODE ;;',
            'awavma) directory="$RUN_DIR/awavma"; placement=(numactl "--cpunodebind=$REMOTE_NODE" "--membind=$LOCAL_NODE"); thread_node=$REMOTE_NODE; memory_node=$LOCAL_NODE; runtime=true ;;')
    assert 'placement=(numactl "--cpunodebind=$LOCAL_NODE" "--membind=$REMOTE_NODE")' not in runner
    require(awavma_block, 'wait_for_signal "$benchmark_ready" "$benchmark_pid" benchmark',
            'wait_for_signal "$runtime_ready" "$runtime_pid" runtime',
            'wait_for_signal "$target_ready" "$runtime_pid" runtime-target',
            ': >"$benchmark_start"')
    assert awavma_block.index('wait_for_signal "$target_ready"') < awavma_block.index(': >"$benchmark_start"')
    require(aggregate, "ADAPTERS", "phase2", "phase8", "ADDRESS_FIELD", "COMPARATOR_SCHEMA", "elapsed_seconds", "throughput", "latency_ms", "include-in-progress-run")
    require(graphs, "elapsed_time_comparison", "throughput_comparison", "latency_comparison", "summary_input", "zero graphs generated")
    require(makefile, "phase4d-check", "phase4d-tests", "test-phase4d-tooling")
    require(docs, "manifest IN_PROGRESS", "manifest PASS", *public)
    print("cloudlab_phase4d_tooling_test: PASS")

if __name__ == "__main__": main()
