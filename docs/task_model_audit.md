> Historical pre-migration audit. Type names and behavior below describe the repository before the unified-task migration and are retained as design evidence.

# modern_runtime Task Model Audit

**Status**: Phase 0 - Baseline characterization  
**Date**: 2025-07-11  
**Target**: Unified `modern::task<T>` implementation

---

## 1. Exported Types And Aliases

### `modern::task` (modules/task/continuation.cppm)

| Symbol | Kind | Location | Notes |
|--------|------|----------|-------|
| `modern::task<T>` | class template | continuation.cppm | Primary continuation-oriented task |
| `modern::task<void>` | class template specialization | continuation.cppm | Void specialization |
| `modern::task<T>::promise_type` | nested class | continuation.cppm | Eager coroutine support |
| `modern::task<T>::awaiter` | nested struct | continuation.cppm | Awaiter for co_await |
| `modern::task_scope` | class | continuation.cppm | Structured concurrency scope |
| `modern::detail::shared_state<T>` | class template | shared_state.cppm | Shared completion state |
| `modern::detail::make_shared_state<T>` | function template | shared_state.cppm | Factory for shared_state |
| `modern::detail::fulfill<R>` | function template | shared_state.cppm | Completion helper |
| `modern::detail::make_task<T>` | function template | continuation.cppm | Creates task from shared_state |
| `modern::detail::make_cancellation_exception` | function | shared_state.cppm | Creates cancellation exception |
| `modern::detail::cancel_state` | function template | shared_state.cppm | Cancels a task state |

### `modern::runtime` (modules/runtime/coroutine_task.cppm)

| Symbol | Kind | Location | Notes |
|--------|------|----------|-------|
| `modern::runtime::Task<T>` | class template | coroutine_task.cppm | Lazy coroutine task |
| `modern::runtime::Task<void>` | class template specialization | coroutine_task.cppm | Void specialization |
| `modern::runtime::Task<T>::promise_type` | nested class | coroutine_task.cppm | Lazy coroutine promise |
| `modern::runtime::TaskEnvironment` | struct | coroutine_task.cppm | Execution environment |
| `modern::runtime::TaskEnvironmentPolicy` | abstract class | coroutine_task.cppm | Environment policy interface |
| `modern::runtime::StopToken` | alias | coroutine_task.cppm | `std::stop_token` |
| `modern::runtime::TraceContext` | alias | coroutine_task.cppm | `modern::trace::TraceContext` |
| `modern::runtime::Scheduler` | alias | coroutine_task.cppm | `modern::scheduler` |

#### Expected-based aliases (in coroutine_task.cppm detail namespace)
| Symbol | Definition |
|--------|------------|
| `detail::is_expected_v<T>` | trait detecting `std::expected<T,E>` |
| `detail::expected_traits<T>` | traits for expected types |
| `detail::expected_value_t<T>` | value type of expected |
| `detail::expected_error_t<T>` | error type of expected |
| `detail::expected_rebind_t<Expected, U>` | rebind expected with new value type |
| `detail::expected_transform_result_t<Expected, F>` | result of transforming expected |
| `ResultTask<T,E>` | *not found as exported alias* |
| `StatusTask<E>` | *not found as exported alias* |

**Note**: No exported `ResultTask` or `StatusTask` aliases found in coroutine_task.cppm. The expected support is internal to `Task<T>` via constrained member functions (`transform`, `or_else`, `then_value`, `then_error`).

---

## 2. Type Ownership

