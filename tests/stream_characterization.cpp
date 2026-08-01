#include <atomic>
#include <chrono>
#include <coroutine>
#include <expected>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

import modern.runtime;

using namespace std::chrono_literals;

namespace
{
void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

void test_contract_traits()
{
  static_assert(std::is_move_constructible_v<modern::stream<int>>);
  static_assert(!std::is_copy_constructible_v<modern::stream<int>>);
  static_assert(std::is_move_constructible_v<modern::stream_source<int>>);
  static_assert(!std::is_copy_constructible_v<modern::stream_source<int>>);

  bool rejected = false;
  try
  {
    auto invalid = modern::make_stream<int>(0,
      [](modern::stream_source<int>) -> modern::task<void>
      {
        co_return;
      });
    (void)invalid;
  }
  catch (const std::invalid_argument&)
  {
    rejected = true;
  }
  require(rejected, "capacity zero was not rejected");
}

void test_close_drains_and_backpressures()
{
  std::atomic<bool> second_send_waited = false;
  auto values = modern::make_stream<int>(1,
    [&second_send_waited](modern::stream_source<int> source) -> modern::task<void>
    {
      auto first = co_await source.send(1);
      if (!first)
        co_return;

      auto second = source.send(2);
      second_send_waited.store(!second.ready(), std::memory_order_release);
      auto sent = co_await std::move(second);
      if (!sent)
        co_return;
      source.close();
    });

  auto first = values.next().get();
  require(first && first->has_value() && **first == 1, "first stream value was lost");
  require(second_send_waited.load(std::memory_order_acquire), "bounded stream did not backpressure");

  auto second = values.next().get();
  require(second && second->has_value() && **second == 2, "close did not drain buffered value");

  auto end = values.next().get();
  require(end && !end->has_value(), "closed stream did not return EOF");
}

void test_fail_is_immediate()
{
  auto cause = std::make_exception_ptr(std::runtime_error("producer boom"));
  auto values = modern::make_stream<int>(2,
    [cause](modern::stream_source<int> source) -> modern::task<void>
    {
      auto sent = co_await source.send(1);
      if (!sent)
        co_return;
      source.fail(modern::stream_error{
        modern::stream_error_code::producer_failed,
        "producer boom",
        cause});
    });

  auto failed = values.next().get();
  require(!failed, "failed stream delivered a buffered value");
  require(failed.error().code == modern::stream_error_code::producer_failed,
    "failed stream returned wrong error code");
  require(failed.error().cause == cause, "failed stream lost its exception cause");
}

void test_request_stop_is_shared()
{
  std::atomic<bool> producer_cancelled = false;
  auto values = modern::make_stream<int>(1,
    [&producer_cancelled](modern::stream_source<int> source) -> modern::task<void>
    {
      auto first = co_await source.send(1);
      if (!first)
        co_return;
      auto second = co_await source.send(2);
      producer_cancelled.store(
        !second && second.error().code == modern::stream_error_code::cancelled,
        std::memory_order_release);
    });

  values.request_stop();
  auto stopped = values.next().get();
  require(!stopped && stopped.error().code == modern::stream_error_code::cancelled,
    "request_stop did not cancel the consumer");
  require(producer_cancelled.load(std::memory_order_acquire),
    "request_stop did not cancel the producer");
}

void test_cancel_with_pointer_convertible_value()
{
  auto values = modern::make_stream<void*>(1,
    [](modern::stream_source<void*> source) -> modern::task<void>
    {
      (void)co_await source.send(nullptr);
      (void)co_await source.send(nullptr);
    });

  values.request_stop();
  auto stopped = values.next().get();
  require(!stopped && stopped.error().code == modern::stream_error_code::cancelled,
    "cancellation injected an allocator pointer as a stream value");
}

void test_consumer_destruction_cancels_writer()
{
  std::atomic<bool> consumer_gone = false;
  {
    auto values = modern::make_stream<int>(1,
      [&consumer_gone](modern::stream_source<int> source) -> modern::task<void>
      {
        auto first = co_await source.send(1);
        if (!first)
          co_return;
        auto second = co_await source.send(2);
        consumer_gone.store(
          !second && second.error().code == modern::stream_error_code::consumer_gone,
          std::memory_order_release);
      });
  }
  require(consumer_gone.load(std::memory_order_acquire),
    "consumer destruction did not release a blocked producer");
}

void test_completion_observer()
{
  std::atomic<int> observations = 0;
  modern::stream_completion observed;
  auto values = modern::make_stream<int>(2,
    [](modern::stream_source<int> source) -> modern::task<void>
    {
      (void)co_await source.send(1);
      (void)co_await source.send(2);
    });

  values.observe_completion([&](const modern::stream_completion& completion)
  {
    observed = completion;
    observations.fetch_add(1, std::memory_order_relaxed);
  });

  require(values.next().get().value().value() == 1, "observer test lost first value");
  require(values.next().get().value().value() == 2, "observer test lost second value");
  require(!values.next().get().value().has_value(), "observer test did not close");
  require(observations.load(std::memory_order_relaxed) == 1,
    "completion observer was not called exactly once");
  require(observed.state == modern::stream_terminal_state::closed,
    "completion observer saw wrong state");
  require(observed.produced_items == 2 && observed.consumed_items == 2,
    "completion observer saw wrong item counts");

  values.observe_completion([&](const modern::stream_completion&)
  {
    observations.fetch_add(1, std::memory_order_relaxed);
  });
  require(observations.load(std::memory_order_relaxed) == 2,
    "late completion observer was not called");
}

void test_producer_exception_is_typed()
{
  auto values = modern::make_stream<int>(1,
    [](modern::stream_source<int>) -> modern::task<void>
    {
      throw std::runtime_error("automatic producer failure");
      co_return;
    });

  auto failed = values.next().get();
  require(!failed && failed.error().code == modern::stream_error_code::producer_failed,
    "producer exception was not mapped to a typed stream error");
  require(failed.error().message == "automatic producer failure",
    "producer exception message was not retained");
  require(static_cast<bool>(failed.error().cause),
    "producer exception_ptr was not retained");
}

void test_move_only_values()
{
  auto values = modern::make_stream<std::unique_ptr<int>>(1,
    [](modern::stream_source<std::unique_ptr<int>> source) -> modern::task<void>
    {
      (void)co_await source.send(std::make_unique<int>(42));
    });

  auto value = values.next().get();
  require(value && value->has_value() && ***value == 42,
    "move-only stream value was copied or lost");
  require(!values.next().get().value().has_value(),
    "move-only stream did not close");
}

void test_overlapping_operations_are_rejected()
{
  std::atomic<bool> release = false;
  modern::thread_pool pool{1};
  auto values = modern::make_stream<int>(1,
    [&pool, &release](modern::stream_source<int> source) -> modern::task<void>
    {
      co_await modern::submit(pool, [&release]
      {
        while (!release.load(std::memory_order_acquire))
          std::this_thread::yield();
      });
      (void)co_await source.send(1);
    });

  auto first_read = values.next();
  auto second_read = values.next().get();
  require(!second_read
      && second_read.error().code == modern::stream_error_code::protocol_error,
    "overlapping next() was not rejected");
  release.store(true, std::memory_order_release);
  auto first = first_read.get();
  require(first && first->has_value() && **first == 1,
    "original pending next() was disturbed by protocol rejection");
  pool.shutdown();
  pool.join();
}

void test_completion_waits_for_producer_quiescence()
{
  modern::thread_pool pool{1};
  std::atomic<bool> release = false;
  std::atomic<int> observations = 0;
  auto values = modern::make_stream<int>(1,
    [&pool, &release](modern::stream_source<int> source) -> modern::task<void>
    {
      source.close();
      co_await modern::submit(pool, [&release]
      {
        while (!release.load(std::memory_order_acquire))
          std::this_thread::yield();
      });
    });

  values.observe_completion([&](const modern::stream_completion&)
  {
    observations.fetch_add(1, std::memory_order_release);
  });
  require(!values.next().get().value().has_value(),
    "explicit close did not expose EOF");
  require(observations.load(std::memory_order_acquire) == 0,
    "completion observer ran before producer quiescence");

  release.store(true, std::memory_order_release);
  for (int attempt = 0;
       attempt != 100 && observations.load(std::memory_order_acquire) == 0;
       ++attempt)
    std::this_thread::sleep_for(1ms);
  require(observations.load(std::memory_order_acquire) == 1,
    "completion observer did not run after producer quiescence");
  pool.shutdown();
  pool.join();
}
} // namespace

int main()
{
  test_contract_traits();
  test_close_drains_and_backpressures();
  test_fail_is_immediate();
  test_request_stop_is_shared();
  test_cancel_with_pointer_convertible_value();
  test_consumer_destruction_cancels_writer();
  test_completion_observer();
  test_producer_exception_is_typed();
  test_move_only_values();
  test_overlapping_operations_are_rejected();
  test_completion_waits_for_producer_quiescence();
}
