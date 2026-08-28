# Phase 4-6 Pipeline Investigation

## Concurrency Audit

| Phase | State model | Current subprocess? | Thread-safe? | Optimization chosen |
|---|---|---|---|---|
| Coordinator | runtime records | n/a | SERIALIZED | profiling only |
| Phase 4 | run-local | yes | PER_APPLICATION_SAFE | experimental in-process API rejected by reprofile |
| Phase 5 | run-local plus per-app persistence | yes | PER_APPLICATION_SAFE | none before profile gate |
| Phase 6 | process-global config/logger | yes | SERIALIZED_REQUIRED in-process | retain subprocess isolation |

## Scaling Before/After

| Apps | Before Serial Wait | After Serial Wait | Before Pipeline | After Pipeline | Throughput Change |
|---:|---:|---:|---:|---:|---:|
| 1 | 221.198 | 243.413 | 53459.130 | 53706.451 | -7.332% |
| 2 | 9358.459 | 8133.029 | 108081.043 | 134370.233 | -23.822% |
| 4 | 31369.024 | 35596.209 | 318809.262 | 448416.175 | -21.803% |
| 8 | 150745.295 | 126205.613 | 1645886.385 | 2020568.612 | -26.017% |

## Decision

The Phase 4 direct-call experiment removes its measured subprocess launch wall time, but the complete candidate reprofile regressed total pipeline wall time and aggregate throughput at multi-application scale. It is not the runtime default; subprocess mode remains production behavior, while the explicit in-process switch remains available only for investigation. Phase 5 and Phase 6 remain subprocesses, the coordinator remains serialized, and scheduler, worker-default, migration, and feedback safety are unchanged.
