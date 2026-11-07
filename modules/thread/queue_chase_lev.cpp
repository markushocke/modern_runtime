module;

#include "../detail/lock_free_queues.hpp"
#include "../detail/move_only_function_support.hpp"
#include "../detail/recycling_node_pool.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <thread>
#include <utility>
#include <vector>

module modern.thread;

import :state;

namespace modern::detail
{
namespace
{
// Stored task objects are immutable while queued. A pointer is claimed by one
// successful deque/MPMC CAS; only that winner may move and destroy its task.
// No thief dereferences a pointer unless its CAS has succeeded.
using task_node = recycling_task_node<move_only_function>;
using pooled_nodes = recycling_node_pool<move_only_function>;

using task_pointer = task_node*;

[[nodiscard]] std::size_t lane_index(scheduler_priority priority) noexcept
{
  switch (priority)
  {
    case scheduler_priority::high: return 0;
    case scheduler_priority::normal: return 1;
    case scheduler_priority::low: return 2;
  }
  return 1;
}

class local_deques
{
public:
  local_deques(std::size_t capacity, std::pmr::memory_resource* resource)
    : high_(capacity, resource),
      normal_(capacity, resource),
      low_(capacity, resource)
  {
  }

  // The worker owning this local queue is the only bottom-side producer and
  // consumer. If the bounded deque is full, its task is put into injection.
  [[nodiscard]] bool try_push(task_pointer node, scheduler_priority priority) noexcept
  {
    switch (lane_index(priority))
    {
      case 0: return high_.try_push_owner(node);
      case 2: return low_.try_push_owner(node);
      default: return normal_.try_push_owner(node);
    }
  }

  [[nodiscard]] task_pointer pop_owner() noexcept
  {
    if (auto* node = high_.pop_owner()) return node;
    if (auto* node = normal_.pop_owner()) return node;
    return low_.pop_owner();
  }

  [[nodiscard]] task_pointer steal() noexcept
  {
    if (auto* node = high_.steal()) return node;
    if (auto* node = normal_.steal()) return node;
    return low_.steal();
  }

private:
  bounded_chase_lev_deque<task_pointer> high_;
  bounded_chase_lev_deque<task_pointer> normal_;
  bounded_chase_lev_deque<task_pointer> low_;
};

class injection_rings
{
public:
  injection_rings(std::size_t capacity, std::pmr::memory_resource* resource)
    : high_(capacity, resource),
      normal_(capacity, resource),
      low_(capacity, resource)
  {
  }

  [[nodiscard]] bool try_push(task_pointer node, scheduler_priority priority) noexcept
  {
    switch (lane_index(priority))
    {
      case 0: return high_.try_push(node);
      case 2: return low_.try_push(node);
      default: return normal_.try_push(node);
    }
  }

  [[nodiscard]] task_pointer pop() noexcept
  {
    if (auto* node = high_.try_pop()) return node;
    if (auto* node = normal_.try_pop()) return node;
    return low_.try_pop();
  }

private:
  bounded_mpmc_queue<task_pointer> high_;
  bounded_mpmc_queue<task_pointer> normal_;
  bounded_mpmc_queue<task_pointer> low_;
};

class chase_lev_queue_strategy final : public queue_strategy
{
public:
  chase_lev_queue_strategy(
    std::pmr::memory_resource* resource, std::size_t workers,
    std::size_t max_pending_tasks, thread_pool_state* owner,
    bool recycle_nodes)
    : resource_(resource),
      owner_(owner),
      node_allocator_(resource),
      locals_(resource),
      // Per-worker local lanes are small and fixed. Overflow safely enters
      // the global injection ring. Do not preallocate N * max_pending slots.
      local_capacity_(std::min<std::size_t>(max_pending_tasks, 1024)),
      injected_(max_pending_tasks, resource)
  {
    if (recycle_nodes)
      pooled_ = std::make_unique<pooled_nodes>(
        resource_, workers, max_pending_tasks);
    locals_.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i)
      locals_.push_back(std::make_unique<local_deques>(local_capacity_, resource));
  }

