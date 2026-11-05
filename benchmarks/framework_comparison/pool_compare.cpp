// Identical task body and measurement mechanics; only submit() differs.
#if defined(COMPARE_MODERN) || defined(COMPARE_MODERN_INJECTION)
import modern.runtime;
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <latch>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(COMPARE_TASKFLOW)
#include <taskflow/taskflow.hpp>
#elif defined(COMPARE_ONETBB)
#include <oneapi/tbb/task_arena.h>
#elif defined(COMPARE_STDEXEC)
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>
#endif

namespace {
using clock_type = std::chrono::steady_clock;
constexpr std::uint64_t marker = 0x9e3779b97f4a7c15ULL;

struct options {
  std::size_t workers = 4;
  std::size_t producers = 4;
  std::size_t operations = 100000;
  std::string workload = "pool_multi_producer";
};

std::size_t positive(const char* text) {
  std::string input{text};
  std::size_t consumed = 0;
  const auto value = std::stoull(input, &consumed);
  if (consumed != input.size() || value == 0 || value > 1000000)
    throw std::invalid_argument("expected integer in [1,1000000]");
  return value;
}

options parse(int argc, char** argv) {
  options result;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 >= argc) throw std::invalid_argument("expected --key value pairs");
    const std::string key = argv[i];
    const std::string value = argv[i + 1];
    if (key == "--workers") result.workers = positive(value.c_str());
    else if (key == "--producers") result.producers = positive(value.c_str());
    else if (key == "--operations") result.operations = positive(value.c_str());
    else if (key == "--workload") result.workload = value;
    else throw std::invalid_argument("unknown option: " + key);
  }
  if (result.workers > 256 || result.producers > 256)
    throw std::invalid_argument("workers and producers must be <= 256");
  if (result.workload != "pool_dispatch" &&
      result.workload != "pool_multi_producer" &&
      result.workload != "pool_single_flight")
    throw std::invalid_argument("unsupported workload");
  return result;
}

#if defined(COMPARE_MODERN) || defined(COMPARE_MODERN_INJECTION)
#if defined(COMPARE_MODERN_INJECTION)
constexpr char framework[] = "modern_injection";
#else
constexpr char framework[] = "modern_runtime";
#endif
class scheduler {
 public:
  scheduler(std::size_t workers, std::size_t ops)
#if defined(COMPARE_MODERN_INJECTION)
    : pool_(workers, nullptr, ops + workers + 128, {},
            modern::queue_backend::injection_mutex_local) {}
#else
    : pool_(workers, nullptr, ops + workers + 128) {}
#endif
  template<class F> void submit(F&& f) { pool_.execute(std::forward<F>(f)); }
 private:
  modern::thread_pool pool_;
};
#elif defined(COMPARE_TASKFLOW)
constexpr char framework[] = "taskflow";
class scheduler {
 public:
  scheduler(std::size_t workers, std::size_t) : executor_(workers) {}
  template<class F> void submit(F&& f) { executor_.silent_async(std::forward<F>(f)); }
 private:
  tf::Executor executor_;
};
#elif defined(COMPARE_ONETBB)
constexpr char framework[] = "onetbb";
class scheduler {
 public:
  scheduler(std::size_t workers, std::size_t) : arena_(static_cast<int>(workers)) {
    arena_.initialize();
  }
  template<class F> void submit(F&& f) { arena_.enqueue(std::forward<F>(f)); }
 private:
  tbb::task_arena arena_;
};
#elif defined(COMPARE_STDEXEC)
constexpr char framework[] = "stdexec";
class scheduler {
 public:
  scheduler(std::size_t workers, std::size_t) : pool_(workers) {}
  template<class F> void submit(F&& f) {
    exec::start_detached(
      stdexec::then(stdexec::schedule(pool_.get_scheduler()), std::forward<F>(f)));
  }
 private:
  exec::static_thread_pool pool_;
};
#else
#error "Select exactly one COMPARE_* backend"
#endif

template<class F> void checked_submit(scheduler& executor, F&& f) {
  try {
    executor.submit(std::forward<F>(f));
  } catch (const std::exception& error) {
    std::cerr << "async submission error: " << error.what() << '\n';
    // Outstanding tasks reference the benchmark stack. Do not unwind it.
    std::abort();
  } catch (...) {
    std::abort();
  }
}

