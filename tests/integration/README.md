# AWAVMA Integration Tests

`integration_test.py` runs Phases 2 through 9 in a temporary isolated root.
Real benchmark/monitor output is kept separate from controlled feedback
fixtures. The controlled CSV under `inputs/` is algorithm-test data, not real
workload or migration performance data.

Run from the project root with:

```text
python3 tests/integration_test.py
```

The test writes its final report to `results/integration_results.csv`, its
phase matrix to `results/integration_matrix.csv`, and its log to
`logs/integration.log`. Existing `results/`, `state/`, `history/`, and
`graphs/` contents are not used as writable integration state.
