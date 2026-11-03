# Thread-pool performance and correctness audit

**Correctness passed on the tested Linux host; full performance acceptance remains open.**
All 43 CTests passed in Release, ASan/UBSan with leak detection, and TSan.
The final benchmark matrix completed 750/750 processes successfully. Higher
8/16-worker throughput medians coexist with substantial variation and regressions.
Raw measurements, diagnostic logs and experiment patches are local artifacts
under ignored `build/performance/`; they are not part of the repository.

## Accepted changes and validation

- A semaphore permit does not make separately locked queue inspections atomic.
  Workers now retain the permit and retry an empty scan until work or terminal
  drain is observed, avoiding the original multi-producer abort.
- TSan shutdown diagnostics found one outstanding permit with the last worker
  blocked in libstdc++ 13's semaphore. Linux now uses an internal POSIX semaphore
  with EINTR retries. Other platforms retain the standard implementation.
  Admission and shutdown share an atomic state; failed publication returns its
  reserved slot, and the final dequeue after shutdown releases exit permits.
  Queue capacity reserves space for those permits; `join()` waits for execution.
- ASan exposed dangling async-scope closures and async composition handlers or
  borrowed inputs. Their lifetimes now extend through child completion.
  Gated tests cover value/void, catching, finally and borrowed arguments.
- Submit reuses the shared state's environment instead of capturing another
  copy. Small callables fit the existing 48-byte inline storage. Cancellation,
  exception propagation and completion arbitration retain their semantics.
- Test fixes synchronize PMR counters, priority-result reads and fiber enqueue
  order. Seven pool stress scenarios cover producers, shutdown during submit,
  queue limits, exceptions, lifecycle, allocation failure and cancellation races
  at 1/2/4/8/16 workers with 100,000 tasks each and unchanged 45-second deadlines.
  Per-ID checks detect lost or duplicate executions under `NDEBUG` too.

The five lifecycle tests additionally passed 20 consecutive TSan repetitions:
10 million tasks across 10,000 pool lifetimes. These results validate Linux/POSIX;
the standard-semaphore fallback remains unvalidated. Concurrent `join()` or
object destruction during method calls is not a new API guarantee.

## Measurement conditions and results

Compared revisions: CV `88a81d3`, correctness-only `e23269f`, optimized `e2d3e75`.
All used the unchanged benchmark, Clang 18.1.3, C++23, `-O3 -DNDEBUG`, libstdc++ 13
headers and 48-byte callable storage. Host: i7-8700K, 6 physical/12 logical cores,
Linux, powersave governor, shared desktop; 16 workers oversubscribe it.
The shared `modern_trace` dependency was at `34942ad496f9ece66058247257215420dd609d04`
with pre-existing local CMake changes, recorded in the local environment archive.
Exact historical build reproduction therefore also needs that local configuration.

The final comparison used five processes per version/workload/worker/PMR mode,
100,000 operations/run and four producers. Version order alternates; failed runs
are retained. Timing uses PMR counters disabled; allocation counts come from
separate instrumented runs. Throughput is **median [min–max], million ops/s**.
Ratios describe these runs, not guaranteed speedups.