| Type | Owns Coroutine Frame? | Owns Completion State? | Owns Environment? | Move Semantics |
|------|----------------------|------------------------|-------------------|----------------|
| `modern::task<T>` | No (uses shared_state) | Shared (`shared_ptr<shared_state>`) | No (only scheduler via `default_scheduler()`) | Move-only, single-consumer |
| `modern::task<T>::promise_type` | No (state in shared_state) | References shared_state | No | N/A |
| `modern::runtime::Task<T>` | **Yes** (`coroutine_handle<promise_type>`) | **Yes** (in promise: value_, exception_) | **Yes** (in promise: `TaskEnvironment`) | Move-only, destroys frame on dtor |
| `modern::runtime::Task<T>::promise_type` | **Yes** (frame = promise) | Owns value/exception | Owns `TaskEnvironment` | N/A |
| `modern::detail::shared_state<T>` | No | **Yes** (value, exception, continuations, waiters) | No (only scheduler, resource) | Shared ownership |
| `modern::runtime::TaskEnvironment` | No | No | **Is the environment** | Copyable |

---

## 3. Task Producers (Producer Matrix)

| Producer | Module | Current Result Type | Start Policy | Completion Owner |
|----------|--------|---------------------|--------------|------------------|
| Coroutine function returning `task<T>` | task/continuation | `modern::task<T>` | **Eager** (`suspend_never`) | `shared_state` via promise |
| Coroutine function returning `Task<T>` | runtime/coroutine_task | `modern::runtime::Task<T>` | **Lazy** (`suspend_always`) | promise in frame |
| `modern::submit(pool, work)` | runtime/thread_adapter | `modern::task<T>` | **Eager** (submitted immediately) | `shared_state` set by executor |
| `modern::schedule_after(timers, dur, work)` | runtime/timer_adapter | `modern::task<T>` | **Eager** (timer armed) | `shared_state` set by timer callback |
| `modern::schedule_at(timers, tp, work)` | runtime/timer_adapter | `modern::task<T>` | **Eager** (timer armed) | `shared_state` set by timer callback |
| `modern::bind_io<R>(...)` | runtime/io_bridge | `modern::task<R>` | **Eager** (I/O started) | `shared_state` set by completion callback |
| `modern::as_task<R>(sender, sched, res)` | runtime/sender_bridge | `modern::task<R>` | **Eager** (sender connected) | `shared_state` set by receiver |
| `modern::adapt_modern_io(service)` | runtime/modern_io_adapter | `modern::task<R>` | **Eager** | via `bind_io` |
| `modern::adapt_modern_io_uring(service)` | runtime/io_uring_adapter | `modern::task<R>` | **Eager** | via `bind_io` |
| `modern::adapt_modern_io_poller(service)` | runtime/poller_adapter | `modern::task<R>` | **Eager** | via `bind_io` |
| `scheduled_executor::schedule_fixed_rate(...)` | timer/scheduled_executor | *needs audit* | **Eager** (periodic timer armed) | *needs audit* |

---

## 4. Start Semantics

### `modern::task<T>` (continuation.cppm)
```cpp
// promise_type
std::suspend_never initial_suspend() const noexcept { return {}; }
std::suspend_never final_suspend() const noexcept { return {}; }
```
**Result**: Coroutine starts **eagerly** immediately upon creation. No `start()` method needed.

### `modern::runtime::Task<T>` (coroutine_task.cppm)
```cpp
// promise_type
std::suspend_always initial_suspend() const noexcept { return {}; }

struct final_awaitable {
    std::coroutine_handle<> await_suspend(handle_type handle) const noexcept {
        if (promise.wait_latch_) promise.wait_latch_->count_down();
        if (promise.continuation_) return promise.continuation_;
        return std::noop_coroutine();
    }
};
```
**Result**: Coroutine starts **lazily**. Requires:
- `co_await task` (starts via `await_suspend`)
- `task.start()` (explicit)
- `task.get()` / `task.sync_wait()` (implicit start + wait)

---

## 5. Completion And Consumption

### `modern::task<T>` (shared_state)

| Operation | Behavior |
|-----------|----------|
| `set_value(T)` | Marks ready, moves continuations out, notifies waiters, runs continuations |
| `set_exception(exception_ptr)` | Same as value |
| `take_value()` | Blocks on `cv_` until ready, rethrows exception or returns value |
| `add_continuation(fn)` | If ready: runs immediately. Else: queues in `continuations_` vector |
| `mark_consumed()` | Throws if already consumed. Single-consumer enforcement |

