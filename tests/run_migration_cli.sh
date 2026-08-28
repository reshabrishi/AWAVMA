#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT

mkdir -p "$temporary/results" "$temporary/history" "$temporary/logs" "$temporary/state"
"$root/bin/migration" \
    --input "$root/tests/migration_inputs/authorization.csv" \
    --output "$temporary/results/migration.csv" \
    --history "$temporary/history/migration.csv" \
    --log "$temporary/logs/migration.log" \
    --state "$temporary/state/migration.csv" \
    >"$temporary/summary.txt"

grep -q 'Rows        : 4' "$temporary/summary.txt"
grep -q 'Successful  : 0' "$temporary/summary.txt"
grep -q 'Not executed: 4' "$temporary/summary.txt"
grep -q 'MIGRATION_NO_ACTION' "$temporary/results/migration.csv"
grep -q 'MIGRATION_INSUFFICIENT_INFORMATION' "$temporary/results/migration.csv"
grep -q 'MIGRATION_NOT_AUTHORIZED' "$temporary/results/migration.csv"

printf '%s\n' 'Migration CLI fixtures passed.'
