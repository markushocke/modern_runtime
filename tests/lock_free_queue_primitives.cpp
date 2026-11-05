#include "../modules/detail/lock_free_queues.hpp"

#include <atomic>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char* error) {
  if (!ok)
    throw std::runtime_error(error);
}

void mpmc(std::size_t capacity, std::size_t count) {
  modern::detail::bounded_mpmc_queue<std::size_t*> queue(capacity);
  std::vector<std::size_t> values(count);
  auto seen = std::make_unique<std::atomic<unsigned>[]>(count);
  std::atomic<std::size_t> completed{0};

  std::vector<std::jthread> consumers;
  for (std::size_t i = 0; i < 4; ++i) {
    consumers.emplace_back([&] {
      while (completed.load(std::memory_order_acquire) < count) {
        if (auto* value = queue.try_pop()) {
          seen[value - values.data()].fetch_add(1, std::memory_order_relaxed);
          completed.fetch_add(1, std::memory_order_release);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  std::vector<std::jthread> producers;
  for (std::size_t p = 0; p < 4; ++p) {
    producers.emplace_back([&, p] {
      for (std::size_t i = p; i < count; i += 4)
        while (!queue.try_push(values.data() + i))
          std::this_thread::yield();
    });
  }
  producers.clear();
  consumers.clear();

  require(!queue.try_pop(), "MPMC ring not empty");
  for (std::size_t i = 0; i < count; ++i)
    require(seen[i].load(std::memory_order_relaxed) == 1,
            "MPMC lost/duplicated task");
}

void chase_lev(std::size_t capacity, std::size_t count) {
  modern::detail::bounded_chase_lev_deque<std::size_t*> queue(capacity);
  std::vector<std::size_t> values(count);
  auto seen = std::make_unique<std::atomic<unsigned>[]>(count);
  std::atomic<std::size_t> completed{0};
  std::atomic<bool> finished_publishing{false};
  auto take = [&](std::size_t* value) {
    seen[value - values.data()].fetch_add(1, std::memory_order_relaxed);
    completed.fetch_add(1, std::memory_order_release);
  };

  std::vector<std::jthread> thieves;
  for (std::size_t c = 0; c < 4; ++c) {
    thieves.emplace_back([&] {
      while (!finished_publishing.load(std::memory_order_acquire) ||
             completed.load(std::memory_order_acquire) < count) {
        if (auto* value = queue.steal())
          take(value);
        else
          std::this_thread::yield();
      }
    });
  }

  // Only this thread may push/pop the bottom.
  for (std::size_t i = 0; i < count; ++i) {
    while (!queue.try_push_owner(values.data() + i)) {
      if (auto* value = queue.pop_owner())
        take(value);
      else
        std::this_thread::yield();
    }
    if ((i & 1) == 0)
      if (auto* value = queue.pop_owner())
        take(value);
  }
  finished_publishing.store(true, std::memory_order_release);
  while (completed.load(std::memory_order_acquire) < count) {
    if (auto* value = queue.pop_owner())
      take(value);
    else
      std::this_thread::yield();
  }
  thieves.clear();

  require(!queue.pop_owner(), "Chase-Lev ring not empty");
  for (std::size_t i = 0; i < count; ++i)
    require(seen[i].load(std::memory_order_relaxed) == 1,
            "Chase-Lev lost/duplicated task");
}
}

int main() {
  try {
    for (auto cap : {std::size_t{2}, std::size_t{32}, std::size_t{256}}) {
      mpmc(cap, 50000);
      chase_lev(cap, 50000);
    }
    std::cout << "PASS: bounded MPMC injection and Chase-Lev deques\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
