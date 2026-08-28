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
| sequential | 139.416 | 141.004 | 1.139 | 147.956 | 15.820 | -0.802 |
| random | 241.532 | 279.480 | 15.711 | 267.361 | 34.021 | -12.870 |
| hot | 203.663 | 189.735 | -6.839 | 187.337 | 18.830 | 7.318 |
| moderate | 286.278 | 311.028 | 8.645 | 324.281 | 44.396 | -8.031 |
| cold | 282.677 | 305.176 | 7.959 | 299.012 | 35.487 | -7.430 |
| mixed | 245.896 | 265.825 | 8.104 | 262.477 | 24.769 | -7.567 |
| changing | 374.688 | 405.633 | 8.259 | 395.498 | 23.304 | -7.657 |
| local | 849.249 | 933.516 | 9.923 | 1051.161 | 232.994 | -10.434 |

## E. Multi-App Results

| Apps | Aggregate Baseline Throughput | AWAVMA Throughput | Change % | Runtime CPU | Serial Wait us | Queue Wait us |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 4299031.715 | 4070252.114 | -5.322 | 26.017 | 367.137 | 302.237 |
| 2 | 7615075.278 | 7392744.445 | -2.920 | 29.148 | 12020.758 | 744.264 |
| 4 | 11580950.311 | 11489232.284 | -0.792 | 31.232 | 52260.223 | 3536.512 |
| 8 | 13357308.216 | 13174063.002 | -1.372 | 20.114 | 193588.948 | 11587.607 |

## Final Fairness

| Apps | Jain Index | Min Cycles/App | Max Cycles/App | Worst Evaluation Gap ms | Starvation? |
|---:|---:|---:|---:|---:|---|
| 1 | 1.000 | 4.000 | 4.000 | 1051.744 | NO EVIDENCE |
| 2 | 0.998 | 4.000 | 5.000 | 1090.715 | NO EVIDENCE |
| 4 | 0.998 | 4.000 | 5.000 | 1230.985 | NO EVIDENCE |
| 8 | 1.000 | 5.000 | 5.000 | 1648.249 | NO EVIDENCE |

## F. Resource Usage

| Apps | CPU Mean % | CPU Peak % | RSS Mean KiB | RSS Peak KiB | VMS Mean KiB | VMS Peak KiB |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 26.017 | 189.419 | 6048.000 | 6220.000 | 156804.000 | 156804.000 |
| 2 | 29.148 | 182.488 | 6063.200 | 6196.000 | 156779.200 | 156784.000 |
| 4 | 31.232 | 247.173 | 6140.800 | 6308.000 | 156800.000 | 156800.000 |
| 8 | 20.114 | 201.524 | 6267.200 | 6316.000 | 158717.600 | 166244.000 |

## G. Pipeline Breakdown

| Component | Mean us | Median us | Notes |
|---|---:|---:|---|
| Discovery | 3278029.948 | 3271898.209 | Run-level total; not additive with per-scan work. Nested scopes are not additive. |
| Queue wait | 11587.607 | 11354.448 | Queue-to-worker delay. Nested scopes are not additive. |
| Worker execution | 6761.135 | 6762.685 | Worker wrapper, includes Phase 3. Nested scopes are not additive. |
| Phase 3 | 6740.313 | 6756.542 | Monitor execution. Nested scopes are not additive. |
| Serial-entry wait | 193588.948 | 194144.209 | Serialized coordinator admission. Nested scopes are not additive. |
| Phase 4 subprocess wall | 16417.578 | 16558.138 | Parent wall time. Nested scopes are not additive. |
| Phase 5 subprocess wall | 19086.273 | 18908.354 | Parent wall time. Nested scopes are not additive. |
| Phase 6 subprocess wall | 16624.410 | 16794.867 | Parent wall time. Nested scopes are not additive. |
| Coordinator overhead | 1840.256 | NA | Coordinator wrapper minus Phase 4-6 means. Nested scopes are not additive. |
| Completion latency | 18432.961 | 19152.656 | Submit to completed job. Nested scopes are not additive. |

## H. Discovery Results

- Production discovery remains 250 ms. The earlier controlled cadence study measured 829.2 to 286.6 ms/run total discovery time (65.4% reduction), controlled CPU 68.3% to 26.5%, and 249.4 ms mean detection latency. It is retained separately from this short-run final measurement.
- Final 8-app runs: mean scans/run 20.000, mean scan time 163901.497 us, mean total discovery time/run 3278029.948 us, and 44.532% of summed runtime-cycle wall time (non-additive attribution).

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
| 1 | 0.000 | 0.000 | 0.000 | 4497.063 |
| 2 | 1.000 | 0.000 | 0.000 | 9395.870 |
| 4 | 3.000 | 0.000 | 0.000 | 17106.926 |
| 8 | 7.000 | 0.000 | 0.000 | 54252.480 |

## M. Runtime Outcome Counts

| Outcome | Count | Status |
|---|---:|---|
| INSUFFICIENT | 78 | MEASURED |
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

1. Serial-entry wait: 193588.948 us (final 8-app production measurement).
2. Phase 5 subprocess wall: 19086.273 us (final 8-app production measurement).
3. Completion latency: 18432.961 us (final 8-app production measurement).
4. Phase 6 subprocess wall: 16624.410 us (final 8-app production measurement).
5. Phase 4 subprocess wall: 16417.578 us (final 8-app production measurement).
6. Queue wait: 11587.607 us (final 8-app production measurement).
7. Worker execution: 6761.135 us (final 8-app production measurement).
8. Phase 3: 6740.313 us (final 8-app production measurement).
9. Coordinator overhead: 1840.256 us (final 8-app production measurement).

## P. Final Phase 10 Runtime Conclusion

The accepted AWAVMA runtime overhead and scalability were characterized on the current single-NUMA-node environment. Comparative benefit over conventional multi-node NUMA placement remains to be evaluated on suitable 2+ node hardware.
