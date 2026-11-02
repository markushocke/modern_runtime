import modern.runtime;

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct options
{
  std::string scenario = "all";
  std::size_t iterations = 10000;
};

options parse(int argc, char** argv)
{
  options result;
  for (int i = 1; i < argc; ++i)
  {
    const std::string flag{argv[i]};
    if (++i >= argc)
      throw std::invalid_argument("missing value for " + flag);
    const std::string value{argv[i]};

    if (flag == "--case")
    {
      if (value != "all" && value != "producers" && value != "shutdown" &&
          value != "cancellation")
        throw std::invalid_argument("unknown stress scenario: " + value);
      result.scenario = value;
    }
    else if (flag == "--iterations")
    {
      std::size_t position = 0;
      result.iterations = std::stoull(value, &position);
      if (position != value.size() || result.iterations == 0 ||
          result.iterations > 10000000)
        throw std::invalid_argument("iterations must be in [1,10000000]");
    }
    else
      throw std::invalid_argument("unknown option: " + flag);
  }
  return result;
}

void checkpoint(const char* scenario, const char* step)
{
  // CTest shows this output when a test times out (even with --output-on-failure).
  // Flush immediately: the next operation may block indefinitely.
  std::cerr << "[runtime_stress:" << scenario << "] " << step << std::endl;
}

void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

void test_concurrent_producers(std::size_t iterations)
{
  constexpr std::size_t producers = 4;
  checkpoint("producers", "start: creating thread pool");
  modern::thread_pool pool{4, modern::memory::get_default_resource(), iterations + 64};
  std::atomic<std::size_t> observed{0};
  std::atomic<std::size_t> submitted{0};
  std::latch finished(static_cast<std::ptrdiff_t>(iterations));
  std::vector<std::jthread> writers;
  checkpoint("producers", "starting four producer threads");
  for (std::size_t producer = 0; producer < producers; ++producer)
  {
    writers.emplace_back([&, producer]
    {
      for (std::size_t i = producer; i < iterations; i += producers)
      {
        pool.execute([&]
        {
          observed.fetch_add(1, std::memory_order_relaxed);
          finished.count_down();
        });
        submitted.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  checkpoint("producers", "joining producers");
  for (auto& writer : writers)
    writer.join();
  checkpoint("producers", "producers joined; awaiting task completions");
  require(submitted.load() == iterations, "incorrect number of submissions");
  finished.wait();
  checkpoint("producers", "task completions reached; shutting down pool");
  pool.shutdown();
  pool.join();
  checkpoint("producers", "pool joined");
  require(observed.load() == iterations, "lost or duplicated tasks");
}

void test_shutdown_drains()
{
  checkpoint("shutdown", "start: submitting 1000 tasks");
  modern::thread_pool pool{4, modern::memory::get_default_resource(), 2048};
  std::atomic<int> observed{0};
  for (int i = 0; i < 1000; ++i)
    pool.execute([&] { observed.fetch_add(1, std::memory_order_relaxed); });
  checkpoint("shutdown", "submitted; calling shutdown");
  pool.shutdown();
  checkpoint("shutdown", "calling join");
  pool.join();
  checkpoint("shutdown", "pool joined");
  require(observed.load() == 1000, "shutdown lost accepted tasks");
}

void test_cancellation(std::size_t iterations)
{
  checkpoint("cancellation", "start: creating thread pool");
  modern::thread_pool pool{4};
  checkpoint("cancellation", "checking pre-cancelled submits");
  for (std::size_t i = 0; i < iterations; ++i)
  {
    std::stop_source source;
    source.request_stop();
    auto task = modern::submit(pool, source.get_token(), [] { return 1; });
    bool cancelled = false;
    try { (void)task.get(); }
    catch (const modern::operation_cancelled&) { cancelled = true; }
    require(cancelled, "pre-cancelled task executed");
  }
  checkpoint("cancellation", "checks passed; shutting down pool");
  pool.shutdown();
  pool.join();
  checkpoint("cancellation", "pool joined");
}
}

int main(int argc, char** argv)
{
  try
  {
    const auto settings = parse(argc, argv);
    if (settings.scenario == "all" || settings.scenario == "producers")
      test_concurrent_producers(settings.iterations);
    if (settings.scenario == "all" || settings.scenario == "shutdown")
      test_shutdown_drains();
    if (settings.scenario == "all" || settings.scenario == "cancellation")
      test_cancellation(settings.iterations);
    std::cout << "PASS: " << settings.scenario << '\n';
    return EXIT_SUCCESS;
  }
  catch (const std::exception& error)
  {
    std::cerr << "runtime_stress: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
