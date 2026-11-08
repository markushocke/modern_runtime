# Thread-pool queue backends

The scheduler-facing public API stays identical. For reproducible A/B
measurement, new code can use named `thread_pool_options`, or an optional
backend at the **end** of the existing constructor's argument list. Existing
callers remain source compatible and continue to use the original queue
implementation by default.

## API

```cpp
import modern.runtime;

// Original behavior; no caller changes required.
modern::thread_pool baseline{8};

// Preferred: named options, no positional backend magic.
modern::thread_pool injected{modern::thread_pool_options{
  .workers = 8,
  .resource = modern::memory::get_default_resource(),
  .max_pending_tasks = 4096,
  .backend = modern::queue_backend::injection_mutex_local
}};

// Opt-in bounded Chase-Lev + lock-free MPMC injection.
modern::thread_pool lock_free_pool{modern::thread_pool_options{
  .workers = 8,
  .max_pending_tasks = 4096,
  .backend = modern::queue_backend::injection_chase_lev
}};

// Opt-in idle parking: preserves the same Chase-Lev queues but replaces
// one POSIX semaphore permit per task with targeted worker wakeups.
modern::thread_pool parked{modern::thread_pool_options{
  .workers = 8,
  .max_pending_tasks = 4096,
  .backend = modern::queue_backend::injection_chase_lev,
  .wakeup = modern::worker_wakeup::idle_parking
}};

// Linux-only experiment: identical Chase-Lev queue and idle detection,
// but direct FUTEX_WAIT_PRIVATE instead of std::atomic::wait.
modern::thread_pool futex_pool{modern::thread_pool_options{
  .workers = 8,
  .max_pending_tasks = 4096,
  .backend = modern::queue_backend::injection_chase_lev,
  .wakeup = modern::worker_wakeup::idle_parking_futex
}};

// Opt-in bounded node recycling with the same queues and atomic parking.
modern::thread_pool pooled{modern::thread_pool_options{
  .workers = 8,
  .max_pending_tasks = 4096,
  .backend = modern::queue_backend::injection_chase_lev_pooled,
  .wakeup = modern::worker_wakeup::idle_parking
}};

// Make the original backend explicit for A/B comparisons.
modern::thread_pool classic{modern::thread_pool_options{
  .workers = 8,
  .backend = modern::queue_backend::direct_mutex
}};
```

All three backends implement the same internal `modern::detail::queue_strategy`
contract: `push`, `pop_local`, `pop_injected`, `steal_local`,
`steal_injected`. Backend selection is made once at pool construction; the
public `scheduler` and `submit` APIs do not change.

## Implementation

- `direct_mutex`: the existing three PMR-backed priority deques per worker.
  External and internal producers push to the same local mutex-protected deque.
- `injection_mutex_local`: each worker has a **separate** priority-preserving
  injection inbox and worker-local three-priority deque. External producers
  distribute submissions over inboxes, while a worker publishing more work
  uses its local deque. The two mutexes do not contend on external submission.
  A worker first drains local work, then its own inbox, then steals work from
  another local deque and finally assists a remote inbox if necessary.
- The mutex-backed inboxes are *MPSC-style ingress* paths, not lock-free
  queues and **not strictly single-consumer**: assisting workers may consume
  from another inbox.
- `injection_chase_lev`: each worker owns three bounded Chase-Lev deques
  (high/normal/low priority). The owner pushes and pops the bottom without a
  mutex. Other workers steal the top with a CAS. A separate bounded MPMC
  injection ring, also with three priority lanes, accepts all external
  submissions and owner-deque overflow. Any worker may consume from it.
  MPMC (rather than strict MPSC) is intentional: pool-wide semaphore permits
  do not identify an inbox owner. The shared ring is sized to hold at least
  `max_pending_tasks` entries per priority, ensuring accepted tasks can be
  published without requiring a mutex-backed overflow queue.
- Chase-Lev local lanes have a fixed, power-of-two ring capacity of at most
  1024 pointer slots (minimum two). Larger bursts spill into injection. The
  buffers never resize, so concurrent thieves cannot access freed ring
  storage. Tasks are stored in separately allocated PMR nodes: a winning CAS
  gives exclusive ownership of the node, which is reclaimed only by that
  winner. No hazard pointers or epoch reclamation are required for this fixed
  ring design.
