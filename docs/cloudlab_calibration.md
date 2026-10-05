# CloudLab NUMA calibration

P4-C is separate from final P7 evaluation. It collects controlled P3 placement
evidence and controlled, rollback-required migration-cost measurements; it never
enables normal AWAVMA production migration execution.

1. Build `make benchmark calibration-validate bin/p4c-migration-cost-collector` on a suitable CloudLab node.
2. Run `tools/collect_numa_calibration.sh --smoke` and inspect its retained
   placement CSVs and `manifest.json`.
3. Only after a successful smoke, run `tools/collect_numa_calibration.sh --full`.
4. Supply the retained raw timing and authenticated 4096-page cost CSVs to
   `tools/build_numa_calibration.py`, then run `bin/calibration-validate ARTIFACT`.

The collector defaults to smoke. Smoke performs one authenticated exact-4096-page
`move_pages` transaction and retains its row as a warmup, so it cannot contribute
to a calibration artifact. Full collection requires a smoke manifest and retains
two successful warmups plus seven successful measured samples; invalid attempts
are retained but excluded, with bounded retries.
Smoke is always labelled `NOT_PRODUCTION_CALIBRATION`; it is not a production
artifact. The builder requires at least seven valid pairs and seven exact,
successful 4096-page `move_pages` cost observations for every production record.

The paired lower bound and migration cost upper bound use a one-sided 95% Student
t interval. `uncertainty_pct` is zero because those bounds already represent the
statistical uncertainty; P4-C does not invent a second uncertainty percentage.
`safety_margin_pct` is an explicit, configurable policy value (default 1%).

Thread calibration is deliberately `NOT_IMPLEMENTED`: P3 memory placement alone
does not establish a valid CPU-affinity calibration. Persistent files contain no
page addresses or map ranges.
