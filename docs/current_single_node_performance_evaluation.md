# Current Single-Node Performance Evaluation of AWAVMA

## 1. Introduction

Adaptive Workload-Aware Virtual Memory Allocation for Multi-Core Architectures (AWAVMA) is a runtime that discovers applications, monitors their behavior, classifies workload evidence, makes allocation-related decisions, validates those decisions, and applies migration and feedback only when the required evidence is valid.

Before evaluating any potential NUMA-locality benefit, the runtime cost of this integrated control path must be measured. The current evaluation was therefore performed on a machine with a single NUMA node. It measures AWAVMA runtime overhead and multi-application scalability, not remote-memory behavior. A final AWAVMA-versus-conventional-NUMA comparison will be performed later on suitable multi-node NUMA hardware.

## 2. Current Experimental Environment

| Item | Current environment | Evaluation implication |
|---|---|---|
| CPUs | 8 | Supports controlled concurrent application tests. |
| NUMA topology | 1 node (`node0`) | No remote NUMA node is available. |
| `libnuma` | Available | Basic NUMA library support is present. |
| `numa.h` | Unavailable | Some NUMA-dependent benchmark and API paths cannot be built. |
| `numaif.h` | Unavailable | NUMA memory-policy interfaces are unavailable. |
| Remote NUMA migration | Not demonstrated | A cross-node migration result cannot be measured. |
| perf/cache counters | Unavailable due to permission restrictions | Cache-counter-based analysis cannot be reported. |

These limitations prevent a genuine remote NUMA performance comparison. In particular, a single node provides no remote placement target, and unavailable headers and counter permissions restrict validation of NUMA policy, migration, and cache behavior.

## 3. Current Accepted AWAVMA Runtime Configuration

The accepted production configuration is:

| Runtime component | Accepted configuration |
|---|---|
| Phase 4 | Subprocess |
| Phase 5 | Subprocess |
| Phase 6 | Subprocess |
| Coordinator | Serialized |
| Scheduler | FIFO |
| Worker pool | 2 workers |
| Discovery interval | 250 ms |
| Runtime mode | Foreground `bin/awavma-runtime` |

## 4. What Is Being Compared

### Baseline

The application or benchmark runs normally without the AWAVMA runtime.

### Integrated AWAVMA

The same application or benchmark runs while the accepted AWAVMA runtime is active.

> This comparison measures AWAVMA runtime overhead and scalability on the same single-node system. It does not compare AWAVMA against conventional multi-node NUMA allocation or migration.

## 5. Single-Application Performance

**Key result: unweighted mean execution-time overhead: +6.613%.**

Across the eight controlled workloads, the accepted AWAVMA runtime increased execution time by approximately 6.613% on average compared with running the benchmark alone. This is an unweighted mean of the workload-level execution-time overheads from the final integrated runtime evaluation.

This value is the observed cost of the active runtime, including:

- application discovery;
- monitoring;
- classification;
- decision processing;
- validation;
- runtime coordination; and
- persistence.

It is not a claim that AWAVMA is 6.613% slower than NUMA. The present machine cannot support that comparison.

## 6. Multi-Application Performance

| Applications | Throughput Change with AWAVMA |
|---:|---:|
| 1 | -5.322% |
| 2 | -2.920% |
| 4 | -0.792% |
| 8 | -1.372% |

Negative values represent runtime overhead relative to the corresponding baseline. The relative throughput impact remained small at higher application counts: four and eight simultaneous applications showed less than approximately 1.5% measured aggregate throughput loss. These are measured values and are not claims of statistical significance.

## 7. Fairness and Worker Pool Behavior

The final eight-application measurement recorded:

| Metric | Result |
|---|---:|
| Jain fairness index | 1.000 |
| Queue-full saturation events | 0 |
| Deferrals | 0 |
| Starvation evidence | None observed |

The bounded two-worker pool serviced all eight applications evenly in the measured experiment. Four workers reduced queue wait, but did not materially improve eight-application aggregate throughput. Therefore, two workers remain the accepted production default.

## 8. Discovery Optimization

Discovery and monitoring cadence were separated so that repeated `/proc` scanning could be reduced without reducing the monitoring cadence. The previous controlled discovery cadence was 50 ms; the accepted production cadence is 250 ms.

