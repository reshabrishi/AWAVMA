# Final Integrated Runtime Performance Report

## A. Exact Production Configuration

- Phase 4/5/6: subprocess; coordinator: serialized; scheduler: FIFO; workers: 2.
- Discovery: 250 ms; monitor: 100 ms; evaluation: 1000 ms; Phase 4 in-process mode was not used.

## B. Methodology

- Single app: eight workloads, 2 threads, 8 MiB, 1,000,000 iterations, seed 12345; five warm-ups and five deterministic interleaved baseline/integrated pairs.
- Multi-app: mixed, one thread/app, 8 MiB/app, fixed calibrated 17,841,231 iterations/thread; 1/2/4/8 apps, two workers, three warm-ups and five deterministic interleaved pairs.
- Values are observed measurements, not statistical-significance claims. The short single-app workload can legitimately leave some 1000-ms evaluation/pipeline fields unavailable.

## C. Regression Results

| Command | Result | Notes |
|---|---|---|
| make test-runtime | PASS | ./bin/awavma-runtime-test | AR01: PASS | AR02: PASS | AR03: PASS | AR04: PASS | AR05: PASS | AR06: PASS | AR07: PASS | ./bin/runtime-target-filter-test | RTF01: PASS | RTF02: PASS | RTF03: PASS | RTF04: PASS | python3 tests/runtime_target_filter_cli_test.py | RTFCLI01: PASS | RTFCLI02: PASS | RTFCLI03: PASS | RTFCLI04: PASS |
| make test-discovery-cadence | PASS | ./bin/discovery-cadence-test | DC00: PASS | DC03: PASS | DC01: PASS | DC02: PASS | DC04: PASS | DC07: PASS | DC08: PASS | DC05: PASS | DC09: PASS | DC06: PASS | Discovery cadence tests: PASS | python3 tests/discovery_cadence_cli_test.py | DC10[0]: PASS | DC10[-1]: PASS | DC10[invalid]: PASS | DC10[999999999999999999999999999999999999]: PASS |
| make test-continuous-monitor | FAIL | : PASS | W07: PASS | W08: PASS | W09: PASS | W10: PASS | W11: PASS | W12: PASS | W13: PASS | W14: PASS | W15: PASS | W16: PASS | W17: PASS | W18: PASS | W19: PASS | W20: PASS | W21: PASS | W22: PASS | W23: PASS | W24: PASS | W25: PASS | Worker pool tests: PASS | make[1]: Leaving directory '/home/rishi/major_project' | AWAVMA_REGRESSION_VERIFIED=1 ./bin/continuous-monitor-test | CM01: PASS | CM02: PASS | CM03: PASS | CM04: PASS | CM05: PASS | CM06: PASS | CM07: PASS | CM08: PASS | CM09: PASS | CM10: FAIL | CM13: PASS | CM14: PASS | CM16: PASS | CM17: PASS | CM20: PASS | CM21: PASS | CM22: PASS | CM18: PASS | CM19: PASS | CM11: PASS | CM12: PASS | CM12-queue: PASS | CM15-A: PASS | CM15-B: PASS | CM15-C: PASS | CM15-D: PASS | CM15-E: PASS | CM23: PASS | CM24: PASS | CM25: PASS | Continuous monitoring tests: FAIL | Error: target PID 275135 is unavailable for one sample. | CM10 diagnostic: status=TARGET_GONE submissions=10 before=8 | make: *** [Makefile:139: test-continuous-monitor] Error 1 |
| make test-continuous-monitor (rerun) | PASS | first failure:  | CM15-C: PASS | CM15-D: PASS | CM15-E: PASS | CM23: PASS | CM24: PASS | CM25: PASS | Continuous monitoring tests: FAIL | Error: target PID 275135 is unavailable for one sample. | CM10 diagnostic: status=TARGET_GONE submissions=10 before=8 | make: *** [Makefile:139: test-continuous-monitor] Error 1 | rerun: g directory '/home/rishi/major_project' | AWAVMA_REGRESSION_VERIFIED=1 ./bin/continuous-monitor-test | CM01: PASS | CM02: PASS | CM03: PASS | CM04: PASS | CM05: PASS | CM06: PASS | CM07: PASS | CM08: PASS | CM09: PASS | CM10: PASS | CM13: PASS | CM14: PASS | CM16: PASS | CM17: PASS | CM20: PASS | CM21: PASS | CM22: PASS | CM18: PASS | CM19: PASS | CM11: PASS | CM12: PASS | CM12-queue: PASS | CM15-A: PASS | CM15-B: PASS | CM15-C: PASS | CM15-D: PASS | CM15-E: PASS | CM23: PASS | CM24: PASS | CM25: PASS | Continuous monitoring tests: PASS | Error: target PID 275250 is unavailable for one sample. |
| make test-validation | PASS | sh tests/run_validation_fixtures.sh | Validation fixtures passed. |
| make test-feedback | PASS | F25 | invalid state preserved and rejected | FEEDBACK_INVALID_STATE | PASS | F26 | 3 feedback records retained | header plus 3 records | PASS | F27 | original history preserved on compaction failure | original retained | PASS | F28 | identical event/state is deterministic | identical reward and state | PASS | I01 | approved beneficial memory integration | FEEDBACK_UPDATED | PASS | I02 | approved harmful memory integration | FEEDBACK_UPDATED | PASS | I03 | approved beneficial thread integration | FEEDBACK_UPDATED | PASS | I04 | approved harmful thread integration | FEEDBACK_UPDATED | PASS | I05 | real insufficient pipeline produces no learning | FEEDBACK_NO_UPDATE | PASS | ADAPT | APP_A trends up and APP_B trends down | opposite isolated trends | PASS | STABILITY | finite bounded opposite movement | positive then negative bounded | PASS | LEARNING_RATE | 0.01 < 0.05 < 0.10 update magnitude | monotonic update magnitude | PASS | sh tests/run_feedback_cli.sh | Feedback CLI fixtures passed. |
| make test-migration | PASS | ry rejection | not safely reproduced | NOT TESTED — ENVIRONMENT LIMITATION | T23 | MIGRATION_TARGET_GONE | MIGRATION_TARGET_GONE | PASS | T24 | MIGRATION_TARGET_GONE | MIGRATION_TARGET_GONE | PASS | T25 | successful migration CSV record | record present | PASS | T26 | failed migration CSV record | record present | PASS | T27 | human-readable migration log | record present | PASS | T28 | 3 records retained | 3 records retained | PASS | T29 | old record removed, new retained | cleanup executed | PASS | T30 | original history preserved on cleanup failure | original retained | PASS | T31 | MIGRATION_SUCCESS | MIGRATION_SUCCESS | PASS | T32 | MIGRATION_NOT_AUTHORIZED and unchanged affinity | MIGRATION_NOT_AUTHORIZED | PASS | T33 | remote NUMA migration only with >=2 nodes | 1 NUMA node | NOT TESTED — ENVIRONMENT LIMITATION | T34 overhead | expected execution overhead measurements | min=0.028 ms max=0.038 ms avg=0.031 ms | PASS | sh tests/run_migration_cli.sh | Migration CLI fixtures passed. |
| make test-phase46-pipeline | PASS | cc -std=c11 -O2 -Wall -Wextra -Wpedantic -D_GNU_SOURCE -Iinclude -DAWAVMA_PROFILE -o bin/profile-awavma-runtime src/awavma_runtime.c src/awavma_runtime_main.c src/runtime_target_filter.c src/classifier.c src/runtime_monitor.c src/application_manager.c src/application_discovery.c src/worker_pool.c src/monitor.c src/monitor_profile.c -pthread  -lm | python3 tests/phase46_pipeline_test.py | P46-01: PASS | P46-02: PASS | P46-03: PASS | P46-04: PASS | P46-05: PASS | P46-06: PASS | P46-07: NOT APPLICABLE (pipeline concurrency not enabled) | P46-08: NOT APPLICABLE (Phase 5 remains a subprocess) | P46-09: PASS | P46-10: PASS |
| make test-phase56-subprocess-profile | PASS | python3 scripts/generate_phase56_investigation_graphs.py --root /home/rishi/major_project | Phase56 investigation graphs: generated=5 no_data=2 | python3 tests/phase56_subprocess_profile_test.py | P56-01: PASS | P56-02: PASS | P56-03: PASS | P56-04: PASS | P56-05: PASS | P56-06: PASS |
| python3 tests/final_integration_test.py | PASS | Final integration results: 81 records, failures=0 |
| python3 tests/system_regression_test.py | PASS | System regression: total=54 pass=53 fail=0 not_tested=1 not_applicable=0 |

