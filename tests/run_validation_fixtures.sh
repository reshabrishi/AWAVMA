#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT

"$root/bin/validation_gate_test"
mkdir -p "$temporary/history" "$temporary/logs" "$temporary/results"
"$root/bin/validation" \
    --input "$root/tests/validation_inputs/validation_fixtures.csv" \
    --config "$root/tests/validation_inputs/compaction.conf" \
    --output "$temporary/results/validation.csv" \
    --history "$temporary/history/validation.csv" \
    --log "$temporary/logs/validation.log" \
    >"$temporary/summary.txt"

grep -q 'Rows        : 5' "$temporary/summary.txt"
grep -q 'Accepted    : 2' "$temporary/summary.txt"
grep -q 'Rejected    : 3' "$temporary/summary.txt"
grep -q 'mig-pass.*PASS,APPROVED' "$temporary/results/validation.csv"
grep -q 'mig-roi.*REJECT_LOW_ROI,REJECTED' "$temporary/results/validation.csv"
grep -q 'mig-veto.*REJECT_PAGE_LOCKED,REJECTED' "$temporary/results/validation.csv"
grep -q 'mig-none.*VALID_NO_MIGRATION,NO_MIGRATION' "$temporary/results/validation.csv"
grep -q 'mig-signal.*REJECT_INSUFFICIENT_SIGNAL,REJECTED' "$temporary/results/validation.csv"
[ "$(wc -l < "$temporary/history/validation.csv")" -eq 3 ]
grep -q 'history_relevance' "$temporary/history/validation.csv"

printf '%s\n' 'Validation fixtures passed.'
