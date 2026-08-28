# Phase 5/6 Subprocess and Persistence Investigation

## Methodology

- Production subprocess Phase 4/5/6 path; Phase 4 explicitly uses `subprocess` mode.
- Application counts: 1, 2, 4, 8; workers: 2; discovery interval: 250 ms.
- Workload: calibrated 17,841,231 iterations/thread, one thread/app, 8 MiB/app.
- Three warm-ups and five measured runs per application count.
- Paired/interleaved ordering is not applicable: no candidate cleared the baseline-only optimization gate.
- Measurements are monotonic child-process scopes emitted under `AWAVMA_PROFILE`; no synthetic values.
- A controlled valid-input fixture records Phase 5 computation and all Phase 6 gates that the safety-first runtime workload legitimately short-circuits.

## Operation Timing

| Context | Apps | Phase | Operation | Mean us | Median us | Samples |
|---|---:|---|---|---:|---:|---:|
| FIXTURE | 0 | phase5 | p5_application_state_persist | 62.724 | 62.724 | 1 |
| FIXTURE | 0 | phase5 | p5_decision_compute | 0.245 | 0.245 | 1 |
| FIXTURE | 0 | phase5 | p5_history_cleanup | 133.269 | 133.269 | 1 |
| FIXTURE | 0 | phase5 | p5_input_open_parse | 34.811 | 34.811 | 1 |
| FIXTURE | 0 | phase5 | p5_logging | 38.031 | 38.031 | 1 |
| FIXTURE | 0 | phase5 | p5_output_history_write | 64.134 | 64.134 | 1 |
| FIXTURE | 0 | phase5 | p5_state_load_or_initialize | 292.852 | 292.852 | 1 |
| FIXTURE | 0 | phase5 | p5_state_path_prepare | 56.028 | 56.028 | 1 |
| FIXTURE | 0 | phase5 | phase5_child_api_total | 862.449 | 862.449 | 1 |
| FIXTURE | 0 | phase6 | p6_confidence_gate | 0.679 | 0.509 | 3 |
| FIXTURE | 0 | phase6 | p6_history_cleanup | 136.773 | 129.751 | 5 |
| FIXTURE | 0 | phase6 | p6_human_log_write | 25.490 | 22.372 | 5 |
| FIXTURE | 0 | phase6 | p6_initialize | 1.625 | 1.625 | 1 |
| FIXTURE | 0 | phase6 | p6_input_open_parse | 39.282 | 39.282 | 1 |
| FIXTURE | 0 | phase6 | p6_input_row_parse | 11.390 | 12.086 | 5 |
| FIXTURE | 0 | phase6 | p6_logging_total | 272.781 | 235.754 | 5 |
| FIXTURE | 0 | phase6 | p6_output_history_write | 76.929 | 70.431 | 5 |
| FIXTURE | 0 | phase6 | p6_roi_gate | 0.592 | 0.496 | 3 |
| FIXTURE | 0 | phase6 | p6_safety_gate | 0.774 | 0.734 | 3 |
| FIXTURE | 0 | phase6 | p6_shutdown | 0.401 | 0.401 | 1 |
| FIXTURE | 0 | phase6 | p6_validate_total | 9.797 | 11.364 | 5 |
| FIXTURE | 0 | phase6 | phase6_child_validate_log | 291.848 | 252.918 | 5 |
| MEASURED | 1 | phase5 | p5_application_state_persist | 123.519 | 125.292 | 15 |
| MEASURED | 1 | phase5 | p5_history_cleanup | 256.369 | 246.417 | 15 |
| MEASURED | 1 | phase5 | p5_input_open_parse | 44.029 | 40.017 | 15 |
| MEASURED | 1 | phase5 | p5_logging | 35.035 | 27.580 | 15 |
| MEASURED | 1 | phase5 | p5_output_history_write | 48.021 | 36.513 | 127 |
| MEASURED | 1 | phase5 | p5_state_load_or_initialize | 280.678 | 258.772 | 15 |
| MEASURED | 1 | phase5 | p5_state_path_prepare | 24.169 | 23.567 | 15 |
| MEASURED | 1 | phase5 | phase5_child_api_total | 1475.392 | 1386.428 | 15 |
| MEASURED | 1 | phase6 | p6_human_log_write | 15.127 | 11.317 | 127 |
| MEASURED | 1 | phase6 | p6_initialize | 1.244 | 1.110 | 15 |
| MEASURED | 1 | phase6 | p6_input_open_parse | 38.207 | 34.066 | 15 |
| MEASURED | 1 | phase6 | p6_input_row_parse | 6.693 | 5.394 | 127 |
| MEASURED | 1 | phase6 | p6_logging_total | 92.157 | 54.000 | 127 |
| MEASURED | 1 | phase6 | p6_output_history_write | 45.979 | 30.256 | 127 |
| MEASURED | 1 | phase6 | p6_shutdown | 0.427 | 0.418 | 15 |
| MEASURED | 1 | phase6 | p6_validate_total | 1.368 | 0.985 | 127 |
| MEASURED | 1 | phase6 | phase6_child_validate_log | 101.489 | 65.106 | 127 |
| MEASURED | 2 | phase5 | p5_application_state_persist | 157.788 | 149.481 | 30 |
| MEASURED | 2 | phase5 | p5_history_cleanup | 317.052 | 296.318 | 30 |
| MEASURED | 2 | phase5 | p5_input_open_parse | 44.325 | 43.412 | 30 |
| MEASURED | 2 | phase5 | p5_logging | 37.464 | 33.619 | 30 |
| MEASURED | 2 | phase5 | p5_output_history_write | 61.670 | 59.533 | 250 |
| MEASURED | 2 | phase5 | p5_state_load_or_initialize | 330.037 | 307.065 | 30 |
| MEASURED | 2 | phase5 | p5_state_path_prepare | 27.863 | 26.991 | 30 |
| MEASURED | 2 | phase5 | phase5_child_api_total | 1806.834 | 1871.318 | 30 |
| MEASURED | 2 | phase6 | p6_human_log_write | 18.222 | 14.332 | 250 |
| MEASURED | 2 | phase6 | p6_initialize | 1.307 | 1.232 | 30 |
| MEASURED | 2 | phase6 | p6_input_open_parse | 42.071 | 36.612 | 30 |
| MEASURED | 2 | phase6 | p6_input_row_parse | 7.930 | 7.540 | 250 |
| MEASURED | 2 | phase6 | p6_logging_total | 97.422 | 79.973 | 250 |
| MEASURED | 2 | phase6 | p6_output_history_write | 60.497 | 44.474 | 250 |
| MEASURED | 2 | phase6 | p6_shutdown | 0.511 | 0.518 | 30 |
| MEASURED | 2 | phase6 | p6_validate_total | 1.757 | 1.364 | 250 |
| MEASURED | 2 | phase6 | phase6_child_validate_log | 110.069 | 89.913 | 250 |
| MEASURED | 4 | phase5 | p5_application_state_persist | 183.218 | 179.279 | 79 |
| MEASURED | 4 | phase5 | p5_history_cleanup | 411.993 | 371.567 | 79 |
| MEASURED | 4 | phase5 | p5_input_open_parse | 61.605 | 47.554 | 79 |
| MEASURED | 4 | phase5 | p5_logging | 40.242 | 30.899 | 79 |
| MEASURED | 4 | phase5 | p5_output_history_write | 91.071 | 61.207 | 695 |
| MEASURED | 4 | phase5 | p5_state_load_or_initialize | 421.865 | 303.188 | 79 |
| MEASURED | 4 | phase5 | p5_state_path_prepare | 32.224 | 28.541 | 79 |
| MEASURED | 4 | phase5 | phase5_child_api_total | 2513.874 | 2153.739 | 79 |
| MEASURED | 4 | phase6 | p6_human_log_write | 32.500 | 18.196 | 695 |
| MEASURED | 4 | phase6 | p6_initialize | 2.950 | 1.528 | 79 |
| MEASURED | 4 | phase6 | p6_input_open_parse | 51.813 | 49.755 | 79 |
| MEASURED | 4 | phase6 | p6_input_row_parse | 13.613 | 8.214 | 695 |
| MEASURED | 4 | phase6 | p6_logging_total | 147.848 | 90.976 | 695 |
| MEASURED | 4 | phase6 | p6_output_history_write | 81.434 | 48.859 | 695 |
| MEASURED | 4 | phase6 | p6_shutdown | 0.559 | 0.563 | 79 |
| MEASURED | 4 | phase6 | p6_validate_total | 1.962 | 1.619 | 695 |
| MEASURED | 4 | phase6 | phase6_child_validate_log | 161.642 | 104.125 | 695 |
| MEASURED | 8 | phase5 | p5_application_state_persist | 403.535 | 215.522 | 211 |
| MEASURED | 8 | phase5 | p5_history_cleanup | 581.070 | 432.255 | 211 |
| MEASURED | 8 | phase5 | p5_input_open_parse | 103.442 | 53.784 | 211 |
| MEASURED | 8 | phase5 | p5_logging | 70.642 | 34.472 | 211 |
| MEASURED | 8 | phase5 | p5_output_history_write | 258.515 | 64.214 | 1379 |
| MEASURED | 8 | phase5 | p5_state_load_or_initialize | 796.749 | 307.126 | 211 |
| MEASURED | 8 | phase5 | p5_state_path_prepare | 70.944 | 34.060 | 211 |
| MEASURED | 8 | phase5 | phase5_child_api_total | 4691.028 | 4881.883 | 211 |
| MEASURED | 8 | phase6 | p6_human_log_write | 70.955 | 20.321 | 1379 |
| MEASURED | 8 | phase6 | p6_initialize | 1.820 | 1.780 | 211 |
| MEASURED | 8 | phase6 | p6_input_open_parse | 117.439 | 56.257 | 211 |
| MEASURED | 8 | phase6 | p6_input_row_parse | 34.205 | 9.299 | 1379 |
| MEASURED | 8 | phase6 | p6_logging_total | 317.097 | 97.671 | 1379 |
| MEASURED | 8 | phase6 | p6_output_history_write | 175.561 | 53.837 | 1379 |
| MEASURED | 8 | phase6 | p6_shutdown | 0.608 | 0.598 | 211 |
| MEASURED | 8 | phase6 | p6_validate_total | 6.863 | 1.824 | 1379 |
| MEASURED | 8 | phase6 | phase6_child_validate_log | 371.346 | 111.336 | 1379 |