| Metric | 50 ms cadence | 250 ms cadence | Result |
|---|---:|---:|---|
| Total discovery time | 829.2 ms/run | 286.6 ms/run | 65.4% reduction |
| Runtime CPU | 68.3% | 26.5% | Lower measured runtime CPU |
| Mean application detection latency | - | 249.4 ms | Within the <=350 ms acceptance gate |

The optimization was accepted because it substantially reduced repeated discovery work while retaining an acceptable measured process-detection latency.

## 9. Comparison With Earlier Phase 10 Results

The earlier launcher-based Phase 10 measurements were:

| Workload | Earlier Overhead |
|---|---:|
| sequential | +21.74% |
| random | +20.38% |
| hot | +13.68% |
| moderate | +6.37% |
| cold | +29.17% |
| mixed | +6.36% |
| changing | +3.74% |
| local | +24.98% |

These earlier values were produced before the final integrated runtime architecture and therefore should not be interpreted as final AWAVMA production overhead. The final integrated runtime result is an unweighted mean single-application execution-time overhead of **+6.613%**.

The project progressively reduced and better characterized runtime overhead through profiling and architectural optimization. No percentage improvement between the two result sets is calculated here because the methodologies are not directly comparable.

## 10. Current Runtime Architecture Summary

```text
Linux Applications
        |
        v
Application Discovery
        |
        v
Application Manager
        |
        v
Runtime Monitor / FIFO Scheduler
        |
        v
Bounded Worker Pool - 2 workers
        |
        v
Parallel Phase 3 Monitoring
        |
        v
Serialized Runtime Coordinator
        |
        v
Phase 4 Classification
        |
        v
Phase 5 Decision Engine
        |
        v
Phase 6 Validation
        |
        v
Phase 7 only if legitimate migration evidence exists
        |
        v
Phase 8 only after a valid later observation
```

Phase 3 monitoring is parallelized through the bounded worker pool. Phases 4 through 6 are currently serialized by the runtime coordinator to protect shared state, persistence, and lifecycle boundaries.

## 11. Current Main Bottleneck

The final eight-application measurement identified the following relevant latencies:

| Metric | Measured value |
|---|---:|
| Serial-entry wait | 193,588.948 us (approximately 193.6 ms) |
| Phase 5 subprocess wall | 19,086.273 us (approximately 19.1 ms) |

The current main scalability limitation is waiting to enter the serialized Phase 4-6 pipeline, not worker scheduling or queue saturation.

## 12. Why Scheduler Optimization Was Not Implemented

The current scheduler is FIFO. A Priority + Aging + FIFO scheduler was not implemented because the measurements showed no queue-full saturation, no deferrals, high fairness, and no starvation.

> The scheduler was not changed because profiling did not identify it as the dominant bottleneck.

## 13. Phase 4-6 Optimization Investigation

At eight applications, the subprocess investigation measured:

| Phase | Fork/wait wall | Child API work |
|---|---:|---:|
| Phase 4 | 12.491 ms | 0.856 ms |
| Phase 5 | 14.908 ms | 4.152 ms |
| Phase 6 | 13.062 ms | 1.629 ms |

The difference between parent subprocess wall time and measured child API work shows that subprocess startup and uninstrumented residual work are significant. This residual must not be described as pure fork cost.

The Phase 4 direct-call experiment reduced Phase 4 from roughly 12.6 ms to roughly 0.9 ms locally, but whole-pipeline performance became worse. Therefore, the Phase 4 in-process candidate was rejected for production.

AWAVMA uses end-to-end system performance, not local function timing alone, to accept or reject optimizations.

## 14. Phase 5 and Phase 6 Investigation

At eight applications, the measured child envelopes were approximately:

| Component | Child envelope |
|---|---:|
| Phase 5 | 4691 us |
| Phase 6 | 371 us |

Phase 5 child work is dominated by persistence. Phase 6 child work is dominated by logging and lifecycle operations. Controlled validation-gate computation was sub-microsecond.

No Phase 5 or Phase 6 candidate was accepted because a meaningful optimization would require changing persistence or lifecycle semantics. Those semantics are retained to preserve safe runtime behavior.

## 15. Current Runtime Outcomes

Real runtime evaluations primarily produced **INSUFFICIENT** outcomes.

