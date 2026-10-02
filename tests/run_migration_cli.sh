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

grep -q 'Rows        : 5' "$temporary/summary.txt"
grep -q 'Successful  : 0' "$temporary/summary.txt"
grep -q 'Not executed: 5' "$temporary/summary.txt"
grep -q 'MIGRATION_NO_ACTION' "$temporary/results/migration.csv"
grep -q 'MIGRATION_INSUFFICIENT_INFORMATION' "$temporary/results/migration.csv"
grep -q 'MIGRATION_NOT_AUTHORIZED' "$temporary/results/migration.csv"
grep -q 'standalone migration execution is disabled' "$temporary/results/migration.csv"
grep -q 'NOT_EXECUTED' "$temporary/results/migration.csv"
! grep -q 'MIGRATION_SUCCESS\|MIGRATION_PARTIAL_SUCCESS' "$temporary/results/migration.csv"

# The diagnostic CLI never has an executor link or accepts raw addresses as authority.
! grep -q 'Migration_Execute' "$root/src/migration_main.c"
! grep -q 'parse_pages' "$root/src/migration_main.c"
grep -q 'migration_safety_manager_attempt' "$root/src/awavma_runtime.c"
grep -q '^migration_safety_enabled=false$' "$root/config/awavma.conf"
grep -q '^migration_execution_enabled=false$' "$root/config/awavma.conf"
grep -q 'config->page_registration_enabled = false;' "$root/src/awavma_runtime.c"

printf '%s\n' 'Migration CLI fixtures passed.'
