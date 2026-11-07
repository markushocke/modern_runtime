# Scheduler comparison benchmarks

**Canonical scope and next steps:** [BENCHMARK_PLAN.md](BENCHMARK_PLAN.md).
The primary question is practical C++23 execution workloads versus Taskflow
and oneTBB. Queue microbenchmarks are only an internal diagnostic layer.
For the complete comparison explicitly use COMPARE_ONLY_RUNTIME=0.

This folder is **benchmark infrastructure only**. The production runtime is not
modified. New runtime development goes into `dev`; changes are then merged from
`dev` into `bench/framework-comparison`, **never in the opposite direction**.

## Run

From the repository root (Linux, Git, Python >= 3.9, CMake >= 3.28, Ninja,
Clang with C++23 named-module support and a C++ toolchain):

```sh
git switch bench/framework-comparison
bash benchmarks/framework_comparison/run.sh
```

On Linux, the default run builds six `modern_runtime` scheduling variants
(`modern_runtime` with `direct_mutex`, `modern_injection` with
`injection_mutex_local`, and `modern_chase_lev` with the opt-in bounded
lock-free `injection_chase_lev`, plus `modern_chase_lev_park` using the
same Chase-Lev queues with idle-only worker parking, plus
`modern_chase_lev_futex` with direct Linux futex sleeping, plus
`modern_chase_lev_park_pooled` with bounded PMR task-node recycling),
**Taskflow 4.1.0** and
**oneTBB 2022.3.0**. Both competitors are fetched from pinned Git revisions
by default. Set `-DCOMPARE_USE_SYSTEM_TBB=ON` during manual CMake configuration
only if you intentionally want to benchmark an installed oneTBB release. The parent runtime is built using its
existing CMake files and dependencies. Network access is required for the
initial external checkout unless sources are already cached.

Optional ninth adapter (after six Linux runtime variants, Taskflow and oneTBB):

```sh
COMPARE_STDEXEC=1 bash benchmarks/framework_comparison/run.sh
```

**stdexec is a distinct comparison category.** Its public detached-sender path
constructs an operation state and cannot be equated with a low-level pool
enqueue. The source revision is pinned, but the optional adapter still needs
validation on the target Clang/CMake toolchain.

## Fast runtime-only comparison

For a first smoke test or to focus exclusively on the six Linux runtime variants:

```sh
COMPARE_ONLY_RUNTIME=1 COMPARE_RUNS=1 COMPARE_OPERATIONS=1000 \
  COMPARE_WORKERS="1 4" bash benchmarks/framework_comparison/run.sh
```

For a full paired runtime comparison, set `COMPARE_ONLY_RUNTIME=1` and
`COMPARE_RUNS=20`. The default run tests eight candidates on Linux (six runtime variants
plus two competitor frameworks).
The runtime-only mode also works without downloading/building competitor
dependencies. Do not combine it with `COMPARE_STDEXEC=1`.

## Actual use cases and profiling

The suite now includes three P1 use-case scenarios in addition to the three
existing raw scheduling diagnostics:

- `pool_worker_fanout`: a worker receives a root task and generates child
  tasks for an asset/geometry/build-style fanout. Unlike external dispatch,
  it exercises local deque push/pop and stealing.
- `pool_mixed_compute`: identical tasks perform deterministic CPU work of
  mixed sizes (15 short operations per one longer operation). Includes
  scheduling plus realistic load imbalance, but does not claim to implement
  a specific real-world parser or renderer.
- `pool_staged_pipeline`: three dependent processing tasks per data item
  (decode -> transform -> pack). Throughput counts **completed items/s**,
  not individual tasks/s. p99 measures end-to-end submission-to-completion.
  The stages are representative in-memory transforms rather than a real codec.

A full-framework test is the **default**; `COMPARE_ONLY_RUNTIME=1` intentionally
excludes Taskflow/oneTBB and must not be described as a framework comparison.

