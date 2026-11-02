# Runtime KPI audit

This optional harness measures **runtime behavior**, not application-level performance.
Build with the project's Clang-based `kpi` preset (requires C++23 named-module support and `clang-scan-deps`). The KPI targets are registered automatically when the option is enabled.

## Commands

```bash
cmake --preset kpi
cmake --build --preset kpi --parallel
ctest --test-dir build/kpi --output-on-failure
for t in 1 2 4 8 16; do
  ./build/kpi/modern_runtime_benchmark --threads "$t" --producers 4 --operations 10000 --timer-samples 128 > "kpi-${t}.csv"
done
./build/kpi/modern_runtime_stress --iterations 1000000
```

## Definitions

- `pool_dispatch`: **single-producer saturated** submission: wall-clock completion throughput and enqueue-to-start delay, including queue backlog. A high delay under one worker can be expected even with high throughput.
- `pool_single_flight`: isolated enqueue-to-execution-start delay. One job must complete before the next is submitted. Measures scheduling and wakeup without queue backlog; the benchmark itself adds a semaphore handshake to total runtime.
- `pool_multi_producer`: concurrent submissions from `--producers` producer threads to one shared pool. Keep producer count fixed when comparing worker counts (e.g. four producers at 1/2/4/8/16 workers). Producer start is gated; OS thread creation is excluded from measured elapsed time.
- `task_submit`: one-at-a-time `submit` + blocking `get`, with no continuation; compare with `task_roundtrip` to estimate composition overhead.
- `task_roundtrip`: sequential submit + continuation + blocking get, including scheduling and completion; not peak throughput.
- `timer_lateness`: measured execution start minus requested due time for sequential 2 ms timers. Negative values indicate early execution.
- `pmr_allocs_per_op` and `pmr_bytes_per_op`: **only** allocations routed through the supplied counting PMR resource. Does not represent all heap allocations. Thread pool and timer initialization are excluded from the snapshots. These are cumulative allocated bytes, not peak resident memory.
- Percentiles use nearest-rank method, in nanoseconds. The `ops_per_second` value is the per-workload completion rate, not a cross-workload comparison.

## Reproducibility

Use Release builds, a quiet dedicated machine, fixed CPU governor and affinity, and at least five independent runs **per configuration**. Report median of runs and variation. Compare the same workload and producer count between configurations; never infer scheduler scalability from serial task-roundtrip measurements. Record compiler, standard library, kernel, CPU, memory, thread count, and commit SHA. Avoid mixing sanitizer runs with performance data.

## Acceptance roadmap

1. Correctness: TSAN/ASAN/UBSAN on supported toolchains, cancellation-vs-completion races, task-scope teardown, and bounded-queue saturation.
2. Performance: comparable workloads with stdexec, Taskflow and a std::thread baseline; separate direct scheduling from task-chain overhead.
3. Integration: install/package consumer builds on multiple supported compiler configurations.
4. Adoption: independent projects and feedback, not GitHub stars alone.

The current stress executable checks multi-producer exactly-once completion, accepted-task shutdown draining, and pre-cancelled tasks. It does **not** yet prove all race/lifetime invariants or provide a 24-hour soak test. No benchmark results are claimed until run on a validated build.

## Compiler selection

CMake caches the selected compiler. A previous `build-kpi` directory configured with `/usr/bin/c++` (GCC 13) cannot be switched to Clang merely by rerunning configure. The `kpi` preset uses its own `build/kpi` directory and selects `clang++`; check `command -v clang++ clang-scan-deps` and `clang++ --version`. If the machine has several versions, use `-DCMAKE_CXX_COMPILER=/usr/local/bin/clang++` or an explicit versioned Clang binary in a **new** build directory. GCC 13 is rejected with a clear configuration error because its C++ modules dependency scanning is not usable with this project's CMake setup.

## Diagnosing stress timeouts

