# modern-runtime

modern-runtime is a modular C++23 runtime for applications and libraries that need schedulers, thread pools, timers, tasks, coroutine-aware execution, and allocator-friendly async composition. It provides a reusable execution foundation so you can build async systems on top of a consistent runtime layer instead of re-solving scheduling, timing, cancellation, continuation chaining, and completion handoff in each project.

## Why modern_runtime

Use modern_runtime when you want to:

- run CPU work, timer work, and completion-based async work through one runtime surface
- compose pipelines with `modern::task<T>` continuations or coroutine-native `modern::runtime::Task<T>`
- propagate scheduler, allocator, stop, and trace context across async boundaries
- keep execution concerns separate from domain code such as networking, file handling, or protocol layers
- integrate external I/O backends without rebuilding a full runtime around them

`modern_runtime` consumes the shared trace contract from `modern_trace`. Runtime-owned behavior is propagation and environment seeding, not trace wire-format parsing or drain infrastructure.

modern_runtime is the layer below domain-specific async libraries. It owns scheduling, task orchestration, timers, coroutine task infrastructure, PMR-aware allocation points, and runtime adapters. It intentionally does not try to become a second filesystem or networking framework next to modern_io.

## What You Get

- `modern.exec`: schedulers, executor primitives, and priority-aware execution
- `modern.thread`: thread pools, work stealing, bounded queues, and affinity-aware workers
- `modern.timer`: delayed and periodic scheduling
- `modern.task`: `task<T>` continuations with `then`, `catching`, and `finally`
- `modern.runtime`: umbrella module for adapters, fibers, senders, and coroutine-native runtime tasks
- `modern.memory`: a small `std::pmr`-oriented allocation surface
- `modern.sync` and `modern.platform`: synchronization and platform helpers used by the runtime surface

## Build And Run

Requirements:

- C++23 toolchain
- CMake 3.28+
- Clang with module support is the primary validated setup
- Ninja is used by the included presets

Debug build:

```bash
# Source builds resolve modern_trace automatically:
# sibling ../modern_trace when present, otherwise GitHub fetch.
# Override with -DMODERN_TRACE_PROVIDER=package|local|fetch
# and -DMODERN_TRACE_ROOT=../modern_trace.
cmake --preset debug
cmake --build --preset debug
./build/debug/exec_smoke
./build/debug/async_demo
./build/debug/smoke_test
```

Other presets:

```bash
cmake --preset release
cmake --build --preset release

cmake --preset sanitize
cmake --build --preset sanitize
```

The local validated setup is Clang 18.1.3, Ninja, CMake 3.28.3, and libstdc++ 13. GCC 13 is not an active validation target at the moment.

## Add It To Your Project

The repository builds a `modern_runtime` library target and resolves `modern_trace` as a sibling or fetched dependency.

```cmake
add_subdirectory(modern_runtime)

target_link_libraries(my_app PRIVATE modern_runtime)
```

If you want the full umbrella surface, import `modern.runtime`. If you want tighter compile-time boundaries, import only the modules you need.

## Quick Start

```cpp
import modern.runtime;

#include <chrono>
#include <iostream>
#include <string>

int main()
{
	using namespace std::chrono_literals;

	modern::thread_pool cpu{4};
	modern::scheduled_executor timers{cpu};

	auto value = modern::submit(cpu, []
		{
			return 21;
		})
		.then([](int x)
		{
			return x * 2;
		})
		.get();

	auto delayed = modern::schedule_after(timers, 100ms, []
		{
			return std::string{"done"};
		})
		.then([](std::string text)
		{
			return text + " after a timer";
		})
		.get();

	std::cout << value << "\n";
	std::cout << delayed << "\n";

	timers.shutdown();
	timers.join();
	cpu.shutdown();
	cpu.join();
}
```

That is the core usage pattern:

- create an execution context such as `thread_pool` or `scheduled_executor`
- submit work with `modern::submit(...)`
- compose follow-up work with continuations or coroutines
- wait, stop, or hand results onward at your application boundary

## How To Use modern_runtime

### Choose The Import Surface

Use the umbrella when you want the complete runtime layer:

```cpp
import modern.runtime;
```

Use narrow imports when you want explicit boundaries:

```cpp
import modern.exec;
import modern.task;
import modern.timer;
import modern.thread;
import modern.sync;
import modern.memory;
import modern.platform;
```