## D. Single-App Results

| Workload | Baseline Mean ms | Final AWAVMA Mean ms | Overhead % | Final Median ms | Final Stddev ms | Throughput Change % |
|---|---:|---:|---:|---:|---:|---:|
| sequential | 106.773 | 111.759 | 4.669 | 108.063 | 12.747 | -3.769 |
| random | 186.163 | 168.984 | -9.228 | 177.161 | 46.350 | 18.180 |
| hot | 135.908 | 126.845 | -6.668 | 131.947 | 14.173 | 7.922 |
| moderate | 206.938 | 186.999 | -9.635 | 185.605 | 26.833 | 11.889 |
| cold | 190.217 | 148.246 | -22.065 | 115.986 | 62.365 | 45.689 |
| mixed | 179.235 | 172.933 | -3.516 | 175.145 | 11.870 | 3.884 |
| changing | 198.652 | 169.521 | -14.664 | 126.068 | 62.367 | 18.282 |
| local | 99.341 | 104.651 | 5.346 | 103.650 | 12.863 | -4.122 |

## E. Multi-App Results

| Apps | Aggregate Baseline Throughput | AWAVMA Throughput | Change % | Runtime CPU | Serial Wait us | Queue Wait us |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 14347673.834 | 14623425.363 | 1.922 | 12.349 | 92.814 | 77.632 |
| 2 | 25636069.938 | 24859439.138 | -3.029 | 13.143 | 3394.895 | 78.592 |
| 4 | 41722739.113 | 39927619.119 | -4.302 | 16.771 | 12327.980 | 775.221 |
| 8 | 47440741.020 | 44211871.417 | -6.806 | 17.081 | 78819.433 | 3909.145 |

