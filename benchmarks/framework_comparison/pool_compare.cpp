// Identical task body and measurement mechanics; only submit() differs.
#if defined(COMPARE_MODERN) || defined(COMPARE_MODERN_INJECTION) || defined(COMPARE_MODERN_CHASE_LEV) || defined(COMPARE_MODERN_PARK)
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
  bool profile_only = false;
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
  for (int i = 1; i < argc;) {
    const std::string key = argv[i++];
    if (key == "--profile-only") {
      result.profile_only = true;
      continue;
    }
    if (i >= argc) throw std::invalid_argument("expected --key value pairs");
    const std::string value = argv[i++];
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
      result.workload != "pool_single_flight" &&
      result.workload != "pool_worker_fanout" &&
      result.workload != "pool_mixed_compute" &&
      result.workload != "pool_staged_pipeline")
    throw std::invalid_argument("unsupported workload");
  return result;
}

#if defined(COMPARE_MODERN) || defined(COMPARE_MODERN_INJECTION) || defined(COMPARE_MODERN_CHASE_LEV) || defined(COMPARE_MODERN_PARK)
#if defined(COMPARE_MODERN_PARK)
constexpr char framework[] = "modern_chase_lev_park";
#elif defined(COMPARE_MODERN_CHASE_LEV)
constexpr char framework[] = "modern_chase_lev";
#elif defined(COMPARE_MODERN_INJECTION)
constexpr char framework[] = "modern_injection";
#else
constexpr char framework[] = "modern_runtime";
#endif
class scheduler {
 public:
  scheduler(std::size_t workers, std::size_t ops)
#if defined(COMPARE_MODERN_PARK)
    : pool_(modern::thread_pool_options{
        .workers = workers,
        .resource = nullptr,
        .max_pending_tasks = ops + workers + 128,
        .backend = modern::queue_backend::injection_chase_lev,
        .wakeup = modern::worker_wakeup::idle_parking
      }) {}
#elif defined(COMPARE_MODERN_CHASE_LEV)
    : pool_(modern::thread_pool_options{
        .workers = workers,
        .resource = nullptr,
        .max_pending_tasks = ops + workers + 128,
        .backend = modern::queue_backend::injection_chase_lev
      }) {}
#elif defined(COMPARE_MODERN_INJECTION)
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

// Mixed CPU work approximates a batch of cheap event handlers plus occasional
// expensive parse/transform steps. All adapters execute exactly the same
// deterministic integer workload, with no external library cost.
std::uint64_t compute_digest(std::size_t index) noexcept {
  std::uint64_t value = static_cast<std::uint64_t>(index) ^ marker;
  const auto iterations = (index % 16 == 0) ? 2048u : 64u;
  for (unsigned step = 0; step < iterations; ++step) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value += 0x9e3779b97f4a7c15ULL + step;
  }
  return value ^ marker;
}

// Three dependent CPU stages emulate an in-memory decode/transform/pack job.
// They deliberately exercise nested task submission and end-to-end completion.
std::uint64_t stage_decode(std::size_t index) noexcept {
  auto value = static_cast<std::uint64_t>(index) + marker;
  value ^= value >> 30;
  return value * 0xbf58476d1ce4e5b9ULL;
}

std::uint64_t stage_transform(std::uint64_t value) noexcept {
  value ^= value >> 27;
  return value * 0x94d049bb133111ebULL;
}

std::uint64_t stage_pack(std::uint64_t value) noexcept {
  return (value ^ (value >> 31)) ^ marker;
}

std::uint64_t pipeline_result(std::size_t index) noexcept {
  return stage_pack(stage_transform(stage_decode(index)));
}

