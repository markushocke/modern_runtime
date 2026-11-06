module;

#include "../detail/move_only_function_support.hpp"
#include "../detail/intrusive_queue.hpp"

#include <atomic>
#include <limits>
#include "../detail/task_semaphore.hpp"
#include "../detail/worker_parking.hpp"
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

module modern.thread:state;

import :api;
import modern.exec;

namespace modern::detail
{
class thread_pool_state;

// Uniform ownership and dispatch boundary for interchangeable queue engines.
class queue_strategy
{
public:
  virtual ~queue_strategy() = default;
  [[nodiscard]] virtual std::size_t size() const noexcept = 0;
  virtual void push(std::size_t target, bool from_worker,
                    move_only_function task, scheduler_priority priority) = 0;
  [[nodiscard]] virtual move_only_function pop_local(std::size_t worker) = 0;
  [[nodiscard]] virtual move_only_function pop_injected(std::size_t worker) = 0;
  [[nodiscard]] virtual move_only_function steal_local(
    std::size_t worker, std::size_t& next_victim) = 0;
  [[nodiscard]] virtual move_only_function steal_injected(
    std::size_t worker, std::size_t& next_victim) = 0;
};

// Internal factory: Chase-Lev strategy lives in a separate implementation TU.
[[nodiscard]] std::unique_ptr<queue_strategy> make_chase_lev_queue_strategy(
  std::pmr::memory_resource* resource, std::size_t workers,
  std::size_t max_pending_tasks);

inline thread_local thread_pool_state* current_thread_pool = nullptr;
inline thread_local std::size_t current_worker_index = 0;

struct worker_queue
{
  explicit worker_queue(std::pmr::memory_resource* resource)
    : high_priority_tasks(resource),
      normal_priority_tasks(resource),
      low_priority_tasks(resource)
  {
  }

  [[nodiscard]] bool empty() const noexcept
  {
    return high_priority_tasks.empty()
        && normal_priority_tasks.empty()
        && low_priority_tasks.empty();
  }

  void push(move_only_function task, scheduler_priority priority)
  {
    switch (priority)
    {
      case scheduler_priority::high:
        high_priority_tasks.push(std::move(task));
        return;
      case scheduler_priority::low:
        low_priority_tasks.push(std::move(task));
        return;
      case scheduler_priority::normal:
      default:
        normal_priority_tasks.push(std::move(task));
        return;
    }
  }

  [[nodiscard]] move_only_function pop_front()
  {
    if (!high_priority_tasks.empty())
      return high_priority_tasks.pop();

    if (!normal_priority_tasks.empty())
      return normal_priority_tasks.pop();

    if (!low_priority_tasks.empty())
      return low_priority_tasks.pop();

    return {};
  }

  [[nodiscard]] move_only_function pop_back()
  {
    if (!high_priority_tasks.empty())
      return high_priority_tasks.pop_back();

    if (!normal_priority_tasks.empty())
      return normal_priority_tasks.pop_back();

    if (!low_priority_tasks.empty())
      return low_priority_tasks.pop_back();

    return {};
  }

  std::mutex mutex;
  intrusive_queue<move_only_function> high_priority_tasks;
  intrusive_queue<move_only_function> normal_priority_tasks;
  intrusive_queue<move_only_function> low_priority_tasks;
};

class thread_pool_state
{
public:
  explicit thread_pool_state(
    std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
    std::size_t max_pending_tasks = 1024,
    std::span<const std::size_t> worker_affinity = {},
    queue_backend backend = queue_backend::direct_mutex,
    worker_wakeup wakeup = worker_wakeup::per_task_semaphore);
  ~thread_pool_state();

  [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;

  void start(std::size_t thread_count);
  void execute(move_only_function task, scheduler_priority priority = scheduler_priority::normal);
  void shutdown() noexcept;
  void join();

private:
  static constexpr std::size_t stop_bit =
    std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
  static constexpr std::size_t pending_mask = stop_bit - 1;

  void finish_pending_slot() noexcept;

  void worker_loop(std::size_t worker_index, std::stop_token stop_token);
  void worker_loop_idle_parking(std::size_t worker_index);

  std::pmr::memory_resource* resource_;
  std::size_t max_pending_tasks_;
  queue_backend backend_;
  worker_wakeup wakeup_;
  // High bit: shutdown. Remaining bits: accepted but not yet claimed tasks.
  std::atomic<std::size_t> work_state_{0};
  std::atomic<std::size_t> next_queue_{0};
  task_semaphore available_tasks_{0};
  // Initialized before any worker starts if idle parking is enabled.
  std::unique_ptr<worker_parking> parking_;
  std::unique_ptr<queue_strategy> queues_;
  std::pmr::vector<std::jthread> workers_;
  std::pmr::vector<std::size_t> worker_affinity_;
};
} // namespace modern::detail
