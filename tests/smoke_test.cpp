import modern.runtime;

#include <array>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <coroutine>
#include <exception>
#include <expected>
#include <iostream>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>

namespace
{
class counting_resource final : public modern::memory::memory_resource
{
public:
  explicit counting_resource(
    modern::memory::memory_resource* upstream = modern::memory::new_delete_resource()) noexcept
    : upstream_(upstream)
  {
  }

  [[nodiscard]] std::size_t allocations() const noexcept
  {
    return allocations_;
  }

private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override
  {
    ++allocations_;
    return upstream_->allocate(bytes, alignment);
  }

  void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override
  {
    upstream_->deallocate(ptr, bytes, alignment);
  }

  bool do_is_equal(const modern::memory::memory_resource& other) const noexcept override
  {
    return this == &other;
  }

  modern::memory::memory_resource* upstream_;
  std::size_t allocations_ = 0;
};

class mock_io_service final
{
public:
  explicit mock_io_service(
    modern::thread_pool& pool,
    modern::memory::memory_resource* resource,
    modern::io_backend_kind backend_kind = modern::io_backend_kind::unknown,
    std::size_t submission_queue_depth = 0,
    std::size_t completion_queue_depth = 0) noexcept
    : scheduler_(pool.get_scheduler()),
      resource_(resource),
      backend_info_{backend_kind, submission_queue_depth, completion_queue_depth}
  {
  }

  [[nodiscard]] modern::scheduler get_scheduler() const
  {
    return scheduler_;
  }

  [[nodiscard]] modern::memory::memory_resource* resource() const noexcept
  {
    return resource_;
  }

  [[nodiscard]] modern::io_backend_info backend_info() const noexcept
  {
    return backend_info_;
  }

  template<class Completion>
  void start(int value, Completion completion)
  {
    ++start_calls_;

    scheduler_.execute([completion = std::move(completion), value]() mutable
    {
      completion.set_value(value);
    });
  }

  template<class Completion>
  void start(int value, std::stop_token token, Completion completion)
  {
    ++start_calls_;

    scheduler_.execute([completion = std::move(completion), value, token]() mutable
    {
      if (token.stop_requested())
      {
        completion.cancel();
        return;
      }

      completion.set_value(value);
    });
  }

  [[nodiscard]] int start_calls() const noexcept
  {
    return start_calls_.load(std::memory_order_relaxed);
  }

private:
  modern::scheduler scheduler_;
  modern::memory::memory_resource* resource_;
  modern::io_backend_info backend_info_;
  std::atomic<int> start_calls_ = 0;
};

template<class Receiver>
class mock_value_operation
{
public:
  mock_value_operation(int value, Receiver receiver)
    : value_(value),
      receiver_(std::move(receiver))
  {
  }

  friend void start(mock_value_operation& operation)
  {
    set_value(std::move(operation.receiver_), operation.value_);
  }

private:
  int value_;
  Receiver receiver_;
};

class mock_value_sender
{
public:
  explicit mock_value_sender(int value) noexcept
    : value_(value)
  {
  }

  template<class Receiver>
  friend auto connect(mock_value_sender sender, Receiver receiver)
  {
    return mock_value_operation<Receiver>{sender.value_, std::move(receiver)};
  }

private:
  int value_;
};

template<class Receiver>
class mock_error_operation
{
public:
  explicit mock_error_operation(Receiver receiver)
    : receiver_(std::move(receiver))
  {
  }

  friend void start(mock_error_operation& operation)
  {
    set_error(std::move(operation.receiver_), std::make_exception_ptr(std::runtime_error("sender boom")));
  }

private:
  Receiver receiver_;
};

class mock_error_sender
{
public:
  template<class Receiver>
  friend auto connect(mock_error_sender, Receiver receiver)
  {
    return mock_error_operation<Receiver>{std::move(receiver)};
  }
};

template<class Receiver>
class mock_stopped_operation
{
public:
  explicit mock_stopped_operation(Receiver receiver)
    : receiver_(std::move(receiver))
  {
  }

  friend void start(mock_stopped_operation& operation)
  {
    set_stopped(std::move(operation.receiver_));
  }

private:
  Receiver receiver_;
};

class mock_stopped_sender
{
public:
  template<class Receiver>
  friend auto connect(mock_stopped_sender, Receiver receiver)
  {
    return mock_stopped_operation<Receiver>{std::move(receiver)};
  }
};
} // namespace

