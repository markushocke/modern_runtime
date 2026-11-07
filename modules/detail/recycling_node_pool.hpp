#ifndef MODERN_RUNTIME_RECYCLING_NODE_POOL_HPP
#define MODERN_RUNTIME_RECYCLING_NODE_POOL_HPP

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace modern::detail {

// Retains a live, empty task node between queue operations. No retired node
// is accessed until its deque/MPMC consumer has exclusive ownership.
template<class Callable>
struct recycling_task_node {
  explicit recycling_task_node(Callable&& task) noexcept(
    std::is_nothrow_move_constructible_v<Callable>)
    : callable(std::move(task)) {}

  Callable callable;
  recycling_task_node* next_free = nullptr;
};

// One writer for each local cache (the corresponding pool worker only).
// Other threads exchange nodes through a mutex-protected central freelist.
// No hazard pointers, ABA-sensitive pointer CAS or per-task vector growth.
template<class Callable>
class recycling_node_pool {
public:
  using node = recycling_task_node<Callable>;
  static constexpr std::size_t no_worker = std::numeric_limits<std::size_t>::max();
  static constexpr std::size_t local_limit = 16;
  static constexpr std::size_t central_limit = 4096;

  recycling_node_pool(std::pmr::memory_resource* resource,
                      std::size_t workers, std::size_t max_pending)
    : allocator_(resource ? resource : std::pmr::get_default_resource()),
      locals_(resource ? resource : std::pmr::get_default_resource()),
      max_central_(std::min(max_pending, central_limit)) {
    static_assert(std::is_nothrow_move_assignable_v<Callable>);
    static_assert(std::is_nothrow_destructible_v<Callable>);
    static_assert(std::is_default_constructible_v<Callable>);
    locals_.resize(workers); // Only changed before workers start.
  }

  recycling_node_pool(const recycling_node_pool&) = delete;
  recycling_node_pool& operator=(const recycling_node_pool&) = delete;

  ~recycling_node_pool() {
    // The enclosing thread_pool_state must have joined all its workers.
    for (auto& cache : locals_) {
      while (cache.head) {
        auto* node = pop(cache.head);
        destroy(node);
      }
    }
    while (central_head_) {
      auto* node = pop(central_head_);
      destroy(node);
    }
  }

  [[nodiscard]] node* acquire(Callable&& task,
                              std::size_t worker = no_worker) {
    node* result = nullptr;
    if (worker < locals_.size()) {
      auto& cache = locals_[worker];
      result = pop(cache.head);
      if (result) --cache.count;
    }
    if (!result) {
      std::lock_guard lock(central_mutex_);
      result = pop(central_head_);
      if (result) --central_count_;
    }
    if (!result) {
      result = allocator_.allocate(1);
      try {
        std::construct_at(result, std::move(task));
      } catch (...) {
        allocator_.deallocate(result, 1);
        throw;
      }
    } else {
      result->callable = std::move(task);
    }
    return result;
  }

  void recycle(node* result, std::size_t worker = no_worker) noexcept {
    if (!result) return;
    // Release captured resources before caching the memory; reuse never
    // extends the lifetime of the previous task's callable or references.
    result->callable = Callable{};
    if (worker < locals_.size()) {
      auto& cache = locals_[worker];
      if (cache.count < local_limit) {
        push(cache.head, result);
        ++cache.count;
        return;
      }
    }
    bool retained = false;
    {
      std::lock_guard lock(central_mutex_);
      if (central_count_ < max_central_) {
        push(central_head_, result);
        ++central_count_;
        retained = true;
      }
    }
    if (!retained) destroy(result);
  }

private:
  // Isolate independent worker-owned heads from neighbouring cache lines.
  struct alignas(64) local_cache {
    node* head = nullptr;
    std::size_t count = 0;
  };

  static void push(node*& head, node* item) noexcept {
    item->next_free = head;
    head = item;
  }

  static node* pop(node*& head) noexcept {
    if (!head) return nullptr;
    auto* result = head;
    head = result->next_free;
    result->next_free = nullptr;
    return result;
  }

  void destroy(node* item) noexcept {
    std::destroy_at(item);
    allocator_.deallocate(item, 1);
  }

  std::pmr::polymorphic_allocator<node> allocator_;
  std::pmr::vector<local_cache> locals_;
  const std::size_t max_central_;
  std::mutex central_mutex_;
  node* central_head_ = nullptr;
  std::size_t central_count_ = 0;
};

} // namespace modern::detail
#endif