- The lock-free paths use acquire/release and sequentially consistent atomic
  operations. Queue operations don't acquire mutexes; **the entire submit
  path is not guaranteed lock-free**: creating and destroying a PMR task node
  can use a locking allocator, and the existing pool-wide admission control,
  semaphore wakeup and task type erasure remain.
- Original mutex-backed strategies still use rotating `try_lock` stealing
  and PMR deques. All backends preserve priority lanes, the current
  pool-level queue capacity, shutdown/drain and exception rollback semantics.
  Priorities are local best-effort, not strict global ordering.

## Optional PMR task-node recycling

`injection_chase_lev_pooled` uses the same bounded Chase-Lev local queues,
priority lanes, global MPMC injection ring and pending-task limit as
`injection_chase_lev`. Only task-node allocation changes: instead of
allocating/freeing the PMR task node on each push/pop, a PMR-backed recycler
retains up to 16 empty nodes per worker (exclusive owner access) and up to
4096 empty nodes in a central mutex-protected freelist, additionally limited
by the configured max_pending capacity. Cross-worker freeing is safe: each
worker retires nodes into **its own** cache and spills extras centrally.
External producers use the central list. Recycled nodes release their old
captured callable before caching. Remaining nodes are deallocated through the
same PMR resource after all worker threads have joined.

The central freelist deliberately uses a mutex to avoid ABA/unsafe memory
reclamation in a lock-free stack. There are **no correctness or speedup claims**
until full Clang C++23 modules, stress/sanitizer and repeated A/B benchmarks
pass. The default queue backend and all existing callers stay unchanged.
The recycler can be paired with semaphore, atomic parking or Linux futex.

## Wakeup policy (independent of the queue backend)

A separate `worker_wakeup` setting now controls how a worker sleeps and wakes,
without modifying `queue_strategy` or `scheduler`. The default remains
`per_task_semaphore` for existing callers and ABI-conscious migration.
The optional `idle_parking` mode is experimental:

- Workers first check their local, injection and steal queues without taking
  a semaphore permit for every task. Successful dequeues continue immediately.
- On empty scans, workers register in a PMR-backed array of per-worker atomic
  parking slots. They **recheck** queues, accepted-task count and shutdown
  state after registration. A publisher reserves a parked worker with a CAS/
  exchange, advances its epoch, and notifies only that worker.
- A sequence-cst ordering between accepted-task publication, sleeper
  registration and the final pending-state check ensures that a task cannot
  be published concurrently with an unnoticed transition to sleep.
- If an accepted task exists but has not yet been claimed, a failed scan
  retries with CPU-relax and periodically yields instead of parking on
  undeliverable work. Do not change this check without proving progress.
- Shutdown notifies all parked workers when pending tasks reach zero. All
  sleepers must be joined before the parking slots are released.

This changes *worker parking*, not the underlying task queues. All three queue
backends can use either wakeup mode. The idle parking mode may consume more
CPU while pending but not yet published work exists; test under contention.
Its `std::atomic::wait` is not the positive-counter POSIX semaphore
implementation that previously caused the Linux libstdc++ 13 lost-permit
issue. It still needs sanitizer, repeated idle-wakeup, shutdown and lifecycle
acceptance before default promotion.

### Pending-count scan gate (experimental)