### Choose The Task Model

- Use `modern::task<T>` when you want continuation-oriented pipelines with `then`, `catching`, and `finally`.
- Use `modern::runtime::Task<T>` when you want a coroutine-native task type and runtime environment propagation.
- Use `modern::runtime::ResultTask<T, E>` or `StatusTask<E>` when your coroutine surface should model `std::expected` results directly.

### Schedule Work

- `modern::submit(...)` runs work on a scheduler or thread pool and returns a task you can compose.
- `modern::schedule_after(...)` and `modern::schedule_at(...)` run delayed work.
- `scheduled_executor::schedule_fixed_rate(...)` is the periodic entry point for repeated jobs.

### Control Memory, Cancellation, And Execution

- `thread_pool` and `scheduled_executor` can use `std::pmr::memory_resource` for runtime allocations.
- task continuations can keep propagating a parent resource or accept an explicit override.
- runtime adapters accept `std::stop_token` so cancelled work can fail early before execution starts.
- schedulers support `modern::scheduler_priority` with `high`, `normal`, and `low`.
- thread pools can pin workers to configured CPU affinities.

### Integrate External I/O Instead Of Rebuilding It

- `modern::bind_io<R>(...)` adapts completion-based I/O starts into `task<R>`.
- `modern::adapt_modern_io(service)` wraps modern_io-like services that expose `start(...)` or `submit(...)`.
- `modern::adapt_modern_io_uring(service)` and `modern::adapt_modern_io_poller(service)` add backend-aware integration for `io_uring`, `epoll`, and `kqueue` style services.

This keeps the ownership line clear: modern_runtime provides the runtime substrate, while modern_io or another domain layer owns file, socket, and transport semantics.

### Bridge Other Async Models

- `modern::as_task<R>(sender, scheduler, resource)` adapts senders into `task<R>`.
- `modern::fiber_scheduler` provides cooperative scheduler-based fiber requeueing and grouped stop/join behavior.
- `modern::runtime::TaskEnvironment` carries scheduler, frame allocator, trace context, and stop state through coroutine execution.

## Repository Guide

- [modules/exec/modern.exec.cppm](modules/exec/modern.exec.cppm): scheduler, priorities, and inline executor
- [modules/task/modern.task.cppm](modules/task/modern.task.cppm): `task<T>`, `task_scope`, continuations, and coroutine bridge
- [modules/thread/modern.thread.cppm](modules/thread/modern.thread.cppm): thread pool with work stealing, affinity, and prioritized scheduling
- [modules/timer/modern.timer.cppm](modules/timer/modern.timer.cppm): delayed and periodic scheduling
- [modules/runtime/coroutine_task.cppm](modules/runtime/coroutine_task.cppm): generic runtime coroutine task, result-task aliases, and `TaskEnvironment`
- [modules/runtime/fiber_scheduler.cppm](modules/runtime/fiber_scheduler.cppm): cooperative fiber scheduler over the scheduler surface
- [modules/runtime/io_bridge.cppm](modules/runtime/io_bridge.cppm): bridge from external completion sources to `task<T>`
- [modules/runtime/io_backend.cppm](modules/runtime/io_backend.cppm): backend metadata and abstraction for `io_uring`, `epoll`, and `kqueue`
- [modules/runtime/modern_io_adapter.cppm](modules/runtime/modern_io_adapter.cppm): concrete shim for modern_io-like services over `bind_io`
- [modules/runtime/io_uring_adapter.cppm](modules/runtime/io_uring_adapter.cppm): `io_uring`-specific adapter over the modern_io shim
- [modules/runtime/poller_adapter.cppm](modules/runtime/poller_adapter.cppm): shared `epoll`/`kqueue` abstraction over the modern_io shim
- [modules/runtime/sender_bridge.cppm](modules/runtime/sender_bridge.cppm): bridge from senders to `task<T>`
- [modules/memory/modern.memory.cppm](modules/memory/modern.memory.cppm): small `std::pmr` surface
- [modules/modern.runtime.cppm](modules/modern.runtime.cppm): umbrella module and integration functions
- [examples/async_demo.cpp](examples/async_demo.cpp): runnable reference demo
- [tests/smoke_test.cpp](tests/smoke_test.cpp): focused runtime regression coverage
