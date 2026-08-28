#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT

mkdir -p "$temporary/state" "$temporary/history" "$temporary/logs"
"$root/bin/feedback" \
    --input "$root/tests/feedback_inputs/feedback_fixtures.csv" \
    --output "$temporary/feedback_results.csv" \
    --history "$temporary/history/feedback_history.csv" \
    --log "$temporary/logs/feedback.log" \
    --state-dir "$temporary/state" \
    --history-dir "$temporary/history" \
    >"$temporary/summary.txt"

grep -q 'Rows        : 3' "$temporary/summary.txt"
grep -q 'Updated     : 1' "$temporary/summary.txt"
grep -q 'Not updated : 2' "$temporary/summary.txt"
grep -q 'FEEDBACK_UPDATED' "$temporary/history/feedback_history.csv"
grep -q 'FEEDBACK_NOT_LEARNABLE' "$temporary/history/feedback_history.csv"
grep -q 'FEEDBACK_NO_UPDATE' "$temporary/history/feedback_history.csv"

printf '%s\n' 'Feedback CLI fixtures passed.'
