import modern.runtime;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <latch>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using clock_type = std::chrono::steady_clock;
using nanoseconds = std::chrono::nanoseconds;

class counting_resource final : public std::pmr::memory_resource
{
public:
  struct counters
  {
    std::uint64_t allocations;
    std::uint64_t bytes;
  };

  counters snapshot() const noexcept
  {
    return {allocations_.load(std::memory_order_relaxed),
            bytes_.load(std::memory_order_relaxed)};
  }

private:
  void* do_allocate(std::size_t size, std::size_t alignment) override
  {
    auto* pointer = std::pmr::new_delete_resource()->allocate(size, alignment);
    allocations_.fetch_add(1, std::memory_order_relaxed);
    bytes_.fetch_add(size, std::memory_order_relaxed);
    return pointer;
  }

  void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override
  {
    std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
  {
    return this == &other;
  }

  std::atomic<std::uint64_t> allocations_{0};
  std::atomic<std::uint64_t> bytes_{0};
};

std::size_t parse_positive(const char* value, const char* flag)
{
  std::string input{value};
  std::size_t position = 0;
  std::size_t result = 0;
  try
  {
    result = std::stoull(input, &position);
  }
  catch (const std::exception&)
  {
    throw std::invalid_argument(std::string{flag} + " requires a positive integer");
  }
  if (position != input.size() || result == 0 || result > 1000000)
    throw std::invalid_argument(std::string{flag} + " must be in [1,1000000]");
  return result;
}

struct options
{
  std::size_t threads = 4;
  std::size_t operations = 10000;
  std::size_t timer_samples = 128;
};

options parse(int argc, char** argv)
{
  options result;
  for (int i = 1; i < argc; ++i)
  {
    if (i + 1 >= argc)
      throw std::invalid_argument(std::string{"missing value for "} + argv[i]);
    std::string key{argv[i++]};
    auto value = parse_positive(argv[i], key.c_str());
    if (key == "--threads")
      result.threads = value;
    else if (key == "--operations")
      result.operations = value;
    else if (key == "--timer-samples")
      result.timer_samples = value;
    else
      throw std::invalid_argument("unknown option: " + key);
  }
  if (result.threads > 256)
    throw std::invalid_argument("thread count must be <= 256");
  return result;
}

std::int64_t elapsed_ns(clock_type::time_point start, clock_type::time_point end)
{
  return std::chrono::duration_cast<nanoseconds>(end - start).count();
}

double percentile(const std::vector<std::int64_t>& sorted, double fraction)
{
  if (sorted.empty())
    return 0.0;
  const auto rank = static_cast<std::size_t>(std::ceil(fraction * sorted.size()));
  return static_cast<double>(sorted[std::max<std::size_t>(1, rank) - 1]);
}

void report(const std::string& name, std::size_t threads,
            std::vector<std::int64_t> samples,
            clock_type::time_point start, clock_type::time_point end,
            counting_resource::counters before, counting_resource::counters after)
{
  std::sort(samples.begin(), samples.end());
  const auto total = samples.size();
  const double seconds = std::chrono::duration<double>(end - start).count();
  std::cout << name << ',' << threads << ',' << total << ','
            << std::fixed << std::setprecision(3)
            << seconds * 1000.0 << ',' << total / seconds << ','
            << percentile(samples, 0.50) << ','
            << percentile(samples, 0.95) << ','
            << percentile(samples, 0.99) << ','
            << percentile(samples, 0.999) << ','
            << static_cast<double>(after.allocations - before.allocations) / total << ','
            << static_cast<double>(after.bytes - before.bytes) / total << '\n';
}

void measure_dispatch(modern::thread_pool& pool, counting_resource& resource,
                      std::size_t threads, std::size_t count)
{
  std::vector<std::int64_t> latencies(count);
  std::latch completed(static_cast<std::ptrdiff_t>(count));
  const auto before = resource.snapshot();
  const auto started = clock_type::now();

  for (std::size_t i = 0; i < count; ++i)
  {
    auto submitted = clock_type::now();
    pool.execute([&, i, submitted]
    {
      latencies[i] = elapsed_ns(submitted, clock_type::now());
      completed.count_down();
    });
  }

  completed.wait();
  const auto ended = clock_type::now();
  report("pool_dispatch", threads, std::move(latencies), started, ended,
         before, resource.snapshot());
}

void measure_task_roundtrip(modern::thread_pool& pool, counting_resource& resource,
                            std::size_t threads, std::size_t count)
{
  std::vector<std::int64_t> latencies;
  latencies.reserve(count);
  const auto before = resource.snapshot();
  const auto started = clock_type::now();

  // One task at a time: submit + one continuation + get. Not a saturation test.
  for (std::size_t i = 0; i < count; ++i)
  {
    const auto submitted = clock_type::now();
    const auto result = modern::submit(pool, &resource, [] { return 21; })
      .then([](int value) { return value * 2; }).get();
    if (result != 42)
      throw std::runtime_error("task continuation produced incorrect result");
    latencies.push_back(elapsed_ns(submitted, clock_type::now()));
  }

  const auto ended = clock_type::now();
  report("task_roundtrip", threads, std::move(latencies), started, ended,
         before, resource.snapshot());
}

void measure_timer(modern::scheduled_executor& timers, counting_resource& resource,
                   std::size_t threads, std::size_t count)
{
  std::vector<std::int64_t> lateness;
  lateness.reserve(count);
  const auto before = resource.snapshot();
  const auto started = clock_type::now();

  // Sequential timers avoid confusing a timer-jitter measurement with queue overload.
  for (std::size_t i = 0; i < count; ++i)
  {
    const auto due = clock_type::now() + std::chrono::milliseconds(2);
    const auto late = modern::schedule_at(timers, due, [due]
    {
      return elapsed_ns(due, clock_type::now());
    }).get();
    lateness.push_back(late);
  }

  const auto ended = clock_type::now();
  report("timer_lateness", threads, std::move(lateness), started, ended,
         before, resource.snapshot());
}
}

int main(int argc, char** argv)
{
  try
  {
    const auto settings = parse(argc, argv);
    // +64 avoids intentional queue-limit rejection in the throughput workload.
    counting_resource resource;
    modern::thread_pool pool{settings.threads, &resource, settings.operations + 64};
    modern::scheduled_executor timers{pool, &resource};
    std::cout << "workload,threads,operations,elapsed_ms,ops_per_second,"
                 "p50_ns,p95_ns,p99_ns,p999_ns,pmr_allocs_per_op,pmr_bytes_per_op\n";

    measure_dispatch(pool, resource, settings.threads, settings.operations);
    measure_task_roundtrip(pool, resource, settings.threads, settings.operations);
    measure_timer(timers, resource, settings.threads, settings.timer_samples);

    timers.shutdown();
    timers.join();
    pool.shutdown();
    pool.join();
    return EXIT_SUCCESS;
  }
  catch (const std::exception& error)
  {
    std::cerr << "runtime_benchmark: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