  ~chase_lev_queue_strategy() override
  {
    // The thread_pool_state destructor shuts down and joins all workers
    // before destroying the strategy; no concurrent consumers remain.
    for (auto& lane : locals_)
      while (auto* node = lane->pop_owner())
        destroy_node(node);
    while (auto* node = injected_.pop())
      destroy_node(node);
  }

  [[nodiscard]] std::size_t size() const noexcept override
  {
    return locals_.size();
  }

  void push(std::size_t target, bool from_worker, move_only_function task,
            scheduler_priority priority) override
  {
    task_node* node;
    if (pooled_)
      node = pooled_->acquire(std::move(task), worker_cache_index());
    else
    {
      node = node_allocator_.allocate(1);
      std::construct_at(node, std::move(task));
    }

    if (from_worker && locals_[target]->try_push(node, priority))
      return;

    // MPMC (not strict MPSC): every worker can drain this queue, which is
    // necessary because pool-wide semaphore permits have no worker target.
    //
    // The ring has >= max_pending_tasks slots *per priority*. Admission
    // already reserves a pending slot, therefore true capacity exhaustion
    // cannot occur without another dequeuer making progress. A transient
    // full observation while a dequeuer is finishing is retried.
    while (!injected_.try_push(node, priority))
      std::this_thread::yield();
  }

  [[nodiscard]] move_only_function pop_local(std::size_t worker) override
  {
    return claim(locals_[worker]->pop_owner());
  }

  [[nodiscard]] move_only_function pop_injected(std::size_t) override
  {
    return claim(injected_.pop());
  }

  [[nodiscard]] move_only_function steal_local(
    std::size_t worker, std::size_t& next_victim) override
  {
    const auto count = locals_.size();
    if (count < 2)
      return {};

    const auto first = next_victim % count;
    for (std::size_t offset = 0; offset < count; ++offset)
    {
      const auto victim = (first + offset) % count;
      if (victim == worker)
        continue;
      if (current_steal_probe_counter) ++*current_steal_probe_counter;
      if (auto* node = locals_[victim]->steal())
      {
        next_victim = (victim + 1) % count;
        return claim(node);
      }
    }
    next_victim = (first + 1) % count;
    return {};
  }

  [[nodiscard]] move_only_function steal_injected(
    std::size_t, std::size_t&) override
  {
    // pop_injected() already drains a globally shared MPMC queue.
    return {};
  }

private:
  [[nodiscard]] move_only_function claim(task_pointer node) noexcept
  {
    if (!node)
      return {};

    move_only_function result = std::move(node->callable);
    destroy_node(node);
    return result;
  }

  void destroy_node(task_pointer node) noexcept
  {
    if (pooled_)
      pooled_->recycle(node, worker_cache_index());
    else
    {
      std::destroy_at(node);
      node_allocator_.deallocate(node, 1);
    }
  }

  [[nodiscard]] std::size_t worker_cache_index() const noexcept
  {
    // The per-worker cache has exactly one accessor thread. External
    // producers, other pools and strategy destruction use the central list.
    return current_thread_pool == owner_
      ? current_worker_index : pooled_nodes::no_worker;
  }

  // PMR is preserved: pointer-ring buffers and callable nodes use the
  // caller-provided memory resource. The resource must be thread-safe when
  // concurrently used by multiple submitting/worker threads.
  std::pmr::memory_resource* resource_;
  thread_pool_state* owner_;
  std::pmr::polymorphic_allocator<task_node> node_allocator_;
  std::unique_ptr<pooled_nodes> pooled_;
  std::pmr::vector<std::unique_ptr<local_deques>> locals_;
  const std::size_t local_capacity_;
  injection_rings injected_;
};
} // namespace

std::unique_ptr<queue_strategy> make_chase_lev_queue_strategy(
  std::pmr::memory_resource* resource, std::size_t workers,
  std::size_t max_pending_tasks, thread_pool_state* owner,
  bool recycle_nodes)
{
  return std::make_unique<chase_lev_queue_strategy>(
    resource, workers, max_pending_tasks, owner, recycle_nodes);
}
} // namespace modern::detail
