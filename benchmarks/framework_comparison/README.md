# Scheduler comparison benchmarks

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

The default run builds `modern_runtime`, **Taskflow 4.1.0** and
**oneTBB 2022.3.0**. Both competitors are fetched from pinned Git revisions
by default. Set `-DCOMPARE_USE_SYSTEM_TBB=ON` during manual CMake configuration
only if you intentionally want to benchmark an installed oneTBB release. The parent runtime is built using its
existing CMake files and dependencies. Network access is required for the
initial external checkout unless sources are already cached.

Optional fourth adapter:

```sh
COMPARE_STDEXEC=1 bash benchmarks/framework_comparison/run.sh
```

**stdexec is a distinct comparison category.** Its public detached-sender path
constructs an operation state and cannot be equated with a low-level pool
enqueue. The source revision is pinned, but the optional adapter still needs
validation on the target Clang/CMake toolchain.

## Configuration

- `COMPARE_RUNS=10` — independent processes per configuration
- `COMPARE_OPERATIONS=100000` — tasks per process
- `COMPARE_PRODUCERS=4` — producers for multi-producer
- `COMPARE_WORKERS="1 2 4 8 16"` — worker count matrix
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

Each process initializes a fresh scheduler and completes 256 warm-up tasks
before starting the clock. Throughput includes submission and completion.
Single-flight captures hand-off/round-trip effects. Saturated enqueue-to-start
latency includes **queue backlog** and should not be called wakeup latency.
All frameworks use the same timestamp, storage, task body and completion
protocol; their public enqueue interfaces do not necessarily offer identical
scheduling guarantees.

Any rejected submission, process crash, timeout, missing task verification,
or malformed result is counted as a **failure** rather than silently excluded.
A report is generated even when a run fails. Raw measurements are stored under
`build/framework-comparison/` (ignored by the repository).

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

The benchmark adapters have been committed but have not yet been built/tested
on the target host. The C++23 module toolchain, `modern_trace` dependency and
external source checkout must be available. stdexec is opt-in and not a direct
fire-and-forget scheduling equivalence. Task graphs, continuation chains,
bulk algorithms and memory-allocation counts are **not** part of this first
like-for-like task-dispatch suite.