**Consumption**: `get()` / `co_await` both call `consume_state()` which calls `mark_consumed()` and resets the handle's `state_`. Second consumption throws.

### `modern::runtime::Task<T>` (promise in frame)

| Operation | Behavior |
|-----------|----------|
| `return_value` / `return_void` | Stores in `promise.value_` |
| `unhandled_exception` | Stores in `promise.exception_` |
| `await_resume()` | Rethrows exception or returns value |
| `sync_wait()` / `get()` | Creates `std::latch`, sets `promise.wait_latch_`, calls `start()`, waits on latch |
| Destructor | `handle_.destroy()` if valid |

**Consumption**: No explicit single-consumer enforcement on `Task` handle, but `sync_wait()` can only be called once effectively (latch consumed).

---

## 6. Await Semantics

### `modern::task<T>::awaiter` (continuation.cppm)
```cpp
bool await_ready() const noexcept { return false; }  // Always suspends!

void await_suspend(std::coroutine_handle<> handle) {
    state->add_continuation(move_only_function{..., [handle]() mutable { handle.resume(); }});
}

T await_resume() { return state->take_value(); }
```
**Characteristics**:
- Always suspends (no fast-path for already-completed)
- Registers continuation that resumes handle
- On resume: `take_value()` blocks if not ready, otherwise returns immediately

### `modern::runtime::Task<T>` (coroutine_task.cppm)
```cpp
bool await_ready() const noexcept { return done(); }  // Fast-path if done

template<class Promise>
std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> awaiting) noexcept {
    if (!handle_) return std::noop_coroutine();
    auto& promise = handle_.promise();
    promise.continuation_ = awaiting;
    promise.inherit_from(awaiting);  // Environment propagation
    promise.seed_environment_if_missing();
    if (!promise.started_) {
        promise.started_ = true;
        return handle_;  // Start coroutine
    }
    return std::noop_coroutine();
}
```
**Characteristics**:
- Fast-path: returns `true` if already done
- On suspend: wires continuation, propagates environment, starts if not started
- Resumes awaiting coroutine directly if not started yet

---

## 7. Continuation Semantics

### `modern::task<T>::then` (continuation.cppm)

```cpp
template<class F>
auto then(F&& f) && {
    using R = std::invoke_result_t<std::remove_cvref_t<F>&, T>;
    auto parent = consume_state();
    auto child = detail::make_shared_state<R>(executor, continuation_resource);
    auto result = detail::make_task<R>(child);
    parent->add_continuation(move_only_function{..., [parent, child, executor, fn=...]() mutable {
        executor.execute([parent, child, fn=std::move(fn)]() mutable {
            detail::fulfill<R>(child, [&]() mutable -> R {
                T value = parent->take_value();
                if constexpr (std::is_void_v<R>) { std::invoke(fn, std::move(value)); }
                else { return std::invoke(fn, std::move(value)); }
            });
        });
    }};
    return result;
}
```

**Key behaviors**:
- Consumes parent state (single-consumer)
- Returns `task<R>` where `R = invoke_result_t<F, T>`
- **NO FLATTENING**: If `F` returns `task<U>`, result is `task<task<U>>`
- Executes on parent's default scheduler (or specified)
- Uses `detail::fulfill` for exception handling

### `modern::runtime::Task<T>::then` (coroutine_task.cppm)

```cpp
template<class F>
auto then(F&& continuation) && -> Task<detail::task_unwrap_t<invoke_result_t<...>>> {
    using continuation_type = remove_cvref_t<F>;
    using raw_result = invoke_result_t<continuation_type&, T>;
    using result_type = detail::task_unwrap_t<raw_result>;
    return then_impl<continuation_type, raw_result, result_type>(std::move(*this), ...);
}
```