## Final Fairness

| Apps | Jain Index | Min Cycles/App | Max Cycles/App | Worst Evaluation Gap ms | Starvation? |
|---:|---:|---:|---:|---:|---|
| 1 | 1.000 | 1.000 | 1.000 | NA | NO EVIDENCE |
| 2 | 1.000 | 1.000 | 1.000 | NA | NO EVIDENCE |
| 4 | 1.000 | 1.000 | 1.000 | NA | NO EVIDENCE |
| 8 | 0.995 | 2.000 | 3.000 | 1215.738 | NO EVIDENCE |

## F. Resource Usage

| Apps | CPU Mean % | CPU Peak % | RSS Mean KiB | RSS Peak KiB | VMS Mean KiB | VMS Peak KiB |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 12.349 | 193.070 | 6504.000 | 6756.000 | 156988.000 | 157140.000 |
| 2 | 13.143 | 192.091 | 6495.200 | 6556.000 | 156932.000 | 156956.000 |
| 4 | 16.771 | 187.922 | 6595.200 | 6676.000 | 157004.800 | 157008.000 |
| 8 | 17.081 | 187.625 | 6684.000 | 6828.000 | 156964.800 | 156988.000 |

## G. Pipeline Breakdown

| Component | Mean us | Median us | Notes |
|---|---:|---:|---|
| Discovery | 664854.353 | 673828.239 | Run-level total; not additive with per-scan work. Nested scopes are not additive. |
| Queue wait | 3909.145 | 3654.309 | Queue-to-worker delay. Nested scopes are not additive. |
| Worker execution | 2289.458 | 1714.732 | Worker wrapper, includes Phase 3. Nested scopes are not additive. |
| Phase 3 | 2286.856 | 1711.977 | Monitor execution. Nested scopes are not additive. |
| Serial-entry wait | 78819.433 | 81051.598 | Serialized coordinator admission. Nested scopes are not additive. |
| Phase 4 subprocess wall | 6336.331 | 6553.062 | Parent wall time. Nested scopes are not additive. |
| Phase 5 subprocess wall | 7601.828 | 7603.971 | Parent wall time. Nested scopes are not additive. |
| Phase 6 subprocess wall | 6756.021 | 6497.920 | Parent wall time. Nested scopes are not additive. |
| Coordinator overhead | 437.417 | NA | Coordinator wrapper minus Phase 4-6 means. Nested scopes are not additive. |
| Completion latency | 6236.798 | 6605.616 | Submit to completed job. Nested scopes are not additive. |

