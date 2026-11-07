#include "../modules/detail/recycling_node_pool.hpp"

#include <atomic>
#include <cstddef>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}

class counting_resource final : public std::pmr::memory_resource {
public:
  std::atomic<std::size_t> allocations{0};
  std::atomic<std::size_t> deallocations{0};
  std::atomic<bool> fail{false};
private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    if (fail.load(std::memory_order_relaxed)) throw std::bad_alloc();
    auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
    allocations.fetch_add(1, std::memory_order_relaxed);
    return result;
  }
  void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
    deallocations.fetch_add(1, std::memory_order_relaxed);
    std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
  }
  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }
};

struct move_task {
  std::unique_ptr<unsigned> payload;
  move_task() noexcept = default;
  explicit move_task(unsigned value) : payload(std::make_unique<unsigned>(value)) {}
  move_task(move_task&&) noexcept = default;
  move_task& operator=(move_task&&) noexcept = default;
  move_task(const move_task&) = delete;
  move_task& operator=(const move_task&) = delete;
};

void reuse_and_capacity() {
  using pool_type = modern::detail::recycling_node_pool<move_task>;
  counting_resource resource;
  {
    pool_type pool(&resource, 2, 2);
    auto* first = pool.acquire(move_task{42});
    require(first->callable.payload && *first->callable.payload == 42, "wrong initial task");
    pool.recycle(first);
    for (unsigned i = 0; i < 10000; ++i) {
      auto* item = pool.acquire(move_task{i});
      require(item == first, "central freelist did not reuse memory");
      require(item->callable.payload && *item->callable.payload == i, "stale task payload");
      pool.recycle(item);
      require(!item->callable.payload, "recycled task retained captured state");
    }
    // At most two central nodes retained: excess nodes are actually freed.
    auto* a = pool.acquire(move_task{1});
    auto* b = pool.acquire(move_task{2});
    auto* c = pool.acquire(move_task{3});
    auto* d = pool.acquire(move_task{4});
    pool.recycle(a);
    pool.recycle(b);
    pool.recycle(c);
    pool.recycle(d);
    require(resource.deallocations.load() >= 2, "central retention limit ignored");
  }
  require(resource.allocations.load() == resource.deallocations.load(),
          "PMR allocation leak after pool destruction");
}

void concurrent_local_and_global() {
  using pool_type = modern::detail::recycling_node_pool<move_task>;
  counting_resource resource;
  constexpr std::size_t workers = 8;
  constexpr unsigned rounds = 25000;
  {
    pool_type pool(&resource, workers, 4096);
    std::atomic<std::size_t> accepted{0};
    std::vector<std::jthread> threads;
    for (std::size_t worker = 0; worker < workers; ++worker) {
      threads.emplace_back([&, worker] {
        for (unsigned i = 0; i < rounds; ++i) {
          auto* node = pool.acquire(move_task{i}, worker);
          if (node->callable.payload && *node->callable.payload == i)
            accepted.fetch_add(1, std::memory_order_relaxed);
          pool.recycle(node, worker);
          // Periodically exercise the central freelist from the same thread.
          if ((i & 31U) == 0) {
            auto* shared = pool.acquire(move_task{i});
            pool.recycle(shared);
          }
        }
      });
    }
    threads.clear();
    require(accepted.load() == workers * rounds, "lost or corrupt concurrent task");
    // External producers can reuse nodes spilled from worker caches.
    std::vector<pool_type::node*> burst;
    burst.reserve(1024);
    for (unsigned i = 0; i < 1024; ++i)
      burst.push_back(pool.acquire(move_task{i}));
    std::vector<std::jthread> consumers;
    for (std::size_t worker = 0; worker < workers; ++worker)
      consumers.emplace_back([&, worker] {
        for (std::size_t i = worker; i < burst.size(); i += workers)
          pool.recycle(burst[i], worker);
      });
    consumers.clear();
  }
  require(resource.allocations.load() == resource.deallocations.load(),
          "cross-thread recycling leaked PMR memory");
}

void allocation_failure() {
  counting_resource resource;
  {
    modern::detail::recycling_node_pool<move_task> pool(&resource, 1, 1);
    resource.fail.store(true);
    bool failed = false;
    try { (void)pool.acquire(move_task{13}); }
    catch (const std::bad_alloc&) { failed = true; }
    resource.fail.store(false);
    require(failed, "new node allocation did not propagate bad_alloc");
    auto* node = pool.acquire(move_task{9});
    pool.recycle(node);
  }
  require(resource.allocations.load() == resource.deallocations.load(),
          "allocation-failure recovery leaked memory");
}

} // namespace

int main() {
  try {
    reuse_and_capacity();
    concurrent_local_and_global();
    allocation_failure();
    std::cout << "PASS: recycling task nodes, bounded caches, concurrency, PMR failure\n";
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
  }
}