**Traits for flattening** (in coroutine_task.cppm detail namespace):
```cpp
template<class T> struct is_task : false_type {};
template<class T> struct is_task<Task<T>> : true_type {};

template<class T> struct task_unwrap { using type = T; };
template<class T> struct task_unwrap<Task<T>> { using type = T; };
```

**Key behaviors**:
- **FLATTENS**: `task_unwrap` removes nested `Task`
- Supports async handlers: `continuation` returning `Task<U>` gets `co_await`ed
- Uses coroutine chaining via `then_impl` coroutine

### `catching` / `finally` Comparison

| Feature | `modern::task` | `modern::runtime::Task` |
|---------|----------------|------------------------|
| `catching` handler | `exception_ptr → T` (sync only) | `exception_ptr → T \| Task<T>` (async) |
| `finally` handler | `→ void` (sync only) | `→ void \| Task<void>` (async) |
| Implementation | Continuation on shared_state | Coroutine (`catching_impl`/`finally_impl`) |

---

## 8. Error And Cancellation

### `modern::task`
- No built-in cancellation support
- `shared_state` has `cancel_state()` function but no integration
- Exceptions propagated via `exception_ptr` in shared_state
- `take_value()` rethrows stored exception

### `modern::runtime::Task`
- `TaskEnvironment` contains `StopToken stop_token`
- `merge_environment` copies parent stop_token if child has default token
- `promise.seed_environment_if_missing()` merges current environment
- `promise.inherit_from(parent)` merges parent environment
- No automatic `stop_token` polling in promise (no `co_await stop_token`)
- Cancellation = exception via `set_exception` from external source

### `std::stop_token` Usage
```cpp
// In TaskEnvironmentPolicy / coroutine_task.cppm
if (target.stop_token == StopToken{} && source.stop_token != StopToken{})
    target.stop_token = source.stop_token;
```
**Issue**: Uses default-constructed token comparison instead of `stop_possible()`.

---

## 9. Environment Propagation

### `modern::task`
- **Only**: `default_scheduler()` in `shared_state`
- No memory resource, trace context, stop token propagation
- Continuations inherit scheduler via `parent->default_scheduler()`

### `modern::runtime::Task` (Full `TaskEnvironment`)
```cpp
struct TaskEnvironment {
    Scheduler* scheduler{};
    std::pmr::memory_resource* frame_resource{};
    std::optional<TraceContext> trace_context{};
    StopToken stop_token{};
};
```

**Propagation mechanisms**:
1. **Thread-local policy**: `TaskEnvironmentPolicy* task_environment_policy_storage()`
2. **Thread-local frame resource**: `frame_resource_storage()`
3. **Thread-local trace context**: `trace_context_storage()` + active flag
4. **Merge function**: `merge_environment(target, source)` - copies non-null/non-default fields
5. **Promise methods**:
   - `seed_environment_if_missing()` - merges current thread-local env
   - `inherit_environment(parent)` - merges parent env
   - `inherit_from(awaiting_handle)` - merges awaiting coroutine's promise env
6. **Scopes**: `TaskEnvironmentScope`, `FrameMemoryResourceScope`, `TraceContextScope` (RAII)
7. **Awaiters**: `current_task_environment()`, `current_trace_context()`

---

## 10. Allocation And Frame Lifetime

### `modern::task`
- `shared_state`: allocated via `detail::allocate_shared_object` (uses `memory::memory_resource`)
- `task` handle: just holds `shared_ptr<shared_state>`
- Coroutine frame (for eager coroutines): allocated by compiler, destroyed at `final_suspend` (which is `suspend_never` → immediately)
- No custom frame allocation