\`worker_wakeup::idle_parking_pending_gate\` shares the Chase-Lev queue,
task allocations, and \`std::atomic::wait\` channel of \`idle_parking\`.
It only changes **when** a worker scans queues. If the atomic accepted-but-
unclaimed count is zero, no queue can hold pending work, so the worker
skips the initial scan. After registering as a sleeper, it skips the
second scan and uses the existing seq-cst pending-state recheck. A
publisher's seq-cst admission increment and its notify-after-enqueue
ensure the worker either sees pending work or is visible for notification.
A pending count above zero still forces the full scan and retry loop;
a stopping pool with zero pending tasks exits normally.

This is **opt-in**; neither the default semaphore mode nor the existing
atomic parking, futex, or queue backends change. \`pool_gate_chase_lev_*\`
tests run the full 1/2/4/8/16 worker race, shutdown, capacity, cancellation
and allocation-failure matrix. Uninstrumented comparisons must confirm
both throughput and tail latency before this becomes a default.

### Linux direct-futex parking experiment

`worker_wakeup::idle_parking_futex` is an additional opt-in Linux-only
wakeup strategy. It retains the exact queue, pending-slot admission,
waiter selection, shutdown rules, and post-registration recheck used by
`idle_parking`, but replaces its 64-bit `std::atomic::wait` with a 32-bit
futex epoch and `FUTEX_WAIT_PRIVATE` / `FUTEX_WAKE_PRIVATE`.
The futex wait compares the epoch atomically with sleeping; EAGAIN means
a notifier raced ahead, while EINTR is retried. A sequentially consistent
publication/recheck protocol still protects against lost notifications.
On non-Linux systems, selecting this mode throws `std::invalid_argument`.

This is a **measurement hypothesis**, not a validated speedup.
A libstdc++ `_S_do_spin_v` perf symbol alone does not establish the
caller: the benchmark's `std::latch` may use the same implementation.
Verify full call graphs, compare normal runs and both parking modes, and
use stress/sanitizer tests before promoting the direct-futex implementation.
A future profiler should distinguish idle parks, spin retries, and
successful/failed steals via counters.

## Tradeoffs and forward compatibility

`injection_mutex_local` is deliberately conservative: it separates external
producer contention from worker-local access, but it does not remove mutexes
from the hot path, and can add scanning/queueing overhead. It is **opt-in**
until the controlled benchmark and sanitizer/stress matrices establish a win.

`injection_chase_lev` is implemented as a **bounded, opt-in experiment**.
It deliberately avoids the substantially harder problem of dynamically
resizing Chase-Lev storage and reclaiming slots while thieves are active.
The initial design allocates one PMR task node per enqueue; measure allocation
overhead before considering a pooled node allocator. PMR resources shared
between producer and worker threads must themselves be safe for concurrent
allocation and deallocation. Do not use a single unsynchronized pool resource
from multiple threads.

No backend is promoted until compiler, sanitizer, correctness and matched
throughput/latency tests pass. This release does not claim that lock-free is
necessarily faster than the existing mutex backend.

Queue priorities are enforced within each local deque or inbox; this is not
a strict global priority ordering across different worker queues.

## Test / acceptance

`recycling_node_pool_primitives` independently exercises reuse, PMR allocation
failure, cross-thread retirement, bounded retention and complete destruction.
`pool_pooled_park_*` repeats nine production scenarios for 1/2/4/8/16
workers, with additional pooled-futex and pooled-semaphore compatibility
matrices. The direct benchmark pair is
`modern_chase_lev_park_pooled` vs `modern_chase_lev_park`.


```sh
cmake --preset kpi
cmake --build --preset kpi -j8
ctest --test-dir build/kpi --output-on-failure -L pool
```

The stress suite exercises the same eight scenarios against all three backends,
including work produced by a running worker, external multi-producer load,
bounded queues, shutdown races, allocation failure and task cancellation.

The Linux `pool_futex_chase_lev_*` CTests repeat all nine parking
scenarios for 1/2/4/8/16 workers. The standalone primitive test also
exercises both atomic-wait and direct-futex parking on Linux.

The standalone `worker_parking_primitives` CTest validates concurrent
parking registration and wakeups. The `pool_park_chase_lev_*` matrix repeats
all pool lifecycle/race scenarios at 1/2/4/8/16 workers, plus `idle_waves`
that forces repeated idle->active transitions. Extra 1/4/16 worker tests
exercise idle parking with the two mutex-backed queue strategies.

The standalone `lock_free_queue_primitives` CTest also checks 2-, 32- and
256-slot rings under competing producers and consumers. Stress tests should
include the local-fanout, shutdown and allocation-failure cases at each worker
count and run with ASan/UBSan and TSan before any promotion.

No performance claim is made without matched, repeated A/B/C results on the
same hardware. Keep `dev` for source changes and merge it into
`bench/framework-comparison` for experiments, never the reverse.