std::uint64_t nanos(clock_type::time_point a, clock_type::time_point b) {
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

void emit(const options& cfg, std::vector<std::uint64_t> latency,
          const std::vector<std::uint64_t>& seen,
          clock_type::time_point began, clock_type::time_point ended) {
  for (std::size_t i = 0; i < seen.size(); ++i) {
    const auto expected = cfg.workload == "pool_staged_pipeline"
      ? pipeline_result(i)
      : cfg.workload == "pool_mixed_compute"
        ? compute_digest(i) : (static_cast<std::uint64_t>(i) ^ marker);
    if (seen[i] != expected)
      throw std::runtime_error("missing or incorrect task result: " + std::to_string(i));
  }
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
      seen[i] = cfg.workload == "pool_mixed_compute"
        ? compute_digest(i) : (static_cast<std::uint64_t>(i) ^ marker);
      done.count_down();
    });
  };

  // Models a recursive build/asset-processing job: an external request starts
  // one worker, then that worker creates the actual independent sub-jobs.
  // It deliberately exercises worker-local enqueue and work stealing.
  if (cfg.workload == "pool_worker_fanout") {
    std::latch fanout_done(static_cast<std::ptrdiff_t>(cfg.operations + 1));
    const auto began = clock_type::now();
    checked_submit(executor, [&] {
      for (std::size_t i = 0; i < cfg.operations; ++i) {
        const auto submitted = clock_type::now();
        checked_submit(executor, [&, i, submitted] {
          latency[i] = nanos(submitted, clock_type::now());
          seen[i] = static_cast<std::uint64_t>(i) ^ marker;
          fanout_done.count_down();
        });
      }
      fanout_done.count_down(); // Root published all child tasks.
    });
    fanout_done.wait();
    const auto ended = clock_type::now();
    // The normal benchmark validates/sorts as before. Profiling omits
    // post-run validation and sort to avoid contaminating CPU samples.
    if (!cfg.profile_only)
      emit(cfg, std::move(latency), seen, began, ended);
    return;
  }

  if (cfg.workload == "pool_staged_pipeline") {
    // Identical three-stage task chain for every framework. Each task
    // submits its dependent successor only after writing its stage output.
    // The number of pending tasks is bounded by the number of active items.
    std::vector<std::uint64_t> decoded(cfg.operations);
    std::vector<std::uint64_t> transformed(cfg.operations);
    const auto began = clock_type::now();
    for (std::size_t i = 0; i < cfg.operations; ++i) {
      const auto submitted = clock_type::now();
      checked_submit(executor, [&, i, submitted] {
        decoded[i] = stage_decode(i);
        checked_submit(executor, [&, i, submitted] {
          transformed[i] = stage_transform(decoded[i]);
          checked_submit(executor, [&, i, submitted] {
            seen[i] = stage_pack(transformed[i]);
            // For this scenario the latency is end-to-end from initial
            // submission to *final stage completion*, not start delay.
            latency[i] = nanos(submitted, clock_type::now());
            done.count_down();
          });
        });
      });
    }
    done.wait();
    const auto ended = clock_type::now();
    // The normal benchmark validates/sorts as before. Profiling omits
    // post-run validation and sort to avoid contaminating CPU samples.
    if (!cfg.profile_only)
      emit(cfg, std::move(latency), seen, began, ended);
    return;
  }

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
    // The normal benchmark validates/sorts as before. Profiling omits
    // post-run validation and sort to avoid contaminating CPU samples.
    if (!cfg.profile_only)
      emit(cfg, std::move(latency), seen, began, ended);
    return;
  }

  if (cfg.workload == "pool_dispatch" ||
      cfg.workload == "pool_mixed_compute") {
    const auto began = clock_type::now();
    for (std::size_t i = 0; i < cfg.operations; ++i) submit_at(i);
    done.wait();
    const auto ended = clock_type::now();
    // The normal benchmark validates/sorts as before. Profiling omits
    // post-run validation and sort to avoid contaminating CPU samples.
    if (!cfg.profile_only)
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
  // The normal benchmark validates/sorts as before. Profiling omits
  // post-run validation and sort to avoid contaminating CPU samples.
  if (!cfg.profile_only)
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