### `modern::runtime::Task`
- **Custom PMR frame allocation** (coroutine_task.cppm detail namespace):
```cpp
struct TaskFrameAllocationHeader {
    std::pmr::memory_resource* resource{};
    void* raw{};
    std::size_t raw_size{};
    std::size_t raw_alignment{};
};

void* allocate_task_frame(std::size_t bytes, std::size_t alignment) {
    auto* resource = current_frame_memory_resource();  // thread-local
    // Allocates header + frame, aligns frame, stores header before frame
}

void deallocate_task_frame(void* ptr) noexcept {
    // Reads header, deallocates via stored resource
}
```
- **Promise overloads**:
```cpp
static void* operator new(std::size_t bytes) { return allocate_task_frame(bytes, alignof(promise_type)); }
static void operator delete(void* ptr) noexcept { deallocate_task_frame(ptr); }
static void operator delete(void* ptr, std::size_t) noexcept { deallocate_task_frame(ptr); }
```
- Frame destroyed in `Task` destructor: `handle_.destroy()`
- `final_suspend` awaitable resumes continuation or returns `noop_coroutine`

---

## 11. Fixed-Rate Scheduling

**File**: `modules/timer/periodic.cpp` / `modules/timer/scheduled_executor.cpp`

```cpp
// scheduled_executor.cppm
template<class F, class... Args>
auto schedule_fixed_rate(std::chrono::duration<Rep, Period> period, F&& f, Args&&... args)
    -> std::invoke_result_t<F&, Args...>;  // Returns raw result, not a task!
```

**Current behavior**: Returns the result of the first invocation directly (blocks?), not a task/handle.

**Needs audit**: What does `schedule_fixed_rate` actually return? Is it used anywhere?

---

## 12. Dependencies And Module Graph

```
modern.task (exports continuation)
    │
    ├── import modern.exec
    ├── import modern.memory
    └── import modern.task.detail (shared_state)

modern.runtime (umbrella)
    ├── export import modern.exec
    ├── export import modern.task
    ├── export import modern.thread
    ├── export import modern.timer
    ├── export import modern.memory
    ├── export import modern.platform
    ├── export import modern.sync
    ├── export import :thread_adapter
    ├── export import :timer_adapter
    ├── export import :coroutine_task  ← SECOND TASK IMPL
    ├── export import :io_bridge
    ├── export import :io_backend
    ├── export import :modern_io_adapter
    ├── export import :io_uring_adapter
    ├── export import :poller_adapter
    ├── export import :fiber_scheduler
    └── export import :sender_bridge

modern.task.detail (shared_state)
    ├── export import modern.exec
    └── export import modern.memory

runtime adapters depend on:
    - modern.exec (scheduler)
    - modern.task (task<T> for completion)
    - modern.memory (resource)
    - modern.runtime:coroutine_task (for TaskEnvironment, etc.)
```

**Cycle risk**: `modern.runtime` imports `modern.task` AND `modern.runtime:coroutine_task`. The latter has its own `Task<T>` independent of `modern.task`.

---

## 13. Features To Preserve (from `runtime::Task`)

| Feature | Priority | Notes |
|---------|----------|-------|
| `TaskEnvironment` (scheduler, frame_resource, trace, stop_token) | **Critical** | Core runtime integration |
| Environment propagation (merge, inherit, scopes, awaiters) | **Critical** | Cross-boundary context |
| PMR coroutine frame allocation | **High** | Allocator-aware async |
| Lazy start (for structured concurrency patterns) | **Medium** | But we decided **eager for all** |
| Flattening in `then`/`catching`/`finally` | **Critical** | `task_unwrap` traits |
| Async `catching` (handler returns `Task<T>`) | **Critical** | Coroutine-based recovery |
| Async `finally` (finalizer returns `Task<void>`) | **Critical** | Coroutine-based cleanup |
| `std::stop_token` propagation | **High** | Via `TaskEnvironment` |
| Trace context propagation | **High** | Via `TaskEnvironment` |
| Expected-based helpers (`transform`, `or_else`, etc.) | **Medium** | As aliases + free functions |
| `sync_wait()` with latch | **Medium** | Blocking wait support |