std::uint64_t nanos(clock_type::time_point a, clock_type::time_point b) {
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

void emit(const options& cfg, std::vector<std::uint64_t> latency,
          const std::vector<std::uint64_t>& seen,
          clock_type::time_point began, clock_type::time_point ended) {
  for (std::size_t i = 0; i < seen.size(); ++i)
    if (seen[i] != (static_cast<std::uint64_t>(i) ^ marker))
      throw std::runtime_error("missing or incorrect task result: " + std::to_string(i));
  std::sort(latency.begin(), latency.end());
  const auto pct = [&](double q) {
    const auto rank = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::ceil(q * latency.size())));
    return latency[rank - 1];
  };
  const double seconds = std::chrono::duration<double>(ended - began).count();
  std::cout << "framework,workload,workers,producers,operations,elapsed_ms,"
               "ops_per_second,p50_ns,p95_ns,p99_ns,p999_ns,verified\n";
  std::cout << framework << ',' << cfg.workload << ',' << cfg.workers << ','
            << cfg.producers << ',' << cfg.operations << ','
            << std::fixed << std::setprecision(3) << seconds * 1000.0 << ','
            << std::setprecision(3) << cfg.operations / seconds << ','
            << pct(.50) << ',' << pct(.95) << ',' << pct(.99) << ','
            << pct(.999) << ",1\n";
}

void warmup(scheduler& executor) {
  std::latch done{256};
  for (int i = 0; i < 256; ++i)
    checked_submit(executor, [&done] { done.count_down(); });
  done.wait();
}

void measure(const options& cfg, scheduler& executor) {
  std::vector<std::uint64_t> latency(cfg.operations);
  std::vector<std::uint64_t> seen(cfg.operations, 0);
  std::latch done(static_cast<std::ptrdiff_t>(cfg.operations));

  const auto submit_at = [&](std::size_t i) {
    const auto submitted = clock_type::now();
    checked_submit(executor, [&, i, submitted] {
      latency[i] = nanos(submitted, clock_type::now());
      seen[i] = static_cast<std::uint64_t>(i) ^ marker;
      done.count_down();
    });
  };

  if (cfg.workload == "pool_single_flight") {
    std::binary_semaphore flight{0};
    const auto began = clock_type::now();
    for (std::size_t i = 0; i < cfg.operations; ++i) {
      const auto submitted = clock_type::now();
      checked_submit(executor, [&, i, submitted] {
        latency[i] = nanos(submitted, clock_type::now());
        seen[i] = static_cast<std::uint64_t>(i) ^ marker;
        done.count_down();
        flight.release();
      });
      flight.acquire();
    }
    done.wait();
    const auto ended = clock_type::now();
    emit(cfg, std::move(latency), seen, began, ended);
    return;
  }

  if (cfg.workload == "pool_dispatch") {
    const auto began = clock_type::now();
    for (std::size_t i = 0; i < cfg.operations; ++i) submit_at(i);
    done.wait();
    const auto ended = clock_type::now();
    emit(cfg, std::move(latency), seen, began, ended);
    return;
  }

  std::latch ready(static_cast<std::ptrdiff_t>(cfg.producers));
  std::latch start{1};
  std::vector<std::thread> writers;
  writers.reserve(cfg.producers);
  for (std::size_t p = 0; p < cfg.producers; ++p) {
    writers.emplace_back([&, p] {
      ready.count_down();
      start.wait();
      for (std::size_t i = p; i < cfg.operations; i += cfg.producers)
        submit_at(i);
    });
  }
  ready.wait();
  const auto began = clock_type::now();
  start.count_down();
  for (auto& writer : writers) writer.join();
  done.wait();
  const auto ended = clock_type::now();
  emit(cfg, std::move(latency), seen, began, ended);
}
}

int main(int argc, char** argv) {
  try {
    const auto cfg = parse(argc, argv);
    scheduler executor(cfg.workers, cfg.operations);
    warmup(executor);
    measure(cfg, executor);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "benchmark error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