## H. Discovery Results

- Production discovery remains 250 ms. The earlier controlled cadence study measured 829.2 to 286.6 ms/run total discovery time (65.4% reduction), controlled CPU 68.3% to 26.5%, and 249.4 ms mean detection latency. It is retained separately from this short-run final measurement.
- Final 8-app runs: mean scans/run 12.000, mean scan time 55404.529 us, mean total discovery time/run 664854.353 us, and 19.648% of summed runtime-cycle wall time (non-additive attribution).

## I. Phase 4-6 Findings

- Production remains serialized subprocess execution. Parent subprocess wall minus child API work is termed subprocess startup/uninstrumented residual, not pure fork cost.
- Completed Phase 5/6 investigation at eight apps: Phase 5 child envelope about 4691 us, dominated by persistence; Phase 6 child envelope about 371 us, dominated by logging/lifecycle; controlled valid gate compute is sub-microsecond.

## J. Accepted Optimizations

- Discovery cadence of 250 ms is accepted. The bounded two-worker FIFO Phase 3 pool is retained.

## K. Rejected Optimizations

- Phase 4 in-process direct call: local Phase 4 work improved about 12.6 ms to 0.9 ms, but whole-pipeline throughput changed -7.332%, -23.822%, -21.803%, and -26.017% at 1/2/4/8 apps. REJECTED FOR PRODUCTION.

## L. Scheduler/Worker Conclusion

- Two workers remain the default. Final queue/fairness evidence is below; Priority + Aging + FIFO remains unjustified absent saturation, deferrals, or starvation.

| Apps | Max Queue Depth | Deferrals | Saturation | Max Queue Wait us |
|---:|---:|---:|---:|---:|
| 1 | 0.000 | 0.000 | 0.000 | 442.345 |
| 2 | 1.000 | 0.000 | 0.000 | 1405.963 |
| 4 | 3.000 | 0.000 | 0.000 | 4625.766 |
| 8 | 7.000 | 0.000 | 0.000 | 41779.572 |

## M. Runtime Outcome Counts

| Outcome | Count | Status |
|---|---:|---|
| INSUFFICIENT | 75 | MEASURED |
| NO_MIGRATION | 0 | NOT EXECUTED |
| VALIDATION_REJECTED | 0 | NOT EXECUTED |
| VALIDATION_APPROVED | 0 | NOT EXECUTED |
| MIGRATION_EXECUTED | 0 | NOT EXECUTED |
| FEEDBACK_PENDING | 0 | NOT EXECUTED |
| FEEDBACK_APPLIED | 0 | NOT EXECUTED |

## N. Environment Limitations

- CPUs: 8; NUMA nodes: node0 only; libnuma available; numa.h and numaif.h unavailable.
- Remote NUMA migration was not demonstrated; perf/cache counters are permission-limited; the benchmark binary is frozen because benchmark source needs unavailable numa.h.

## O. Bottleneck Ranking

1. Serial-entry wait: 78819.433 us (final 8-app production measurement).
2. Phase 5 subprocess wall: 7601.828 us (final 8-app production measurement).
3. Phase 6 subprocess wall: 6756.021 us (final 8-app production measurement).
4. Phase 4 subprocess wall: 6336.331 us (final 8-app production measurement).
5. Completion latency: 6236.798 us (final 8-app production measurement).
6. Queue wait: 3909.145 us (final 8-app production measurement).
7. Worker execution: 2289.458 us (final 8-app production measurement).
8. Phase 3: 2286.856 us (final 8-app production measurement).
9. Coordinator overhead: 437.417 us (final 8-app production measurement).

## P. Final Phase 10 Runtime Conclusion

The accepted AWAVMA runtime overhead and scalability were characterized on the current single-NUMA-node environment. Comparative benefit over conventional multi-node NUMA placement remains to be evaluated on suitable 2+ node hardware.
