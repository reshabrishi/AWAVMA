# CloudLab NUMA calibration

P4-C is separate from final P7 evaluation. It collects controlled P3 placement
evidence and controlled, rollback-required migration-cost measurements; it never
enables normal AWAVMA production migration execution.

1. Build `make benchmark calibration-validate bin/p4c-migration-cost-collector` on a suitable CloudLab node.
2. Run `sudo --preserve-env=P4C_OUTPUT_DIR env P4C_OUTPUT_DIR="$PWD/results/cloudlab_calibration" tools/collect_numa_calibration.sh --smoke` and inspect its retained
   placement CSVs and `manifest.json`.
3. Only after a successful smoke, run the same exact privileged command with `--full`.
4. Supply the retained raw timing, authenticated 4096-page cost CSV, and collection
   manifest to `tools/build_numa_calibration.py`. Its CSV and production manifest
   paths must be distinct files in a new bundle directory; it stages both files as
   a sibling and atomically publishes the directory without replacing a collision.
   Then run `bin/calibration-validate ARTIFACT`.

The collector defaults to smoke. Smoke performs one authenticated exact-4096-page
`move_pages` transaction and retains its row as a warmup, so it cannot contribute
to a calibration artifact. Full collection requires a smoke manifest and retains
two successful warmups plus seven successful measured samples; invalid attempts
are retained but excluded, with bounded retries.
`cost_migration_valid` describes only the retained cost evidence for that run:
smoke requires one valid warmup; full requires exactly two valid warmups and
seven valid measured rows. Every accepted row must be an exact 4096-page
transaction with positive finite elapsed time and a consistent source,
destination, distance, and page size. It is independent of timing calibration,
thread calibration, remote equivalence, and production authorization.
The script contains no `sudo`. Before any timing or migration-cost work it reads
the exact `0` or `1` from `/proc/sys/kernel/numa_balancing` (overridable for
testing with `P4C_NUMA_BALANCING_PATH`), writes `0`, and verifies readback. It
requires the value to remain `0` throughout collection and restores and verifies
the original value on normal exit and signals. Restoration failure overrides an
otherwise successful run.

Schema-2 manifests retain `numa_balancing_original`,
`numa_balancing_during`, `numa_balancing_restore_status`, `timing_valid`,
`cost_migration_valid`, `numa_balancing_transaction_valid`, and `overall_valid`.
Smoke promotion and successful exit require every timing, cost, transaction,
and restoration gate. Full mode requires that promoted schema-2 smoke gate;
legacy schema-1 artifacts remain diagnostically readable but provide no claim
of a controlled NUMA-balancing transaction.
Smoke is always labelled `NOT_PRODUCTION_CALIBRATION`; it is not a production
artifact. The builder requires at least seven valid pairs and seven exact,
successful 4096-page `move_pages` cost observations for every production record.

The paired lower bound and migration cost upper bound use a one-sided 95% Student
t interval. `uncertainty_pct` is zero because those bounds already represent the
statistical uncertainty; P4-C does not invent a second uncertainty percentage.
`safety_margin_pct` is an explicit, configurable policy value (default 1%).

## Controlled workload matrix

The controlled timing workload matrix is `sequential`, `random`, `hot`,
`moderate`, `cold`, `mixed`, and `changing`, each measured under both P3
`LOCAL` and `REMOTE` placement. The benchmark's legacy `--pattern local` is
not in this matrix: implementation assigns both the worker and allocation node
and rejects unequal nodes, while its access loop is sequential. It is therefore
a legacy placement-oriented mode, preserved for direct benchmark use but
semantically redundant and incompatible with the single placement authority
required by controlled P3 calibration.

Thread calibration is deliberately `NOT_IMPLEMENTED`: P3 memory placement alone
does not establish a valid CPU-affinity calibration. Persistent files contain no
page addresses or map ranges.