| Workload | Workers | CV | Corrected | Optimized | Optimized / CV |
| --- | ---: | ---: | ---: | ---: | ---: |
| pool_single_flight | 1 | 0.322 [0.133–0.341] | 0.563 [0.465–0.907] | 0.524 [0.249–0.787] | 1.63× |
| pool_single_flight | 2 | 0.080 [0.037–0.220] | 0.584 [0.123–0.624] | 0.301 [0.116–0.571] | 3.74× |
| pool_single_flight | 4 | 0.048 [0.033–0.186] | 0.289 [0.124–0.509] | 0.463 [0.113–0.512] | 9.68× |
| pool_single_flight | 8 | 0.138 [0.054–0.167] | 0.535 [0.383–0.570] | 0.505 [0.271–0.543] | 3.66× |
| pool_single_flight | 16 | 0.128 [0.086–0.137] | 0.479 [0.323–0.572] | 0.461 [0.215–0.575] | 3.61× |
| pool_dispatch | 1 | 1.053 [0.433–2.129] | 2.122 [0.428–2.446] | 1.577 [0.455–2.429] | 1.50× |
| pool_dispatch | 2 | 1.253 [0.476–1.447] | 0.523 [0.265–0.925] | 0.400 [0.266–0.826] | 0.32× |
| pool_dispatch | 4 | 0.385 [0.093–0.457] | 0.632 [0.274–0.677] | 0.612 [0.278–0.720] | 1.59× |
| pool_dispatch | 8 | 0.297 [0.092–0.368] | 0.563 [0.278–0.622] | 0.612 [0.271–0.625] | 2.06× |
| pool_dispatch | 16 | 0.357 [0.153–0.369] | 0.585 [0.301–0.617] | 0.611 [0.361–0.715] | 1.71× |
| pool_multi_producer | 1 | 0.939 [0.320–1.666] | 0.380 [0.355–2.108] | 0.574 [0.375–2.014] | 0.61× |
| pool_multi_producer | 2 | 1.241 [0.268–1.724] | 0.973 [0.483–3.093] | 1.340 [0.501–2.970] | 1.08× |
| pool_multi_producer | 4 | 1.570 [0.313–1.770] | 1.803 [0.736–2.783] | 1.912 [0.521–2.786] | 1.22× |
| pool_multi_producer | 8 | 0.464 [0.311–0.553] | 1.544 [1.026–2.081] | 2.062 [1.179–2.200] | 4.45× |
| pool_multi_producer | 16 | 0.336 [0.104–0.430] | 1.185 [0.626–1.483] | 1.271 [0.421–1.366] | 3.78× |
| task_submit | 1 | 0.029 [0.023–0.116] | 0.108 [0.024–0.123] | 0.039 [0.030–0.124] | 1.31× |
| task_submit | 2 | 0.030 [0.026–0.113] | 0.036 [0.031–0.123] | 0.050 [0.032–0.122] | 1.64× |
| task_submit | 4 | 0.050 [0.019–0.109] | 0.031 [0.028–0.117] | 0.032 [0.023–0.119] | 0.64× |
| task_submit | 8 | 0.068 [0.025–0.106] | 0.097 [0.019–0.114] | 0.106 [0.032–0.115] | 1.55× |
| task_submit | 16 | 0.090 [0.032–0.096] | 0.101 [0.025–0.103] | 0.095 [0.041–0.102] | 1.06× |
| task_roundtrip | 1 | 0.024 [0.022–0.095] | 0.030 [0.025–0.101] | 0.060 [0.022–0.103] | 2.51× |
| task_roundtrip | 2 | 0.026 [0.020–0.082] | 0.018 [0.017–0.074] | 0.018 [0.014–0.072] | 0.70× |
| task_roundtrip | 4 | 0.020 [0.013–0.058] | 0.058 [0.016–0.067] | 0.051 [0.015–0.067] | 2.55× |
| task_roundtrip | 8 | 0.024 [0.013–0.055] | 0.018 [0.011–0.064] | 0.020 [0.013–0.062] | 0.81× |
| task_roundtrip | 16 | 0.052 [0.011–0.053] | 0.059 [0.013–0.060] | 0.058 [0.013–0.059] | 1.10× |


Latency below is in microseconds: medians of per-run percentiles, with ranges
across five runs. Full p50/p95/p99/p99.9 and variability tables can be generated
from local results using the scripts below.

| Workload | Workers | CV p99.9 [range] | Optimized p50 / p95 / p99 | Optimized p99.9 [range] |
| --- | ---: | ---: | ---: | ---: |
| pool_single_flight | 8 | 68.84 [40.64–250.85] | 1.22 / 2.61 / 5.16 | 12.24 [8.01–56.38] |
| pool_single_flight | 16 | 49.40 [48.41–107.00] | 1.32 / 3.20 / 5.71 | 11.07 [7.32–108.03] |
| pool_dispatch | 8 | 21.66 [15.92–159.22] | 1.52 / 3.41 / 5.17 | 11.21 [8.93–48.59] |
| pool_dispatch | 16 | 25.55 [22.17–55.24] | 1.58 / 3.25 / 4.92 | 20.97 [11.95–22.67] |
| pool_multi_producer | 8 | 632.09 [238.87–720.19] | 1.68 / 6.11 / 13.98 | 48.12 [34.07–816.71] |
| pool_multi_producer | 16 | 107.21 [31.37–224.76] | 2.29 / 7.37 / 37.51 | 235.20 [24.57–339.03] |
| task_submit | 8 | 187.25 [56.48–317.53] | 8.90 / 11.94 / 24.63 | 61.91 [45.92–431.07] |
| task_submit | 16 | 85.31 [58.44–215.99] | 8.45 / 13.99 / 41.42 | 66.84 [57.81–236.87] |
| task_roundtrip | 8 | 459.80 [156.40–917.99] | 16.90 / 154.93 / 207.04 | 352.81 [73.83–536.14] |
| task_roundtrip | 16 | 174.16 [122.26–820.33] | 16.30 / 21.91 / 29.86 | 109.18 [76.67–418.24] |


The capture reduction lowered PMR allocations across 1–16 workers by about
45–50% for submit and 15–16% for submit plus continuation. At one worker:

| Workload | Corrected allocations/op | Optimized allocations/op | Corrected bytes/op | Optimized bytes/op |
| --- | ---: | ---: | ---: | ---: |
| task_submit | 2.167 | 1.167 | 432 | 304 |
| task_roundtrip | 12.662 | 10.663 | 2231 | 1959 |

PMR counters exclude global heap allocations, including environment shared
ownership. Queue-block churn and await-registration races affect counts.
A deterministic inline-executor test checks one allocation for small submit and
four for a synchronous continuation chain, excluding queue variability.