Stress tests are separated into CTests `modern_runtime_stress_producers`, `modern_runtime_stress_shutdown`, and `modern_runtime_stress_cancellation`, each with a 45-second timeout. Each scenario prints flushed progress checkpoints to stderr before blocking operations, so `ctest --output-on-failure -R '^modern_runtime_stress_'` identifies the stuck phase. A timeout is a failure, not proof of a thread-pool defect until the stuck phase and stack traces are inspected. To isolate a case interactively, run `./build/kpi/modern_runtime_stress --case producers --iterations 1000` (or `shutdown`, `cancellation`).

## Interpreting results

The tool prints CSV. `pool_dispatch` can show high throughput and high per-task delay simultaneously because it measures queueing. Do not interpret `pool_multi_producer` as linear scaling until there is an independent reference/baseline and repeat-run variation. `task_submit` isolates fixed Task overhead from the additional `.then()` composition. Sequential 2-ms timers are a jitter benchmark, **not** a timer capacity or maximum-operations-per-second benchmark. No acceptance threshold should be introduced before the baseline is statistically stable.

## Optimization checkpoint: small callable storage

The thread pool scheduler uses `modern::detail::move_only_function`. The original
implementation allocated **every** scheduled callable through a PMR resource.
The experimental optimization keeps nothrow-movable callables up to a configurable capacity
(including their type-erasure model) inline. Large, over-aligned, or
potentially-throwing-move callables retain the original PMR-backed fallback.
The public scheduling API is unchanged. An explicit PMR resource is honored\nfor callables that need allocation; it is not required to receive an\nallocation for inline-stored callables. `exec_smoke` therefore tests both\ninline no-allocation and large-capture PMR propagation.

**Trade-off:** enlarging `move_only_function` can reduce PMR call counts while increasing the memory needed for `std::pmr::deque` queue blocks and hurting cache locality. At 48-byte SBO, the initial v3 measurements showed about 84% fewer PMR allocation calls for direct pool-dispatch, but around 40% more PMR allocated bytes and no reliable throughput improvement. The 32-byte candidate is experimental. This optimization is **not** accepted on allocation counts alone.

Regression test: `ctest --test-dir build/kpi -R move_only_function_test
--output-on-failure`. The test covers inline no-allocation behavior, PMR
fallback and deallocation, alignment, move-only capture, moves and exceptions.

To evaluate on the same machine, run the full CTest suite and then repeat
`kpi-v2` workload parameters with five runs per worker count. Compare
`pool_dispatch` and `pool_multi_producer` throughput, queue-latency
percentiles, `pmr_allocs_per_op` and `task_roundtrip`. A regression in
throughput or long-tail latency is a reason to reduce or disable inline size;
a decrease in PMR allocations alone is not sufficient to merge.

## Controlled SBO experiment (v4)

Run `bash scripts/run_kpi_ab.sh`. This configures and tests two independent
Release builds (`kpi-sbo32` and `kpi-sbo48`), then writes a single
`kpi-ab.zip` containing five repeated runs for three worker counts and five
workloads, separately measuring counting-PMR and native-PMR modes.

The new benchmark options are:
- `--workload pool_dispatch|pool_single_flight|pool_multi_producer|task_submit|task_roundtrip|timer_lateness|all`
- `--pmr counting|default`. The `default` mode uses
  `std::pmr::new_delete_resource()` without a counting wrapper.
  The `pmr_allocs_per_op` and `pmr_bytes_per_op` CSV columns report
  zero in this mode **because they are not instrumented**, not because
  no allocations occurred. The `pmr_mode` CSV column makes that explicit.

Each workload is run in a fresh process. The order of SBO capacities alternates
between runs to reduce order bias. Never compare `--pmr default` counts to
`--pmr counting` counts. Use the uninstrumented timings as the main performance
signal and the counting-PMR run only for allocation cost analysis. Since v2/v3
ran all workloads sequentially in one process, they are **not** a matched
baseline for the isolated v4 numbers.

The build setting `MODERN_RUNTIME_INLINE_CALLABLE_BYTES=32|48` is PUBLIC:
runtime objects and consumers must agree on the same value because it affects
the internal callable's C++ layout. This is a benchmark-only choice pending
final acceptance; avoid mixing objects from different builds.