For a profile of all eight Linux candidates with symbolized optimized code:

```sh
PROFILE_WORKERS=4 PROFILE_OPERATIONS=1000000 \
  bash benchmarks/framework_comparison/profile.sh
```

Linux `perf` and sufficient performance-counter permissions are required.
By default `PROFILE_SAMPLE_EVENT=cycles:u` records user-space samples only,
so restricted Linux kernel symbol maps should not prevent application-level
hotspot analysis. For intentional kernel sampling, use
`PROFILE_SAMPLE_EVENT=cycles` and configure kernel symbol permissions
separately. This does not bypass `perf_event_paranoid`: access to CPU
performance events may still require permission. If supported by the kernel,
`PROFILE_SAMPLE_EVENT=cpu-clock:u` is an alternative software sampling event.
The separate profiler build saves `*.perf.data`, `*.perf.report.txt`,
`*.stat.csv`, `metadata.txt` and the generated `hotspots.md`.
The profiler passes `--profile-only` to each adapter: the **normal benchmark**
still verifies every result and computes percentiles, but profile-only runs
omit the expensive post-measurement sort and repeated result recomputation.
Consequently, profile-only runs are for diagnosing CPU behavior, **not** for
independent correctness acceptance or latency reporting.
This **does not replace** the uninstrumented benchmark results. It identifies
sampled CPU hotspots; use the full call graph and counters to verify a
hypothesis before optimizing.

## Configuration

- `COMPARE_RUNS=10` — independent processes per configuration
- `COMPARE_OPERATIONS=100000` — tasks per process
- `COMPARE_PRODUCERS=4` — producers for multi-producer
- `COMPARE_WORKERS="1 2 4 6 8 12 16"` — worker count matrix
- `COMPARE_WORKLOADS="pool_worker_fanout pool_mixed_compute pool_staged_pipeline pool_dispatch pool_multi_producer pool_single_flight"` — workload matrix
- `COMPARE_TASKSET="2-7"` — optional Linux CPU affinity mask
- `COMPARE_BUILD_JOBS=8` — parallel compiler jobs
- `COMPARE_CXX=/path/to/clang++` — compiler override
- `COMPARE_OUTPUT=build/framework-comparison/my-run` — output directory
- `COMPARE_BUILD_DIR=build/framework-comparison/cmake` — build location

Run a reduced smoke test first:

```sh
COMPARE_RUNS=1 COMPARE_OPERATIONS=1000 COMPARE_WORKERS="1 4" \
  bash benchmarks/framework_comparison/run.sh
```

## Workloads and fairness

Every adapter executes the **same C++ function body**: record enqueue-to-start
delay, write a deterministic result into a disjoint vector slot, count down
completion. The final verification checks that each submitted index produced the expected
value; dedicated stress tests, not this throughput benchmark, cover duplicate
execution and other scheduler races.

| Workload | Meaning |
| --- | --- |
| `pool_dispatch` | One producer submits all tasks, then waits for completion |
| `pool_multi_producer` | Four producers begin together and submit to one pool |
| `pool_single_flight` | One task in flight; submit, wait, repeat |
| `pool_worker_fanout` | One worker creates a large batch of children; verify all completed |
| `pool_mixed_compute` | One producer submits mixed-size deterministic CPU tasks |
| `pool_staged_pipeline` | Decode, transform, pack as three dependent tasks per item |

Each process initializes a fresh scheduler and completes 256 warm-up tasks
before starting the clock. Throughput includes submission and completion.
Single-flight captures hand-off/round-trip effects. Saturated enqueue-to-start
latency includes **queue backlog** and should not be called wakeup latency.
For `pool_staged_pipeline`, latency instead measures the full three-stage
submission-to-completion transaction, not just queueing.
All frameworks use the same timestamp, storage, task body and completion
protocol; their public enqueue interfaces do not necessarily offer identical
scheduling guarantees.

