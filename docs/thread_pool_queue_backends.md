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

// Make the original backend explicit for A/B comparisons.
modern::thread_pool classic{modern::thread_pool_options{
  .workers = 8,
  .backend = modern::queue_backend::direct_mutex
}};
```

Both backends implement the same internal `modern::detail::queue_strategy`
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
- The inboxes are *MPSC-style ingress* paths, not lock-free queues and
  **not strictly single-consumer**: assisting workers may consume from another
  inbox. This is essential for progress with a single pool-wide counting
  semaphore whose permits are not directed to the inbox owners.
- Work stealing uses rotating, nonblocking `try_lock` probes; an empty scan
  retains its acquired permit and retries. All task publication is completed
  before the matching semaphore release.
- Each logical queue uses `std::pmr::deque` and the caller-provided
  `memory_resource`. Neither backend removes PMR. Task callable ownership,
  queue-capacity enforcement, shutdown/drain and exception rollback retain
  the existing pool protocol.

## Tradeoffs and forward compatibility

`injection_mutex_local` is deliberately conservative: it separates external
producer contention from worker-local access, but it does not remove mutexes
from the hot path, and can add scanning/queueing overhead. It is **opt-in**
until the controlled benchmark and sanitizer/stress matrices establish a win.

A Chase-Lev owner/stealer deque can later replace the local queue backend under
the same queue-strategy interface, but is **not implemented or advertised as
available** in this change. It requires explicit design for resizing, memory
reclamation, priority semantics, capacity, and clean shutdown.

Queue priorities are enforced within each local deque or inbox; this is not
a strict global priority ordering across different worker queues.

## Test / acceptance

```sh
cmake --preset kpi
cmake --build --preset kpi -j8
ctest --test-dir build/kpi --output-on-failure -L pool
```

The stress suite exercises the same eight scenarios against both backends,
including work produced by a running worker, external multi-producer load,
bounded queues, shutdown races, allocation failure and task cancellation.

No performance claim is made without matched, repeated A/B results on the
same hardware. Keep `dev` for source changes and merge it into
`bench/framework-comparison` for experiments, never the reverse.
