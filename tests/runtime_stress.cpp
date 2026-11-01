import modern.runtime;

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::size_t parse(int argc, char** argv)
{
  if (argc == 1)
    return 10000;
  if (argc != 3 || std::string{argv[1]} != "--iterations")
    throw std::invalid_argument("usage: runtime_stress [--iterations N]");
  const std::string value{argv[2]};
  std::size_t pos = 0;
  auto count = std::stoull(value, &pos);
  if (pos != value.size() || count == 0 || count > 10000000)
    throw std::invalid_argument("iterations must be in [1,10000000]");
  return count;
}

void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

void test_concurrent_producers(std::size_t iterations)
{
  constexpr std::size_t producers = 4;
  modern::thread_pool pool{4, modern::memory::get_default_resource(), iterations + 64};
  std::atomic<std::size_t> observed{0};
  std::latch finished(static_cast<std::ptrdiff_t>(iterations));
  std::vector<std::jthread> writers;
  for (std::size_t producer = 0; producer < producers; ++producer)
  {
    writers.emplace_back([&, producer]
    {
      for (std::size_t i = producer; i < iterations; i += producers)
        pool.execute([&]
        {
          observed.fetch_add(1, std::memory_order_relaxed);
          finished.count_down();
        });
    });
  }
  for (auto& writer : writers)
    writer.join();
  finished.wait();
  pool.shutdown();
  pool.join();
  require(observed.load() == iterations, "lost or duplicated tasks");
}

void test_shutdown_drains()
{
  modern::thread_pool pool{4, modern::memory::get_default_resource(), 2048};
  std::atomic<int> observed{0};
  for (int i = 0; i < 1000; ++i)
    pool.execute([&] { observed.fetch_add(1, std::memory_order_relaxed); });
  pool.shutdown();
  pool.join();
  require(observed.load() == 1000, "shutdown lost accepted tasks");
}

void test_cancellation(std::size_t iterations)
{
  modern::thread_pool pool{4};
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
  pool.shutdown();
  pool.join();
}
}

int main(int argc, char** argv)
{
  try
  {
    const auto iterations = parse(argc, argv);
    test_concurrent_producers(iterations);
    test_shutdown_drains();
    test_cancellation(iterations);
    std::cout << "PASS: " << iterations << " multi-producer tasks, "
              << iterations << " cancellation cases, shutdown drain\n";
    return EXIT_SUCCESS;
  }
  catch (const std::exception& error)
  {
    std::cerr << "runtime_stress: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
