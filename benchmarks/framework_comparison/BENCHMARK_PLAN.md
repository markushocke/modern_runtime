# CURRENT BENCHMARK PLAN — modern_runtime vs Taskflow and oneTBB

Status: benchmarking infrastructure and three initial representative use-case workloads committed.
**Full integration build, sanitizers, and fresh on-host results have NOT been
verified for these changes. Do not publish a ranking until those checks pass.**
Last reviewed: 2026-11-06.

## Purpose / decision

The point is **NOT** to rank queue primitives. Demonstrate which practical
C++23 execution workloads the library handles correctly, where throughput,
tail latency and memory costs are competitive, and where the unified task/
coroutine/cancellation/timer/I/O surface creates an integration advantage.

Always include Taskflow 4.1.0 and oneTBB 2022.3.0 in the main comparison:
the default run.sh builds four modern_runtime scheduling variants AND both
competitors. The fourth uses Chase-Lev queues with idle worker parking. COMPARE_ONLY_RUNTIME=1 is a **diagnostic** A/B/C mode only.
stdexec is optional and is not strictly equivalent to raw enqueue.

## Benchmark layers

| Priority | Workload | Real-life analog | Comparability / gate | Status |
| --- | --- | --- | --- | --- |
| P0 | pool_dispatch | Enqueue a batch of independent tiny jobs | Raw submission, same task body, not realistic full runtime | Existing |
| P0 | pool_multi_producer | Several request threads submit independent jobs | Shared dispatch under contention | Existing |
| P0 | pool_single_flight | Request/response handoff, one outstanding job | Measures handoff; includes benchmarking synchronization | Existing |
| P1 | pool_worker_fanout | Asset build or geometry tile spawns N smaller tasks on a worker | One root submits leaves; completion verifies all leaves | Added; needs on-host validation |
| P1 | pool_mixed_compute | 15 short event/parse/transform tasks per 1 costly one | Identical deterministic CPU kernel and completion verification for every adapter | Added; needs on-host validation |
| P2 | dependency_dag | Compilation/build DAG, dependent sub-jobs | Compare equal dependency semantics, never only equivalent node count | Not implemented |
| P2 | bulk_parallel_for | Partition geometry/arrays/images into chunks | Equal element work and chunking; compare native Taskflow/oneTBB bulk APIs with equivalent modern tasks | Not implemented |
| P1 | pool_staged_pipeline | Decode -> transform -> pack/result across three dependent tasks | Equal dynamic dependency structure; end-to-end latency; no backpressure guarantee | Added; needs on-host validation |
| P2 | async_composition | task<T>, then/await, exceptions/cancellation | Capability benchmark; clearly distinguish API semantics when no analogue exists | Not implemented |
| P2 | timer_and_io_handoff | Wakeup, cancellation, external I/O completion | Separate integration latency from raw pool throughput | Not implemented |

The P1 CPU mix is deterministic and dependency-free. It is a *model workload*,
not a claim that a particular JSON parser, renderer, or compiler was measured.
The fanout benchmark actually exercises worker-local enqueue/steal: unlike the
original three scenarios it can expose Chase-Lev benefits.
The pipeline intentionally uses three sequential, dynamically submitted tasks
per item on each framework. It is a practical dependency pattern, not a
benchmark of framework-native DAG builders, graph optimization, or bounded
backpressure. Report its throughput in completed items/s and its p99 as
end-to-end completion latency; other workloads use enqueue-to-start latency.

## Primary scorecard and comparison rules

- Report throughput, per-task enqueue-to-start p50/p95/p99/p99.9, coefficient
  of variation, paired same-round ratios and wins, failed submissions, and
  correct output checks. A p99 under saturation includes backlog.
- All competitors must process identical inputs and task bodies. Do not compare
  the fastest backend with a competitor's more expensive unrelated API.
- Include worker-count scaling (1/2/4/8, and 16 labeled oversubscription
  on the six-physical-core i7-8700K). Pin to **actual physical cores** using
  lscpu's CPU/CORE map. Use the same CPU affinity and build settings.