The apparent one-worker submit regression against the corrected version was
checked separately with both processes restricted to CPUs 2,3, five runs and
100,000 operations. Submit medians were 126,257 → 125,873 ops/s (−0.3%; run CV
5.82% → 5.05%). Roundtrip was 89,305 → 107,583 (+20.5%; CV 13.90% → 16.02%),
with overlapping ranges. This focused check does not replace the full matrix.

## Bottlenecks, rejected experiments and remaining limits

A 16-worker multi-producer user-cycle profile attributed 23.28% of samples to
mutex locking and 8.73% to unlocking; stealing was the largest identifiable
caller. These are sampling proportions, not elapsed-time costs. Empty-queue
search remains linear and queue mutations still require locks. Admission atomics
also enforce capacity and shutdown ordering; weakening them was not justified.
Cache-line padding was not accepted without an isolated measured gain.

An empty-queue hint passed correctness tests but failed to show a reliable
regression-free improvement. A bounded semaphore spin experiment showed median
regressions of 50.6% for 16-worker multi-producer and 51.5% for roundtrip, with
high variation. Neither experiment is in production. Continuations still own a
composition frame/state and an inner submit state; eliminating those requires
larger lifetime and scheduling changes and was deferred.

**The stable-scaling and no-regression gates are not fully passed.**
Multi-producer throughput drops from 2.062 million ops/s at 8 workers to 1.271
at 16. Two-worker dispatch, one-worker multi-producer and several serial-task
configurations have lower medians than CV. Multi-producer p99.9 at 16 workers
rises from 107.21 to 235.20 µs; saturated latency includes queue backlog.
High run variation and six physical cores prevent a general claim of stable
16-core scaling. A quiet target host with 16 physical cores is still needed.
Public APIs, benchmark definitions, dependencies and timeouts remain unchanged.

## Reproduction

Build Release with `cmake --preset kpi && cmake --build --preset kpi -j8`, then
`ctest --test-dir build/kpi --output-on-failure`. The sanitizer commands used the
already installed LLVM 18 resource directory because the `/usr/local` compiler
lacked its own sanitizer runtimes:

```sh
cmake --preset kpi-sanitize \
  -DCMAKE_CXX_FLAGS='-resource-dir=/usr/lib/llvm-18/lib/clang/18 -fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build --preset kpi-sanitize -j8
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/kpi-sanitize --output-on-failure
cmake -S . -B build/kpi-tsan -G Ninja \
  -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMODERN_RUNTIME_BUILD_KPI=ON \
  -DCMAKE_CXX_FLAGS='-resource-dir=/usr/lib/llvm-18/lib/clang/18 -fsanitize=thread -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
cmake --build build/kpi-tsan -j8
ctest --test-dir build/kpi-tsan --output-on-failure
ctest --test-dir build/kpi-tsan -R '^pool_lifecycle_' --repeat until-fail:20 --output-on-failure
```


LeakSanitizer needs execution outside the tool's ptrace sandbox. Module builds
supported both sanitizer configurations on this host.

Use separate directories for each compared revision and the same compiler and
dependency configuration. Run measurements without concurrent builds or tests.
The matrix defaults to all five workloads and worker counts, five repetitions,
and both PMR modes; specify 100,000 operations explicitly. Use a fresh output
directory for each run. CSVs, logs and generated tables remain under `build/`.

```sh
mkdir -p build/reproduce/cv/source build/reproduce/corrected/source
git archive 88a81d3 | tar -x -C build/reproduce/cv/source
git archive e23269f | tar -x -C build/reproduce/corrected/source
for variant in cv corrected; do
  cmake -S "build/reproduce/$variant/source" -B "build/reproduce/$variant/build" -G Ninja \
    -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++ -DCMAKE_BUILD_TYPE=Release \
    -DMODERN_RUNTIME_BUILD_KPI=ON -DMODERN_RUNTIME_INLINE_CALLABLE_BYTES=48 \
    -DMODERN_TRACE_ROOT="$PWD/../modern_trace"
  cmake --build "build/reproduce/$variant/build" -j8
done
```

Build the final branch with `cmake --preset kpi && cmake --build --preset kpi`.
Then run, from a normal shell outside ptrace, with no concurrent builds/tests:

```sh
python3 scripts/run_pool_matrix.py --operations 100000 --output build/reproduce/results \
  --binary cv=build/reproduce/cv/build/modern_runtime_benchmark \
  --binary corrected=build/reproduce/corrected/build/modern_runtime_benchmark \
  --binary optimized=build/kpi/modern_runtime_benchmark
python3 scripts/summarize_pool_matrix.py build/reproduce/results build/reproduce/final
python3 scripts/render_pool_report.py build/reproduce/final-summary.csv build/reproduce/tables.md
```


To repeat the focused check, use `taskset -c 2,3` with the matrix runner,
`--workers 1 --workloads task_submit task_roundtrip --operations 100000`,
the corrected/optimized binaries and a separate output directory. Select two
distinct physical cores appropriate to the target host.
