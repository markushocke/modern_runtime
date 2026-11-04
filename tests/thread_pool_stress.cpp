import modern.runtime;

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <latch>
#include <memory_resource>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
using counts = std::vector<std::atomic<unsigned>>;
void exactly_once(const counts& seen) {
  for (const auto& n : seen) require(n.load() == 1, "lost or duplicated task");
}

void producers(std::size_t workers, std::size_t n) {
  counts seen(n);
  modern::thread_pool pool{workers, nullptr, n};
  std::latch go{1};
  std::vector<std::jthread> writers;
  for (std::size_t p = 0; p < 8; ++p)
    writers.emplace_back([&, p] {
      go.wait();
      for (std::size_t i = p; i < n; i += 8)
        pool.execute(static_cast<modern::scheduler_priority>(i % 3),
                     [&, i] { ++seen[i]; });
    });
  go.count_down();
  writers.clear();
  pool.shutdown();
  pool.join();
  exactly_once(seen);
}

// A worker produces tasks into its own queue while other workers steal.
// Exercises contention and verifies that retaining a permit across scans
// never loses tasks or allows a premature shutdown.
void local_fanout(std::size_t workers, std::size_t n) {
  counts seen(n);
  modern::thread_pool pool{workers, nullptr, n + workers + 128};
  std::latch published{1};
  pool.execute([&] {
    for (std::size_t i = 0; i < n; ++i)
      pool.execute(static_cast<modern::scheduler_priority>(i % 3),
                   [&, i] { ++seen[i]; });
    published.count_down();
  });
  published.wait();
  pool.shutdown();
  pool.join();
  exactly_once(seen);
}

void shutdown_race(std::size_t workers, std::size_t n) {
  // Each round has guaranteed accepted work plus racing and rejected work.
  for (std::size_t base = 0; base < n; base += 1000) {
    const auto count = std::min<std::size_t>(1000, n - base);
    counts seen(count * 2);
    std::vector<unsigned char> accepted(count * 2, 0);
    modern::thread_pool pool{workers, nullptr, count * 2};
    const auto scheduler = pool.get_scheduler();
    for (std::size_t i = 0; i < count; ++i) {
      scheduler.execute([&, i] { ++seen[i]; });
      accepted[i] = 1;
    }
    std::latch go{1};
    std::vector<std::jthread> writers;
    for (std::size_t p = 0; p < 4; ++p)
      writers.emplace_back([&, p] {
        go.wait();
        for (std::size_t i = count + p; i < count * 2; i += 4) {
          try {
            scheduler.execute([&, i] { ++seen[i]; });
            accepted[i] = 1;
          } catch (const std::runtime_error&) {}
        }
      });
    std::jthread stopper([&] { go.wait(); pool.shutdown(); });
    go.count_down();
    pool.shutdown(); // Concurrent, idempotent shutdown.
    stopper.join();
    writers.clear();
    pool.join();
    for (std::size_t i = 0; i < seen.size(); ++i)
      require(seen[i].load() == accepted[i], "shutdown acceptance mismatch");
    bool rejected = false;
    try { scheduler.execute([] {}); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "submission after shutdown accepted");
  }
}

void queue_limit(std::size_t workers, std::size_t n) {
  counts seen(n);
  modern::thread_pool pool{workers, nullptr, n};
  std::latch started(static_cast<std::ptrdiff_t>(workers)), release{1};
  for (std::size_t i = 0; i < workers; ++i)
    pool.execute([&] { started.count_down(); release.wait(); });
  started.wait();
  for (std::size_t i = 0; i < n; ++i) pool.execute([&, i] { ++seen[i]; });
  std::size_t rejected = 0;
  for (std::size_t i = 0; i < n; ++i) {
    try { pool.execute([] {}); }
    catch (const std::runtime_error&) { ++rejected; }
  }
  pool.shutdown();
  release.count_down();
  pool.join();
  require(rejected == n, "queue limit not enforced");
  exactly_once(seen);
}