Any rejected submission, process crash, timeout, missing task verification,
or malformed result is counted as a **failure** rather than silently excluded.
A report is generated even when a run fails. Raw measurements are stored under
`build/framework-comparison/` (ignored by the repository).

The `modern_injection`, `modern_chase_lev` and `modern_chase_lev_park`
adapters are opt-in at the
library API level; the benchmark constructs both explicitly.
`modern_runtime` remains the direct-mutex reference, so same-round paired
ratios show whether injection or Chase-Lev wins or loses.

`modern_chase_lev_park_pooled` differs from `modern_chase_lev_park`
only in task-node allocation. A bounded PMR recycler (16 cached nodes per
worker, up to 4096 centrally cached) reuses empty nodes; each worker owns
its local cache, external producers use a mutex-protected central freelist.
The A/B report includes a dedicated throughput and p99 comparison.
The pooled variant is experimental until full on-host C++23 module builds,
stress/sanitizer matrices, and representative benchmark results pass.

`modern_chase_lev_futex` is a Linux-only alternative to the atomic-wait
parking mode. It uses per-worker FUTEX_WAIT_PRIVATE / FUTEX_WAKE_PRIVATE
with a 32-bit epoch, avoiding libstdc++ atomic-wait spinning. The baseline
semaphore and atomic-wait parking modes remain unchanged for A/B testing.
Correctness, sanitizer and speed improvements are NOT yet validated.

`modern_chase_lev_park` uses the **same queues** as `modern_chase_lev`,
but changes only the wakeup protocol to idle-only, per-worker atomic parking.
This isolates the effect of per-task semaphore permits. The original
`modern_chase_lev` remains the A/B baseline. The new variant must pass
sanitizer and stress acceptance before default promotion.

`modern_chase_lev` uses fixed-capacity, PMR-backed Chase-Lev worker deques
and a bounded lock-free MPMC injection ring. Queue operations are mutex-free,
but end-to-end submission still allocates a PMR task node and invokes the
pool's semaphore and admission protocol. This is not yet a proven faster
or completely nonblocking scheduler.

The output contains:
- `metadata.json` — source SHAs, binary SHA-256 hashes, affinity, CPU model,
  Linux frequency governors, build configuration, run parameters
- `runs.csv` — success/failure of **every** process and best-effort CPU-frequency
  snapshots just before/after each process
- per-run `*.csv` and `*.stderr` — raw data and diagnostics
- `summary.csv` — median, min, max and coefficient of variation
- `report.md` — comparison against modern_runtime per workload and worker
  count, paired within each measurement round; number of wins and high-variance
  (CV >= 25%) warnings. Paired results still require a quiet host and enough
  repetitions to draw any conclusions.

Do not interpret five-minute desktop measurements as a reliable framework
ranking. Use a quiet machine, stable frequency governor and explicit CPU
affinity. A 16-worker result on a machine with six physical cores measures
oversubscription, **not 16-core scalability**. Prefer at least ten processes
and inspect variance and p99.

## Keep the benchmark branch current

After committing runtime changes to `dev`:

```sh
git switch bench/framework-comparison
bash benchmarks/framework_comparison/sync_dev.sh
bash benchmarks/framework_comparison/run.sh
```

Resolve merge conflicts manually. Do **not** force-push or automatically merge
the benchmark-only changes into `dev`. The benchmark history follows the
development history; historical comparisons should record the exact `dev`
SHA from `metadata.json` or `git merge-base dev bench/framework-comparison`.

## Current limitations

The existing adapters have previously been built and benchmarked on the
target host; the newly added pooled-node variant has **not** been validated
in a full integrated on-host C++23 build yet. The C++23 module toolchain, `modern_trace` dependency and
external source checkout must be available. stdexec is opt-in and not a direct
fire-and-forget scheduling equivalence. Task graphs, continuation chains,
bulk algorithms and memory-allocation counts are **not** part of this first
like-for-like task-dispatch suite.