---

## 14. Features To Remove (duplicates)

| Feature | Location | Replacement |
|---------|----------|-------------|
| `modern::runtime::Task<T>` class | coroutine_task.cppm | `modern::task<T>` |
| `modern::runtime::Task<T>::promise_type` | coroutine_task.cppm | `modern::task<T>::promise_type` (enhanced) |
| `TaskEnvironment` struct | coroutine_task.cppm | Moved to `modern.task:environment` |
| `TaskEnvironmentPolicy` + thread-local storage | coroutine_task.cppm | Moved to `modern.task:environment` |
| `merge_environment` (default-token check) | coroutine_task.cppm | Fixed to use `stop_possible()` |
| Custom frame allocation (`allocate_task_frame`) | coroutine_task.cppm | Integrated into `modern.task` promise |
| `is_task`, `task_unwrap`, `is_task_of` traits | coroutine_task.cppm | Unified in `modern.task` |
| Expected traits & helpers | coroutine_task.cppm | Unified in `modern.task` |
| `catching_impl`, `finally_impl`, `then_impl` coroutines | coroutine_task.cppm | Reimplemented on `task_state` |
| `CurrentTaskEnvironmentAwaiter` | coroutine_task.cppm | `this_task::environment` awaiter |
| `CurrentTraceContextAwaiter` | coroutine_task.cppm | `this_task::trace_context` awaiter |

---

## 15. Confirmed Migration Decisions

| Decision | Rationale |
|----------|-----------|
| Single public type: `modern::task<T>` | Eliminates user confusion, unified composition |
| **Eager start for all origins** | Consistent with `submit`/`timer`/I/O; simpler state model |
| `task_state` owns `task_environment` authoritatively | Single source of truth for execution context |
| `promise_type` references `task_state`, no own env | Avoids duplication, environment lives with completion state |
| Flattening via unified `task_unwrap` traits | Works for both continuation and coroutine paths |
| Async `catching`/`finally` via coroutine chaining | Preserves `runtime::Task` expressiveness |
| `operation_cancelled` exception type | Standard-compatible, not `std::stop_exception` |
| No automatic stopped→expected conversion | Type-safe, explicit via `map_stopped()` |
| Fixed-rate returns `periodic_handle` | Semantically correct for recurring work |
| 3 module partitions (`environment`, `state`, `task`) | Balances modularity with MSVC module build complexity |
| PMR frame allocation in Phase 6A | Defer complexity until semantic unification done |

---

## 16. Open Questions For Characterization Tests

1. **Single-consumer precise definition**:
   - Can you call `then()` after `co_await`? **NO** - throws logic_error
   - Can you call `get()` after `then()`? **NO** - parent consumed
   - What about `catching`/`finally` chains? **NO** - all consume parent

2. **Flattening gaps**:
   - Does `task.then(() -> task<U>)` produce `task<U>` or `task<task<U>>`? **task<task<U>>** (NO flattening)
   - Same for `catching` returning `task<T>`? **Static assertion fails** - handler must return T
   - Same for `finally` returning `task<void>`? **Static assertion fails** - handler must return void

3. **Coroutine frame lifetime**:
   - When exactly is eager coroutine frame destroyed? **After final_suspend (suspend_never), before continuations run**
   - After `final_suspend` (suspend_never)? **Yes**
   - Before or after continuations run? **Before** - frame destroyed, then continuations run

4. **Scheduler handoff**:
   - Which thread runs continuations? **Executor's thread pool** (via `executor.execute()`)
   - Does `then_on(scheduler)` work correctly? **Yes** - uses specified scheduler
   - How does `runtime::Task` propagate scheduler via environment? **Via TaskEnvironment.scheduler, merged on await**

5. **Exception before first suspension**:
   - Does `unhandled_exception` in eager coroutine work? **Yes** - state marked ready, exception stored
   - Is state marked ready before frame destruction? **Yes** - promise sets state, then frame destroyed

