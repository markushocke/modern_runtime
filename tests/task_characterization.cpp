#include <atomic>
#include <chrono>
#include <coroutine>
#include <expected>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

import modern.runtime;

using namespace std::chrono_literals;

namespace
{
class async_test_executor
{
public:
  template<class F>
  void execute(F&& operation)
  {
    std::thread(std::forward<F>(operation)).detach();
  }

  std::pmr::memory_resource* resource() const noexcept
  {
    return std::pmr::get_default_resource();
  }
};

void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

class allocation_resource final : public std::pmr::memory_resource
{
public:
  std::atomic<std::size_t> allocations{0};
private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override
  {
    ++allocations;
    return std::pmr::new_delete_resource()->allocate(bytes, alignment);
  }
  void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override
  {
    std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
  }
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
  {
    return this == &other;
  }
};

struct counted_inline_executor
{
  std::pmr::memory_resource* allocator;
  auto resource() const noexcept { return allocator; }
  template<class F> void execute(F&& function) { function(); }
};

void test_small_submit_allocations()
{
  allocation_resource resource;
  modern::scheduler executor{std::make_shared<counted_inline_executor>(&resource)};
  auto submitted = modern::submit(executor, &resource, [] { return 21; });
  require(resource.allocations == 1, "small submit allocated beyond its shared state");
  auto result = std::move(submitted).then([](int value) { return value * 2; });
  require(result.get() == 42, "inline continuation result changed");
  // Three shared states plus the composition frame. Inline completion makes
  // this independent of queue growth and await-registration races.
#if MODERN_RUNTIME_INLINE_CALLABLE_BYTES >= 48
  require(resource.allocations == 4, "small continuation allocated callable storage");
#endif
}

modern::task<int> coroutine_pipeline(modern::thread_pool& pool)
{
  auto value = co_await modern::submit(pool, [] { return 20; })
    .then([](int input) { return input + 1; });
  co_return value * 2;
}

modern::task<int> nested_coroutine(modern::thread_pool& pool)
{
  co_return co_await coroutine_pipeline(pool);
}

modern::task<void> race_waiter(modern::scheduler executor, std::atomic<int>& completed)
{
  co_await modern::submit(executor, [] {});
  completed.fetch_add(1, std::memory_order_relaxed);
}

void test_composition(modern::thread_pool& pool)
{
  require(nested_coroutine(pool).get() == 42, "mixed coroutine pipeline failed");

  auto flattened = modern::submit(pool, [] { return 21; })
    .then([&pool](int value)
    {
      return modern::submit(pool, [value] { return value * 2; });
    });
  static_assert(std::same_as<decltype(flattened), modern::task<int>>);
  require(flattened.get() == 42, "then did not flatten task result");

  auto recovered = modern::submit(pool, []() -> int
    {
      throw std::runtime_error("failure");
    })
    .catching([&pool](std::exception_ptr)
    {
      return modern::submit(pool, [] { return 42; });
    });
  require(recovered.get() == 42, "async catching did not flatten");

  bool cleanup = false;
  auto finalized = modern::submit(pool, [] { return 42; })
    .finally([&pool, &cleanup]() -> modern::task<void>
    {
      co_await modern::submit(pool, [&cleanup] { cleanup = true; });
    });
  require(finalized.get() == 42 && cleanup, "async finally failed");

  bool preserved = false;
  try
  {
    modern::submit(pool, []() -> int { throw std::logic_error("original"); })
      .finally([] {})
      .get();
  }
  catch (const std::logic_error& error)
  {
    preserved = std::string{error.what()} == "original";
  }
  require(preserved, "finally did not preserve the original exception");
}

void test_suspended_handler_lifetimes()
{
  for (int kind = 0; kind < 6; ++kind)
  {
    modern::sync::manual_reset_event started, release;
    modern::thread_pool pool{1};
    auto handler = [&, owned = std::make_unique<int>(42)](auto...) -> modern::task<int>
    {
      co_await modern::submit(pool, [&] { started.set(); release.wait(); });
      co_return *owned;
    };
    modern::task<int> result;
    if (kind == 0)
      result = modern::submit(pool, [] { return 1; }).then(std::move(handler));
    else if (kind == 1)
      result = modern::submit(pool, [] {}).then(std::move(handler));
    else if (kind == 2)
      result = modern::submit(pool, []() -> int { throw std::runtime_error("parent"); })
        .catching(std::move(handler));
    else if (kind == 3)
      result = modern::submit(pool, [] { return 42; })
        .finally([&, owned = std::make_unique<int>(42)]() -> modern::task<void>
        {
          co_await modern::submit(pool, [&] { started.set(); release.wait(); });
          require(*owned == 42, "async finally lost its closure");
        });
    else if (kind == 4)
      result = modern::submit(pool, [] { return std::make_unique<int>(42); })
        .then([&](const std::unique_ptr<int>& value) -> modern::task<int>
        {
          co_await modern::submit(pool, [&] { started.set(); release.wait(); });
          co_return *value;
        });
    else
      result = modern::submit(pool, []() -> int { throw std::runtime_error("parent"); })
        .catching([&](const std::exception_ptr& error) -> modern::task<int>
        {
          co_await modern::submit(pool, [&] { started.set(); release.wait(); });
          bool caught = false;
          try { std::rethrow_exception(error); }
          catch (const std::runtime_error&) { caught = true; }
          co_return caught ? 42 : 0;
        });
    // On one worker, the queued handler invocation has returned and its queue
    // callable has been destroyed before this blocked child can start.
    started.wait();
    release.set();
    require(result.get() == 42, "async composition lost its closure");
    pool.shutdown();
    pool.join();
  }
}

void test_environment(modern::thread_pool& pool)
{
  std::stop_source stop;
  modern::trace::TraceContext trace;
  trace.trace_id[0] = std::byte{0x42};

  modern::task_environment environment;
  environment.scheduler = pool.get_scheduler();
  environment.frame_resource = pool.resource();
  environment.trace_context = trace;
  environment.stop_token = stop.get_token();
  environment.execution_deadline = modern::deadline::after(5s);

  modern::task_environment_scope scope{environment};
  auto inspect = [&]() -> modern::task<bool>
  {
    auto current = co_await modern::this_task::environment();
    auto scheduler = co_await modern::this_task::scheduler();
    auto resource = co_await modern::this_task::memory_resource();
    auto inherited_trace = co_await modern::this_task::trace_context();
    auto token = co_await modern::this_task::stop_token();
    auto inherited_deadline = co_await modern::this_task::deadline();
    co_return scheduler.valid()
      && resource == pool.resource()
      && inherited_trace == trace
      && token.stop_possible()
      && inherited_deadline == environment.execution_deadline;
  };

  require(inspect().get(), "task environment was not inherited");
}

void test_deadline_merge_and_async_scope(modern::thread_pool& pool)
{
  auto later = modern::deadline::after(10s);
  auto sooner = modern::deadline::after(1s);
  modern::task_environment merged;
  merged.execution_deadline = later;
  modern::task_environment source;
  source.execution_deadline = sooner;
  modern::merge_environment(merged, source);
  require(merged.execution_deadline == sooner, "earlier child deadline did not win");

  std::stop_source parent_stop;
  modern::task_environment environment;
  environment.scheduler = pool.get_scheduler();
  environment.frame_resource = pool.resource();
  environment.stop_token = parent_stop.get_token();
  environment.execution_deadline = sooner;

  modern::task_environment_scope environment_scope{environment};
  modern::task_scope scope{pool.get_scheduler(), pool.resource()};
  std::atomic<bool> propagated = false;
  modern::sync::manual_reset_event resume_child;
  scope.spawn_async([&]() -> modern::task<void>
  {
    const auto token = co_await modern::this_task::stop_token();
    const auto child_deadline = co_await modern::this_task::deadline();
    co_await modern::submit(pool, [&] { resume_child.wait(); });
    propagated.store(
      token.stop_possible() && child_deadline == sooner,
      std::memory_order_release);
  });
  // The temporary coroutine closure has left its full expression before resume.
  resume_child.set();
  scope.join();
  require(propagated.load(std::memory_order_acquire),
    "async scope did not propagate its task environment");

  parent_stop.request_stop();
  require(scope.stop_requested(), "parent stop was not propagated into task scope");
}

void test_cancellation(modern::thread_pool& pool)
{
  std::stop_source source;
  source.request_stop();
  auto cancelled = modern::submit(pool, source.get_token(), [] { return 1; });

  bool observed = false;
  try
  {
    (void)cancelled.get();
  }
  catch (const modern::operation_cancelled&)
  {
    observed = true;
  }
  require(observed, "stopped task did not throw operation_cancelled");
}

void test_deadline_cancellation_controller(modern::thread_pool& pool)
{
  modern::scheduled_executor timers{pool};

  modern::deadline_cancellation_controller timed{
    timers, {}, modern::deadline::after(5ms)};
  for (int attempt = 0; attempt != 100 && !timed.token().stop_requested(); ++attempt)
    std::this_thread::sleep_for(1ms);
  require(timed.token().stop_requested(), "deadline did not request cancellation");
  require(timed.outcome() == modern::cancellation_outcome::deadline_expired,
    "deadline controller reported the wrong outcome");
  require(!timed.complete(), "completion won after the deadline had already completed");

  std::stop_source parent;
  modern::deadline_cancellation_controller cancelled{
    timers, parent.get_token(), modern::deadline::after(1s)};
  parent.request_stop();
  require(cancelled.token().stop_requested(), "parent stop did not request cancellation");
  require(cancelled.outcome() == modern::cancellation_outcome::parent_cancelled,
    "parent cancellation reported the wrong outcome");

  modern::deadline_cancellation_controller completed{
    timers, {}, modern::deadline::after(1s)};
  require(completed.complete(), "completion did not win a pending controller");
  require(!completed.token().stop_requested(), "normal completion requested stop");
  require(completed.outcome() == modern::cancellation_outcome::completed,
    "completion outcome was not retained");

  timers.shutdown();
  timers.join();
}

void test_move_only_and_races(modern::thread_pool& pool)
{
  auto pointer = modern::submit(pool, []
    {
      return std::make_unique<int>(42);
    })
    .then([](std::unique_ptr<int> value)
    {
      return *value;
    });
  require(pointer.get() == 42, "move-only result was not propagated");

  auto returned_pointer = modern::submit(pool, [] { return 42; })
    .then([](int value) { return std::make_unique<int>(value); })
    .get();
  require(returned_pointer && *returned_pointer == 42,
    "move-only continuation result was copied or lost");

  auto executor = modern::scheduler{std::make_shared<async_test_executor>()};
  constexpr int iterations = 64;
  std::atomic<int> completed = 0;
  std::vector<modern::task<void>> waiters;
  waiters.reserve(iterations);
  for (int i = 0; i < iterations; ++i)
    waiters.push_back(race_waiter(executor, completed));

  for (auto& waiter : waiters)
    waiter.get();

  require(completed.load(std::memory_order_relaxed) == iterations,
    "completion/await registration race lost a resume");
}
} // namespace

int main()
{
  test_small_submit_allocations();
  test_suspended_handler_lifetimes();
  modern::thread_pool pool{4};
  test_composition(pool);
  test_environment(pool);
  test_deadline_merge_and_async_scope(pool);
  test_cancellation(pool);
  test_deadline_cancellation_controller(pool);
  test_move_only_and_races(pool);
  pool.shutdown();
  pool.join();
}
