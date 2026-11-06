#include "../modules/detail/worker_parking.hpp"

#include <atomic>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory_resource>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// Concurrent producers publish into an atomic task counter. Each consumer
// performs the same register -> recheck -> sleep handshake as the pool.
// The stress intentionally includes periods with no work and publications
// during the registration window.
void exercise(std::size_t workers, std::size_t producers, std::size_t count) {
  modern::detail::worker_parking parking(workers, std::pmr::get_default_resource());
  std::atomic<std::size_t> ready{0};
  std::atomic<std::size_t> done{0};
  std::atomic<bool> stop{false};

  std::vector<std::jthread> consumers;
  for (std::size_t index = 0; index < workers; ++index) {
    consumers.emplace_back([&, index] {
      for (;;) {
        std::size_t pending = ready.load(std::memory_order_acquire);
        if (pending > 0 &&
            ready.compare_exchange_weak(pending, pending - 1,
              std::memory_order_acq_rel, std::memory_order_relaxed)) {
          done.fetch_add(1, std::memory_order_release);
          done.notify_one();
          continue;
        }
        if (stop.load(std::memory_order_acquire) &&
            ready.load(std::memory_order_acquire) == 0)
          return;
        const auto epoch = parking.prepare(index);
        if (ready.load(std::memory_order_seq_cst) != 0 ||
            stop.load(std::memory_order_acquire))
          parking.cancel(index);
        else
          parking.wait(index, epoch);
      }
    });
  }

  std::vector<std::jthread> writers;
  for (std::size_t p = 0; p < producers; ++p) {
    writers.emplace_back([&, p] {
      for (std::size_t n = p; n < count; n += producers) {
        ready.fetch_add(1, std::memory_order_seq_cst);
        parking.notify_one();
        if ((n & 31u) == 0)
          std::this_thread::yield();
      }
    });
  }
  writers.clear();

  while (done.load(std::memory_order_acquire) != count) {
    const auto previous = done.load(std::memory_order_acquire);
    if (previous != count)
      done.wait(previous, std::memory_order_acquire);
  }
  require(ready.load(std::memory_order_acquire) == 0,
          "worker parking dropped a published task");

  stop.store(true, std::memory_order_release);
  parking.notify_all();
  consumers.clear();
  require(parking.parked_count() == 0, "worker parking leaked waiter registration");
}
}

int main() {
  try {
    for (auto workers : {std::size_t{1}, std::size_t{2},
                         std::size_t{4}, std::size_t{16}}) {
      for (auto producers : {std::size_t{1}, std::size_t{4}}) {
        exercise(workers, producers, 25000);
      }
    }
    std::cout << "PASS: idle worker parking under concurrent producer/wakeup races\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