## Persistence Breakdown

Shares use the matching phase child envelope, so nested Phase 6 logging scopes are not double-counted as independent totals.

| Apps | Phase | Persistence operation | Mean us | Envelope share |
|---:|---|---|---:|---:|
| 1 | phase5 | p5_application_state_persist | 123.519 | 8.372% |
| 1 | phase5 | p5_history_cleanup | 256.369 | 17.376% |
| 1 | phase5 | p5_logging | 35.035 | 2.375% |
| 1 | phase5 | p5_output_history_write | 48.021 | 27.557% |
| 1 | phase5 | p5_state_load_or_initialize | 280.678 | 19.024% |
| 1 | phase5 | p5_state_path_prepare | 24.169 | 1.638% |
| 1 | phase6 | p6_human_log_write | 15.127 | 14.905% |
| 1 | phase6 | p6_output_history_write | 45.979 | 45.305% |
| 2 | phase5 | p5_application_state_persist | 157.788 | 8.733% |
| 2 | phase5 | p5_history_cleanup | 317.052 | 17.547% |
| 2 | phase5 | p5_logging | 37.464 | 2.073% |
| 2 | phase5 | p5_output_history_write | 61.670 | 28.443% |
| 2 | phase5 | p5_state_load_or_initialize | 330.037 | 18.266% |
| 2 | phase5 | p5_state_path_prepare | 27.863 | 1.542% |
| 2 | phase6 | p6_human_log_write | 18.222 | 16.555% |
| 2 | phase6 | p6_output_history_write | 60.497 | 54.963% |
| 4 | phase5 | p5_application_state_persist | 183.218 | 7.288% |
| 4 | phase5 | p5_history_cleanup | 411.993 | 16.389% |
| 4 | phase5 | p5_logging | 40.242 | 1.601% |
| 4 | phase5 | p5_output_history_write | 91.071 | 31.871% |
| 4 | phase5 | p5_state_load_or_initialize | 421.865 | 16.781% |
| 4 | phase5 | p5_state_path_prepare | 32.224 | 1.282% |
| 4 | phase6 | p6_human_log_write | 32.500 | 20.107% |
| 4 | phase6 | p6_output_history_write | 81.434 | 50.379% |
| 8 | phase5 | p5_application_state_persist | 403.535 | 8.602% |
| 8 | phase5 | p5_history_cleanup | 581.070 | 12.387% |
| 8 | phase5 | p5_logging | 70.642 | 1.506% |
| 8 | phase5 | p5_output_history_write | 258.515 | 36.016% |
| 8 | phase5 | p5_state_load_or_initialize | 796.749 | 16.985% |
| 8 | phase5 | p5_state_path_prepare | 70.944 | 1.512% |
| 8 | phase6 | p6_human_log_write | 70.955 | 19.108% |
| 8 | phase6 | p6_output_history_write | 175.561 | 47.277% |