> This is an expected safe outcome because genuine page-level access-frequency and migration evidence are unavailable in the current environment.

`INSUFFICIENT` is not a failure. It indicates that AWAVMA intentionally avoids fabricating data or authorizing an unsupported migration action.

## 16. Phase 7 and Phase 8 Current Status

| Phase | Current status | Reason |
|---|---|---|
| Phase 7 | Not executed in real multi-node form | Valid remote placement, thread, and page-migration metadata are unavailable on the single-node host. |
| Phase 8 | Not executed in real runtime measurements | There is no valid observation sequence: before observation -> migration -> later after observation. |

These are deliberate safety boundaries, not omitted result claims.

## 17. Summary Comparison Table

| Metric | Current Result | Meaning |
|---|---:|---|
| Mean single-app execution overhead | +6.613% | Current runtime management cost |
| 1-app throughput change | -5.322% | Runtime overhead |
| 2-app throughput change | -2.920% | Lower relative overhead |
| 4-app throughput change | -0.792% | Small measured aggregate impact |
| 8-app throughput change | -1.372% | Small measured aggregate impact |
| 8-app Jain fairness | 1.000 | Even service distribution |
| Queue saturation | 0 | Worker queue did not saturate |
| Deferrals | 0 | No queue-full deferrals |
| Discovery-time reduction | 65.4% | Accepted optimization |
| Discovery detection latency | 249.4 ms | Within <=350 ms gate |
| 8-app serial-entry wait | 193.6 ms | Current main bottleneck |
| Actual multi-node NUMA comparison | Not yet performed | Requires 2+ NUMA nodes |

## 18. What We Can Currently Claim

The project has demonstrated:

- an integrated AWAVMA runtime;
- safe continuous multi-application monitoring;
- isolated application state;
- bounded worker execution;
- low measured multi-application throughput overhead;
- high fairness;
- evidence-based discovery optimization;
- safe handling of insufficient migration evidence; and
- extensive regression and integration verification.

## 19. What We Cannot Claim Yet

The current evidence does not support claims that:

- AWAVMA is faster than traditional NUMA;
- AWAVMA reduces remote memory access;
- AWAVMA improves multi-node NUMA locality;
- real cross-node memory migration improves performance; or
- final adaptive feedback improves NUMA performance.

Each claim requires a genuine machine with at least two NUMA nodes.

## 20. Next Phase

```text
Current single-node evaluation          COMPLETE
        |
        v
Obtain 2+ NUMA-node machine
        |
        v
Validate real NUMA topology
        |
        v
Validate real Phase 7 migration
        |
        v
Traditional/default Linux NUMA baseline
        |
        v
Controlled local/remote NUMA baseline
        |
        v
AWAVMA-enabled NUMA evaluation
        |
        v
Traditional NUMA vs AWAVMA comparison
        |
        v
Final Phase 10 results
```

## Evidence Basis

This report consolidates the verified final integrated runtime and controlled investigation results recorded in:

- `results/final_integrated_performance_report.md`;
- `results/final_integrated_performance_summary.csv`;
- `results/final_integrated_multiapp_scaling.csv`;
- `results/final_integrated_multiapp_fairness.csv`;
- `results/discovery_cadence_summary.csv`;
- `results/phase46_pipeline_report.md`;
- `results/phase56_subprocess_profile_summary.csv`; and
- `results/phase10_final_runtime_report.md`.

## 21. Final Conclusion

The current single-node evaluation demonstrates that AWAVMA has progressed from an isolated collection of monitoring, classification, decision, validation, migration, and feedback modules into an integrated multi-application runtime. The accepted runtime introduces an observed mean single-application execution-time overhead of approximately 6.613%. Under four and eight simultaneous applications, aggregate throughput impact was approximately -0.792% and -1.372%, respectively, while measured Jain fairness reached 1.000 at eight applications with no queue saturation, deferrals, or starvation. Discovery-cadence optimization reduced repeated discovery time by approximately 65.4% in the controlled experiment. The dominant remaining scalability cost is the serialized Phase 4-6 pipeline.

These results establish the runtime overhead and scalability characteristics of AWAVMA on the current single-NUMA-node platform. A genuine performance comparison between conventional NUMA management and AWAVMA remains the next major evaluation and requires hardware containing at least two NUMA nodes.