int main()
{
  using namespace std::chrono_literals;

  try
  {
    counting_resource task_resource;
    modern::thread_pool cpu{2, &task_resource};
    modern::scheduled_executor timers{cpu};

    if (task_resource.allocations() == 0)
    {
      std::cerr << "executor pmr allocation check failed\n";
      return 1;
    }

    auto task_allocations_before = task_resource.allocations();
    bool finished = false;
    auto value = modern::submit(cpu, []
      {
        return 21;
      })
      .then([](int x)
      {
        return x * 2;
      })
      .finally([&]
      {
        finished = true;
      })
      .get();

    if (value != 42 || !finished)
    {
      std::cerr << "task pipeline check failed\n";
      return 1;
    }

    if (task_resource.allocations() <= task_allocations_before)
    {
      std::cerr << "task pmr propagation check failed\n";
      return 1;
    }

    counting_resource continuation_resource;
    auto continuation_allocations_before = continuation_resource.allocations();
    bool injected_finally = false;

    auto injected_value = modern::submit(cpu, &task_resource, []() -> int
      {
        throw std::runtime_error("allocator injection");
      })
      .catching(&continuation_resource, [](std::exception_ptr)
      {
        return 5;
      })
      .then(&continuation_resource, [](int x)
      {
        return x + 1;
      })
      .finally(&continuation_resource, [&]
      {
        injected_finally = true;
      })
      .get();

    if (injected_value != 6
      || !injected_finally
      || continuation_resource.allocations() <= continuation_allocations_before)
    {
      std::cerr << "continuation allocator injection check failed\n";
      return 1;
    }

    bool recovered_exception = false;
    auto recovered = modern::submit(cpu, []() -> int
      {
        throw std::runtime_error("boom");
      })
      .catching([&](std::exception_ptr ep)
      {
        try
        {
          if (ep)
            std::rethrow_exception(ep);
        }
        catch (const std::runtime_error& e)
        {
          recovered_exception = std::string{e.what()} == "boom";
        }

        return 7;
      })
      .then([](int x)
      {
        return x + 1;
      })
      .get();

    if (recovered != 8 || !recovered_exception)
    {
      std::cerr << "exception recovery check failed\n";
      return 1;
    }

    mock_io_service io{cpu, &task_resource};
    auto modern_io = modern::adapt_modern_io(io);
    auto io_allocations_before = task_resource.allocations();
    bool io_bridge_finished = false;
    auto io_value = modern_io.submit<int>(41)
      .then([](int value)
      {
        return value + 1;
      })
      .finally([&]
      {
        io_bridge_finished = true;
      })
      .get();

    if (io_value != 42 || !io_bridge_finished || task_resource.allocations() <= io_allocations_before)
    {
      std::cerr << "io bridge check failed\n";
      return 1;
    }

    if (io.start_calls() == 0)
    {
      std::cerr << "modern_io adapter setup check failed\n";
      return 1;
    }

    std::stop_source io_cancel_source;
    io_cancel_source.request_stop();
    auto io_cancel_starts_before = io.start_calls();
    auto cancelled_io = modern_io.submit<int>(io_cancel_source.get_token(), 7);

    bool io_cancelled = false;

    try
    {
      (void)cancelled_io.get();
    }
    catch (const std::runtime_error& e)
    {
      io_cancelled = std::string{e.what()} == "task cancelled";
    }

    if (!io_cancelled || io.start_calls() != io_cancel_starts_before)
    {
      std::cerr << "modern_io adapter cancellation check failed\n";
      return 1;
    }

    mock_io_service uring_service{
      cpu,
      &task_resource,
      modern::io_backend_kind::io_uring,
      128,
      256};
    auto uring_allocations_before = task_resource.allocations();
    auto uring = modern::adapt_modern_io_uring(uring_service);
    auto uring_info = uring.backend_info();
    auto uring_value = uring.submit<int>(11).get();

    if (uring_info.kind != modern::io_backend_kind::io_uring
      || uring_info.submission_queue_depth != 128
      || uring_info.completion_queue_depth != 256
      || uring_value != 11
      || task_resource.allocations() <= uring_allocations_before)
    {
      std::cerr << "io_uring adapter check failed\n";
      return 1;
    }

    bool wrong_backend_rejected = false;

    try
    {
      mock_io_service wrong_backend{cpu, &task_resource, modern::io_backend_kind::epoll, 64, 64};
      (void)modern::adapt_modern_io_uring(wrong_backend);
    }
    catch (const std::runtime_error& e)
    {
      wrong_backend_rejected = std::string{e.what()} == "service is not io_uring-backed";
    }

    if (!wrong_backend_rejected)
    {
      std::cerr << "io_uring backend guard check failed\n";
      return 1;
    }

    mock_io_service epoll_service{
      cpu,
      &task_resource,
      modern::io_backend_kind::epoll,
      64,
      64};
    auto epoll_adapter = modern::adapt_modern_io_poller(epoll_service);
    auto epoll_info = epoll_adapter.backend_info();
    auto epoll_value = epoll_adapter.submit<int>(12).get();

    if (epoll_adapter.poller_kind() != modern::poller_backend_kind::epoll
      || epoll_info.kind != modern::io_backend_kind::epoll
      || epoll_value != 12)
    {
      std::cerr << "epoll abstraction check failed\n";
      return 1;
    }

    mock_io_service kqueue_service{
      cpu,
      &task_resource,
      modern::io_backend_kind::kqueue,
      32,
      32};
    auto kqueue_adapter = modern::adapt_modern_io_poller(kqueue_service);
    auto kqueue_value = kqueue_adapter.submit<int>(13).get();

    if (kqueue_adapter.poller_kind() != modern::poller_backend_kind::kqueue || kqueue_value != 13)
    {
      std::cerr << "kqueue abstraction check failed\n";
      return 1;
    }

    auto sender_allocations_before = task_resource.allocations();
    auto sender_value = modern::as_task<int>(mock_value_sender{41}, cpu.get_scheduler(), &task_resource)
      .then([](int value)
      {
        return value + 1;
      })
      .get();

    if (sender_value != 42 || task_resource.allocations() <= sender_allocations_before)
    {
      std::cerr << "sender bridge value check failed\n";
      return 1;
    }

    bool sender_error = false;

    try
    {
      (void)modern::as_task<int>(mock_error_sender{}, cpu.get_scheduler(), &task_resource).get();
    }
    catch (const std::runtime_error& e)
    {
      sender_error = std::string{e.what()} == "sender boom";
    }

    if (!sender_error)
    {
      std::cerr << "sender bridge error check failed\n";
      return 1;
    }

    bool sender_stopped = false;

    try
    {
      (void)modern::as_task<int>(mock_stopped_sender{}, cpu.get_scheduler(), &task_resource).get();
    }
    catch (const std::runtime_error& e)
    {
      sender_stopped = std::string{e.what()} == "task cancelled";
    }

    if (!sender_stopped)
    {
      std::cerr << "sender bridge stopped check failed\n";
      return 1;
    }

    modern::thread_pool fiber_pool{1, &task_resource, 16};
    modern::fiber_scheduler fibers{fiber_pool.get_scheduler(), &task_resource};
    modern::sync::manual_reset_event fibers_done;
    std::atomic<int> fibers_remaining = 2;
    std::string fiber_order;
    int fiber_a_steps = 0;
    int fiber_b_steps = 0;

    fibers.spawn([&](modern::fiber_context& context)
    {
      fiber_order.push_back('A');
      ++fiber_a_steps;

      if (fiber_a_steps < 3)
      {
        context.yield();
        return;
      }

      if (fibers_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
        fibers_done.set();
    });

    fibers.spawn([&](modern::fiber_context& context)
    {
      fiber_order.push_back('B');
      ++fiber_b_steps;

      if (fiber_b_steps < 2)
      {
        context.yield();
        return;
      }

      if (fibers_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
        fibers_done.set();
    });

    if (!fibers_done.wait_for(100ms) || fiber_order != "ABABA")
    {
      std::cerr << "fiber scheduler yield check failed\n";
      return 1;
    }

    fibers.join();
    fiber_pool.shutdown();
    fiber_pool.join();

    modern::thread_pool failing_fiber_pool{1, &task_resource, 16};
    modern::fiber_scheduler failing_fibers{failing_fiber_pool.get_scheduler(), &task_resource};
    modern::sync::manual_reset_event failing_fiber_started;
    std::atomic<bool> failing_fiber_cancelled = false;

    failing_fibers.spawn([&](modern::fiber_context& context)
    {
      failing_fiber_started.set();

      if (context.stop_requested())
      {
        failing_fiber_cancelled.store(true, std::memory_order_relaxed);
        return;
      }

      context.yield();
    });

    if (!failing_fiber_started.wait_for(50ms))
    {
      std::cerr << "fiber scheduler failure setup check failed\n";
      return 1;
    }

    failing_fibers.spawn([]
    {
      throw std::runtime_error("fiber boom");
    });

    bool fiber_failure = false;

    try
    {
      failing_fibers.join();
    }
    catch (const std::runtime_error& e)
    {
      fiber_failure = std::string{e.what()} == "fiber boom";
    }

    if (!fiber_failure || !failing_fiber_cancelled.load(std::memory_order_relaxed))
    {
      std::cerr << "fiber scheduler failure check failed\n";
      return 1;
    }

    failing_fiber_pool.shutdown();
    failing_fiber_pool.join();

    bool coroutine_resumed = false;
    auto coroutine_value = [&]() -> modern::task<int>
    {
      auto input = modern::submit(cpu, []
      {
        return 41;
      });

      auto noop = modern::submit(cpu, [] {});
      co_await std::move(noop);

      coroutine_resumed = true;
      co_return (co_await std::move(input)) + 1;
    }().get();

    if (coroutine_value != 42 || !coroutine_resumed)
    {
      std::cerr << "coroutine bridge check failed\n";
      return 1;
    }

    auto runtime_task_allocations_before = task_resource.allocations();

    {
      modern::runtime::FrameMemoryResourceScope frame_scope{&task_resource};
      auto runtime_task = []() -> modern::runtime::Task<int>
      {
        co_return 7;
      }();

      if (runtime_task.sync_wait() != 7 || task_resource.allocations() <= runtime_task_allocations_before)
      {
        std::cerr << "runtime task frame resource check failed\n";
        return 1;
      }
    }

    modern::runtime::TraceContext runtime_trace;
    runtime_trace.trace_id[0] = std::byte{0x4b};
    runtime_trace.span_id[0] = std::byte{0x2a};
    runtime_trace.flags = 1;

    auto read_runtime_trace = []() -> modern::runtime::Task<std::optional<modern::runtime::TraceContext>>
    {
      co_return co_await modern::runtime::current_trace_context();
    };

    auto read_child_runtime_trace = [&]() -> modern::runtime::Task<std::optional<modern::runtime::TraceContext>>
    {
      co_return co_await read_runtime_trace();
    };

    auto runtime_trace_task = read_child_runtime_trace();
    runtime_trace_task.set_trace_context(runtime_trace);
    auto inherited_runtime_trace = runtime_trace_task.sync_wait();

    if (!inherited_runtime_trace || inherited_runtime_trace != runtime_trace)
    {
      std::cerr << "runtime task trace context check failed\n";
      return 1;
    }

    auto runtime_result = []() -> modern::runtime::ResultTask<int, std::error_code>
    {
      co_return std::expected<int, std::error_code>{42};
    }().sync_wait();

    if (!runtime_result || *runtime_result != 42)
    {
      std::cerr << "runtime result task check failed\n";
      return 1;
    }

    auto timer_allocations_before = task_resource.allocations();
    auto delayed = modern::schedule_after(timers, 20ms, []
      {
        return std::string{"delayed result"};
      })
      .then([](std::string s)
      {
        return s + " + continuation";
      })
      .get();

    if (delayed != "delayed result + continuation")
    {
      std::cerr << "delayed scheduling check failed\n";
      return 1;
    }

    if (task_resource.allocations() <= timer_allocations_before)
    {
      std::cerr << "timer task pmr propagation check failed\n";
      return 1;
    }

    modern::sync::manual_reset_event event;
    auto signal = modern::submit(cpu, [&]() -> bool
      {
        modern::platform::sleep_for(5ms);
        event.set();
        return true;
      });

    if (!event.wait_for(50ms) || !signal.get() || !event.is_set())
    {
      std::cerr << "sync/platform integration check failed\n";
      return 1;
    }

    auto affinity_cpu = modern::platform::current_cpu();

    if (!affinity_cpu)
    {
      std::cerr << "affinity setup check failed\n";
      return 1;
    }

    std::array<std::size_t, 1> affinity_map{*affinity_cpu};
    modern::thread_pool pinned{1, &task_resource, 4, affinity_map};
    modern::sync::manual_reset_event affinity_done;
    std::size_t observed_cpu = static_cast<std::size_t>(-1);

    pinned.execute([&]
    {
      auto worker_cpu = modern::platform::current_cpu();

      if (worker_cpu)
        observed_cpu = *worker_cpu;

      affinity_done.set();
    });

    if (!affinity_done.wait_for(50ms) || observed_cpu != affinity_map.front())
    {
      std::cerr << "thread affinity check failed\n";
      return 1;
    }

    pinned.shutdown();
    pinned.join();

    modern::thread_pool prioritized{1, &task_resource, 4};
    modern::sync::manual_reset_event priority_started;
    modern::sync::manual_reset_event priority_release;
    modern::sync::manual_reset_event priority_done;
    std::string priority_order;

    prioritized.execute([&]
    {
      priority_started.set();
      priority_release.wait();
    });

    if (!priority_started.wait_for(50ms))
    {
      std::cerr << "priority setup check failed\n";
      return 1;
    }

    auto prioritized_scheduler = prioritized.get_scheduler();

    prioritized_scheduler.execute(modern::scheduler_priority::low, [&]
    {
      priority_order.push_back('L');
    });

    prioritized_scheduler.execute(modern::scheduler_priority::high, [&]
    {
      priority_order.push_back('H');
      priority_done.set();
    });

    priority_release.set();

    if (!priority_done.wait_for(50ms) || priority_order != "HL")
    {
      std::cerr << "scheduler priority check failed\n";
      return 1;
    }

    prioritized.shutdown();
    prioritized.join();

    event.reset();
    if (event.is_set())
    {
      std::cerr << "event reset check failed\n";
      return 1;
    }

    modern::sync::manual_reset_event first_started;
    modern::sync::manual_reset_event release_first;
    modern::sync::manual_reset_event second_done;
    modern::thread_pool bounded{1, &task_resource, 1};

    bounded.execute([&]
    {
      first_started.set();
      release_first.wait();
    });

    if (!first_started.wait_for(50ms))
    {
      std::cerr << "bounded queue setup check failed\n";
      return 1;
    }

    bounded.execute([&]
    {
      second_done.set();
    });

    bool queue_full = false;

    try
    {
      bounded.execute([] {});
    }
    catch (const std::runtime_error& e)
    {
      queue_full = std::string{e.what()} == "thread_pool queue is full";
    }

    release_first.set();

    if (!queue_full || !second_done.wait_for(50ms))
    {
      std::cerr << "bounded queue check failed\n";
      return 1;
    }

    bounded.shutdown();
    bounded.join();

    modern::thread_pool stealing{2, &task_resource, 8};
    modern::sync::manual_reset_event children_done;
    modern::sync::manual_reset_event steal_done;
    std::atomic<int> children_remaining = 4;
    std::atomic<bool> steal_observed = false;
    std::atomic<bool> steal_completed = false;

    stealing.execute([&]
    {
      auto owner = std::this_thread::get_id();

      for (int i = 0; i < 4; ++i)
      {
        stealing.execute([&, owner]
        {
          if (std::this_thread::get_id() != owner)
            steal_observed.store(true, std::memory_order_relaxed);

          if (children_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
            children_done.set();
        });
      }

      if (children_done.wait_for(100ms))
        steal_completed.store(true, std::memory_order_relaxed);

      steal_done.set();
    });

    if (!steal_done.wait_for(150ms) ||
        !steal_completed.load(std::memory_order_relaxed) ||
        !steal_observed.load(std::memory_order_relaxed))
    {
      std::cerr << "work stealing check failed\n";
      return 1;
    }

    stealing.shutdown();
    stealing.join();

    modern::thread_pool cancellable{1, &task_resource, 2};
    modern::sync::manual_reset_event cancel_blocker_started;
    modern::sync::manual_reset_event cancel_blocker_release;

    cancellable.execute([&]
    {
      cancel_blocker_started.set();
      cancel_blocker_release.wait();
    });

    if (!cancel_blocker_started.wait_for(50ms))
    {
      std::cerr << "cancellation setup check failed\n";
      return 1;
    }

    std::stop_source submit_cancel_source;
    bool cancelled_task_ran = false;
    auto cancelled_task = modern::submit(cancellable, submit_cancel_source.get_token(), [&]() -> int
    {
      cancelled_task_ran = true;
      return 7;
    });

    submit_cancel_source.request_stop();
    cancel_blocker_release.set();

    bool submit_cancelled = false;

    try
    {
      (void)cancelled_task.get();
    }
    catch (const std::runtime_error& e)
    {
      submit_cancelled = std::string{e.what()} == "task cancelled";
    }

    if (!submit_cancelled || cancelled_task_ran)
    {
      std::cerr << "submit cancellation check failed\n";
      return 1;
    }

    cancellable.shutdown();
    cancellable.join();

    std::stop_source timer_cancel_source;
    bool cancelled_timer_ran = false;
    auto cancelled_timer = modern::schedule_after(timers, 100ms, timer_cancel_source.get_token(), [&]() -> int
    {
      cancelled_timer_ran = true;
      return 1;
    });

    timer_cancel_source.request_stop();

    bool timer_cancelled = false;

    try
    {
      (void)cancelled_timer.get();
    }
    catch (const std::runtime_error& e)
    {
      timer_cancelled = std::string{e.what()} == "task cancelled";
    }

    if (!timer_cancelled || cancelled_timer_ran)
    {
      std::cerr << "timer cancellation check failed\n";
      return 1;
    }

    modern::task_scope scope{cpu.get_scheduler(), &task_resource};
    modern::sync::manual_reset_event scope_child_started;
    std::atomic<bool> scope_cancelled = false;

    scope.spawn([&](std::stop_token token)
    {
      scope_child_started.set();

      while (!token.stop_requested())
        modern::platform::sleep_for(1ms);

      scope_cancelled.store(true, std::memory_order_relaxed);
    });

    if (!scope_child_started.wait_for(50ms))
    {
      std::cerr << "structured concurrency setup check failed\n";
      return 1;
    }

    scope.spawn([]
    {
      throw std::runtime_error("scope boom");
    });

    bool scope_failed = false;

    try
    {
      scope.join();
    }
    catch (const std::runtime_error& e)
    {
      scope_failed = std::string{e.what()} == "scope boom";
    }

    if (!scope_failed || !scope_cancelled.load(std::memory_order_relaxed))
    {
      std::cerr << "structured concurrency check failed\n";
      return 1;
    }

    auto before = modern::platform::now();
    modern::platform::yield();
    auto after = modern::platform::now();
    if (after < before || modern::platform::hardware_concurrency() == 0)
    {
      std::cerr << "platform surface check failed\n";
      return 1;
    }

    int ticks = 0;
    auto periodic_allocations_before = task_resource.allocations();
    auto periodic = timers.schedule_fixed_rate(0ms, 10ms, [&]
      {
        ++ticks;
      });

    modern::platform::sleep_for(45ms);
    periodic.request_stop();
    modern::platform::sleep_for(15ms);

    if (ticks < 2)
    {
      std::cerr << "periodic scheduling check failed\n";
      return 1;
    }

    if (task_resource.allocations() <= periodic_allocations_before)
    {
      std::cerr << "periodic pmr propagation check failed\n";
      return 1;
    }

    std::byte buffer[256];
    modern::memory::monotonic_buffer_resource resource{buffer, sizeof(buffer)};
    modern::memory::polymorphic_allocator<int> allocator{&resource};
    auto* values = allocator.allocate(4);
    for (int i = 0; i < 4; ++i)
      values[i] = i;
    allocator.deallocate(values, 4);

    modern::memory::arena arena{buffer, sizeof(buffer)};
    modern::memory::polymorphic_allocator<int> arena_allocator{arena.resource()};
    auto* arena_values = arena_allocator.allocate(4);
    arena_values[0] = 1;
    arena.release();

    modern::memory::pool pool;
    modern::memory::polymorphic_allocator<int> pool_allocator{pool.resource()};
    auto* pooled_values = pool_allocator.allocate(4);
    pool_allocator.deallocate(pooled_values, 4);
    pool.release();

    if (modern::memory::get_default_resource() == nullptr ||
        modern::memory::new_delete_resource() == nullptr)
    {
      std::cerr << "pmr surface check failed\n";
      return 1;
    }

    timers.shutdown();
    timers.join();
    cpu.shutdown();
    cpu.join();

    return 0;
  }
  catch (const std::exception& e)
  {
    std::cerr << e.what() << "\n";
    return 1;
  }
}