## Safety Classification

| Phase | Classification | Reason | Decision |
|---|---|---|---|
| Phase 5 | PER_APPLICATION_SAFE with isolated paths | State/history/log/output must remain isolated by app_id; same-tree writes are unsafe. | Retain subprocess baseline; no concurrency change. |
| Phase 6 | SERIALIZED_REQUIRED in-process | `Validation_Init`, active configuration, and logger state are process-global. | Retain subprocess isolation and serialized coordinator. |

## Candidate Decision

No Phase 5/6 candidate was introduced. At eight apps, Phase 5's child envelope averages 4691.028 us and Phase 6's averages 371.346 us; Phase 5 state/history/output operations and Phase 6 logging dominate their respective profiled child work, while the valid-input Phase 6 gates are sub-microsecond. Reducing those costs safely would require a persistence or lifecycle semantic change, so the data does not justify an in-process or concurrency candidate. A future candidate requires a design that preserves per-app persistence isolation, Phase 6 lifecycle semantics, and whole-system throughput.

| Apps | Comparison status | Notes |
|---:|---|---|
| 1 | NO_CANDIDATE | Baseline-only investigation; no Phase 5/6 candidate introduced. |
| 2 | NO_CANDIDATE | Baseline-only investigation; no Phase 5/6 candidate introduced. |
| 4 | NO_CANDIDATE | Baseline-only investigation; no Phase 5/6 candidate introduced. |
| 8 | NO_CANDIDATE | Baseline-only investigation; no Phase 5/6 candidate introduced. |

## Integrity

No scheduler, worker default, Phase 4 production mode, migration execution, feedback behavior, persistence format, or validation gate policy changed for this investigation.