- Default main comparison includes Taskflow and oneTBB. Do not silently
  present A/B/C runtime-only data as a framework ranking.
- Record source SHAs, framework revisions, binary hashes, governor/frequency
  context and exact CLI. Keep invalid runs visible.
- Gate any speedup claim on successful Release/ASan/UBSan/TSan, capacity,
  shutdown, priority, allocation-failure and exactly-once tests. Meaningful
  CV (>25% is automatically warned) needs controlled repeat before promotion.

## Profiling protocol (diagnosis, not comparative score)

1. Identify an adverse case with the uninstrumented *full* comparison. Focus
   on P1 fanout, staged pipeline and mixed CPU at 4/8 workers.
2. Run profile.sh separately: it builds an optimized library with frame
   pointers and DWARF debug symbols, runs perf stat counters and samples
   CPU stacks with perf record. Default: all six variants, P1 scenarios.
3. Read hotspots.md (top self-overhead and rough categories), the full
   *.perf.report.txt, stat.csv and *.perf.data call graphs.
4. Verify which call chains create the samples; check PMR malloc/free and
   cache behavior, pthread_mutex/futex/sem_wait, atomics, queue scanning,
   wakeup and user compute. Inspect perf annotate if a hot implementation
   path requires source/assembly-level confirmation.
5. Compare the opt-in `modern_chase_lev_park` against the unchanged
   Chase-Lev/semaphore baseline, Taskflow and oneTBB. Hypothesis: far fewer
   voluntary context switches than ~1 per task; verify on 4/6/16 workers.
6. Optimize the next measured hotspot on dev, retain older variants as
   comparators, then rerun acceptance and the full matrix.

CAUTION: a sampled profile cannot prove contention or waiting causality
alone; off-CPU tracing may be required. Recorded samples are not directly
comparable to wall-clock results from a different run. Linux perf_event_paranoid
may prevent collection: report this limitation instead of guessing.

## Commands (from repository root)

Fast full-framework smoke:

    git switch bench/framework-comparison
    git pull --ff-only
    COMPARE_ONLY_RUNTIME=0 COMPARE_RUNS=1 COMPARE_OPERATIONS=1000 COMPARE_WORKERS="1 4" \
      COMPARE_WORKLOADS="pool_worker_fanout pool_mixed_compute" \
      bash benchmarks/framework_comparison/run.sh

Comparable full result (20 repeats per configuration):

    COMPARE_ONLY_RUNTIME=0 COMPARE_RUNS=20 COMPARE_OPERATIONS=100000 COMPARE_WORKERS="1 2 4 8" \
      bash benchmarks/framework_comparison/run.sh

Profiling (independent, instrumented and symbolized build):

    PROFILE_WORKERS=4 PROFILE_OPERATIONS=1000000 \
      bash benchmarks/framework_comparison/profile.sh

Optional: choose PROFILE_TASKSET and COMPARE_TASKSET based on
lscpu -e=CPU,CORE,SOCKET, not an assumed numbering scheme.

Validation for the lock-free backend:

    cmake --preset kpi && cmake --build --preset kpi -j8
    ctest --test-dir build/kpi -R 'lock_free_queue_primitives|pool_chase_lev_' \
      --output-on-failure

## Next concrete steps

1. On-host C++23 module build, stress and sanitizer acceptance of idle parking,
   including `worker_parking_primitives`, `pool_park_chase_lev_*`,
   and repeated idle-wakeup cycles. Do not claim success until measured.
2. Collect full competitor results and matching perf profiles. Upload report.md,
   runs.csv, summary.csv, metadata.json AND profiler hotspots.md/stat/perf reports.
3. Attribute the measured bottleneck before choosing PMR/node-pool, wakeup,
   queue or task API optimization.
4. Implement bulk_parallel_for and a native dependency_dag comparison
   (Taskflow graph, oneTBB flow graph and equivalent modern_runtime
   dependencies), and measure true bounded backpressure; separately assess
   modern-only composition, cancellation, timer and I/O capabilities.

Runtime code changes belong to dev. The benchmark harness and this plan
belong to bench/framework-comparison; merge dev forward into benchmark.
