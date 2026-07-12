# modern_runtime Guide

The canonical project overview and examples are maintained in
[../README.md](../README.md).

## Unified task model

`modern::task<T>` is the only public task abstraction. It is both a coroutine
return type and a continuation pipeline:

```cpp
modern::task<int> operation(modern::thread_pool& pool)
{
  auto value = co_await modern::submit(pool, [] { return 21; });
  co_return value * 2;
}

auto result = modern::submit(pool, work)
  .then(transform)
  .catching(recover)
  .finally(cleanup);
```

Task-returning continuation functions are flattened. Tasks are move-only and
single-consumer. Cancellation is reported with
`modern::operation_cancelled`.

## Environment

`modern::task_environment` contains the active scheduler, PMR frame resource,
trace context, and `std::stop_token`. Use `modern::task_environment_scope`,
`modern::frame_resource_scope`, or `modern::trace_context_scope` for scoped
overrides.

Inside a `modern::task` coroutine, access inherited values through:

```cpp
auto env       = co_await modern::this_task::environment();
auto scheduler = co_await modern::this_task::scheduler();
auto resource  = co_await modern::this_task::memory_resource();
auto trace     = co_await modern::this_task::trace_context();
auto token     = co_await modern::this_task::stop_token();
```

## Runtime adapters

The `modern.runtime` umbrella exports thread and timer adapters, completion
I/O bridges, modern_io, io_uring and poller adapters, sender bridging, and the
fiber scheduler. Their asynchronous task-producing operations return
`modern::task<T>`.