void exceptions(std::size_t workers, std::size_t n) {
  counts seen(n);
  modern::thread_pool pool{workers, nullptr, n * 4};
  std::vector<modern::task<int>> tasks;
  tasks.reserve(n);
  for (std::size_t i = 0; i < n; ++i)
    tasks.push_back(modern::submit(pool, [&, i]() -> int {
      ++seen[i];
      if (i % 2 == 0) throw std::runtime_error("submit failure");
      return 1;
    }).then([](int) -> int { throw std::runtime_error("continuation failure"); }));
  std::size_t failed = 0;
  for (auto& task : tasks) {
    try { (void)task.get(); }
    catch (const std::runtime_error&) { ++failed; }
  }
  pool.shutdown();
  pool.join();
  require(failed == n, "exception propagation failed");
  exactly_once(seen);
}

void lifecycle(std::size_t workers, std::size_t n) {
  for (std::size_t base = 0; base < n; base += 1000) {
    const auto count = std::min<std::size_t>(1000, n - base);
    counts seen(count);
    modern::scheduler retained;
    {
      modern::thread_pool pool{workers, nullptr, count};
      retained = pool.get_scheduler();
      for (std::size_t i = 0; i < count; ++i)
        retained.execute([&, i] { ++seen[i]; });
    } // Destruction must join even while a scheduler keeps the state alive.
    exactly_once(seen);
    bool rejected = false;
    try { retained.execute([] {}); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "retained scheduler accepted work after destruction");
  }
}

void cancellation_race(std::size_t workers, std::size_t n) {
  counts seen(n);
  modern::thread_pool pool{workers, nullptr, n};
  std::vector<modern::task<int>> tasks;
  tasks.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::stop_source stop;
    tasks.push_back(modern::submit(pool, stop.get_token(), [&, i] {
      ++seen[i];
      return 42;
    }));
    stop.request_stop();
  }
  for (std::size_t i = 0; i < n; ++i) {
    bool cancelled = false;
    try { require(tasks[i].get() == 42, "incorrect cancellation race result"); }
    catch (const modern::operation_cancelled&) { cancelled = true; }
    require(seen[i].load() == (cancelled ? 0u : 1u), "cancellation/execution mismatch");
  }
  pool.shutdown();
  pool.join();
}

class failing_resource : public std::pmr::memory_resource {
public:
  std::atomic<bool> fail{false};
private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    if (fail.load()) throw std::bad_alloc();
    return std::pmr::new_delete_resource()->allocate(bytes, alignment);
  }
  void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
    std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
  }
  bool do_is_equal(const std::pmr::memory_resource& r) const noexcept override {
    return this == &r;
  }
};

void allocation_failure(std::size_t workers, std::size_t n) {
  failing_resource resource;
  counts seen(n);
  modern::thread_pool pool{workers, &resource, n + workers * 16};
  std::latch started(static_cast<std::ptrdiff_t>(workers)), release{1};
  for (std::size_t i = 0; i < workers; ++i)
    pool.execute([&] { started.count_down(); release.wait(); });
  started.wait();
  resource.fail = true;
  bool failed = false;
  for (std::size_t i = 0; i < workers * 16; ++i) {
    try { pool.execute([] {}); }
    catch (const std::bad_alloc&) { failed = true; break; }
  }
  resource.fail = false;
  for (std::size_t i = 0; i < n; ++i) pool.execute([&, i] { ++seen[i]; });
  pool.shutdown();
  release.count_down();
  pool.join();
  require(failed, "queue allocation failure not injected");
  exactly_once(seen);
}
}

int main(int argc, char** argv) {
  try {
    if (argc != 4) throw std::invalid_argument("usage: scenario workers tasks");
    const std::string scenario = argv[1];
    const auto workers = std::stoull(argv[2]), n = std::stoull(argv[3]);
    if (!workers || !n) throw std::invalid_argument("positive counts required");
    std::cerr << scenario << ": workers=" << workers << " tasks=" << n << std::endl;
    if (scenario == "producers") producers(workers, n);
    else if (scenario == "local_fanout") local_fanout(workers, n);
    else if (scenario == "shutdown_race") shutdown_race(workers, n);
    else if (scenario == "queue_limit") queue_limit(workers, n);
    else if (scenario == "exceptions") exceptions(workers, n);
    else if (scenario == "lifecycle") lifecycle(workers, n);
    else if (scenario == "cancellation_race") cancellation_race(workers, n);
    else if (scenario == "allocation_failure") allocation_failure(workers, n);
    else throw std::invalid_argument("unknown scenario");
    std::cout << "PASS\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return EXIT_FAILURE;
  }
}