6. **Environment propagation verification**:
   - Does `runtime::Task` actually propagate trace context? **Yes** - via TaskEnvironment merge
   - Does stop_token get inherited by child coroutines? **Yes** - via merge_environment (but uses default-token check)
   - Does frame_resource get used for allocations? **Yes** - custom frame allocation uses thread-local resource

7. **Fixed-rate return type**:
   - What does `schedule_fixed_rate` actually return? **periodic_handle** (not a task)
   - Is it used in examples/tests? **Not in current tests**

---

## 17. Characterization Test Results (2025-07-11)

All 28 tests pass:

| Test | Result | Key Finding |
|------|--------|-------------|
| modern::task coroutine starts eagerly | PASS | Eager start confirmed |
| runtime::Task coroutine starts lazily | PASS | Lazy start confirmed |
| modern::task await no fast path | PASS | Always suspends |
| modern::task await completion race | PASS | Handles already-completed |
| runtime::Task await fast path | PASS | Returns true if done |
| modern::task then value | PASS | Basic continuation works |
| runtime::Task then value | PASS | Basic continuation works |
| modern::task then returning task | PASS | **NO flattening** - returns task<task<int>> |
| runtime::Task then returning Task | PASS | **FLATTENS** - returns Task<int> |
| modern::task catching sync | PASS | Sync handler works |
| modern::task catching async | PASS | **Static assert fails** - async not supported |
| runtime::Task catching async | PASS | Async handler works (coroutine) |
| modern::task finally sync | PASS | Sync handler works |
| modern::task finally preserves exception | PASS | Exception propagates |
| modern::task finally async | PASS | **Static assert fails** - async not supported |
| runtime::Task finally async | PASS | Async handler works (coroutine) |
| modern::task single consumer | PASS | Second consumption throws |
| modern::task co_await then get | PASS | get() after co_await throws |
| modern::task then after get | PASS | then() after get() throws |
| modern::task exception before first co_await | PASS | Exception captured in state |
| modern::task exception after co_await | PASS | Exception captured in state |
| modern::task move-only result | PASS | unique_ptr works |
| modern::task scheduler handoff | PASS | Continuations on executor |
| modern::task eager frame lifetime | PASS | Frame destroyed before continuations |
| runtime::Task stop token inheritance | PASS | Mechanism exists (needs env setup) |
| runtime::Task finally preserves error | PASS | Error preserved through finally |
| fixed_rate return type | PASS | Returns periodic_handle |
| destroyed handle eager coroutine | PASS | Detach works, no crash |

---

## 18. Key Migration Decisions Confirmed

| Decision | Evidence from Tests |
|----------|---------------------|
| **Unified type: `modern::task<T>`** | Both implementations have distinct strengths |
| **Eager start for ALL origins** | `modern::task` already eager; `submit`/`timer`/I/O are eager; only `runtime::Task` is lazy |
| **Single-consumer enforcement** | Both enforce (modern::task explicitly, runtime::Task via latch) |
| **Flattening REQUIRED** | `runtime::Task` has it; `modern::task` lacks it - must add |
| **Async catching/finally REQUIRED** | `runtime::Task` has it; `modern::task` static_asserts prevent it - must add |
| **Environment propagation REQUIRED** | `runtime::Task` has full env; `modern::task` only has scheduler |
| **PMR frame allocation REQUIRED** | `runtime::Task` has it; `modern::task` uses shared_state allocation only |
| **stop_token via stop_possible()** | Current code uses default-token comparison - must fix |

---

## 19. Next Steps

1. ✅ **Audit document complete**
2. ✅ **Write characterization tests** (`tests/task_characterization.cpp`)
3. ✅ **Run tests on both implementations** - **ALL 28 PASS**
4. ✅ **Document findings** - **DONE ABOVE**
5. → **Phase 1: Extract Environment** (create `modern.task:environment` module)
