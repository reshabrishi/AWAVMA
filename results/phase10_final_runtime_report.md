# Phase 10 Final Runtime Checkpoint

This report consolidates the original Phase 10 launcher/serialized measurement, monitoring profiling, discovery cadence optimization, integrated runtime, multi-app scaling, Phase 4-6 and Phase 5/6 investigations, and the final accepted re-profile.

## Historical Stages

- Stage 1: original launcher/serialized measurement reported sequential +21.74%, random +20.38%, hot +13.68%, moderate +6.37%, cold +29.17%, mixed +6.36%, changing +3.74%, local +24.98%. It was not the full production runtime.
- Stage 2: first integrated runtime pooled observed overhead was +6.411% before the accepted sequence was consolidated.
- Stage 3: final accepted production re-profile observed an unweighted mean workload overhead of -6.970%. It is reported as observed, not a significance claim; methodologies are not treated as directly causal.

## Accepted Architecture

- Production remains Phase 4/5/6 subprocesses, a serialized coordinator, FIFO scheduling, two Phase 3 workers, monitor interval 100 ms, evaluation interval 1000 ms, and discovery interval 250 ms.
- Discovery cadence is the accepted optimization: the separate controlled study reduced total discovery time from 829.2 to 286.6 ms/run (65.4%) with 249.4 ms mean detection latency. The final run retains it without conflating methodologies.

## Phase Investigations

- Phase 4 in-process direct call was rejected for production: local work fell about 12.6 ms to 0.9 ms, while whole-pipeline throughput regressed -7.332%, -23.822%, -21.803%, and -26.017% at 1/2/4/8 apps.
- Phase 5/6 remain subprocesses. At eight apps, Phase 5 child work was about 4691 us and persistence-dominated; Phase 6 child work was about 371 us and logging/lifecycle-dominated; controlled valid gate compute was sub-microsecond.

## Final Evidence

- Final source data: `final_integrated_performance_raw.csv`, `final_integrated_multiapp_raw.csv`, final summaries, graph manifest, and regression ledger.
- Final report: `final_integrated_performance_report.md`, including resource cost, fairness, queue/scheduler evidence, pipeline attribution, outcome counts, and bottleneck ranking.
- Phase 7 was not enabled without legitimate migration metadata. Phase 8 was not executed without valid later before/after observations.

## Boundary

The accepted AWAVMA runtime overhead and scalability were characterized on the current one-node NUMA environment. No comparative NUMA-placement or remote-locality claim is made; that work requires suitable 2+ node hardware and a traditional NUMA baseline.
