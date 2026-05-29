module;

#include "../detail/move_only_function_support.hpp"
#include "../detail/intrusive_queue.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <semaphore>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

module modern.thread:state;

import modern.exec;

namespace modern::detail
{
class thread_pool_state;

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
    std::span<const std::size_t> worker_affinity = {});
  ~thread_pool_state();

  [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;

  void start(std::size_t thread_count);
  void execute(move_only_function task, scheduler_priority priority = scheduler_priority::normal);
  void shutdown() noexcept;
  void join();

private:
  [[nodiscard]] move_only_function pop_local_task(std::size_t worker_index);
  [[nodiscard]] move_only_function steal_task(std::size_t worker_index);
  void worker_loop(std::size_t worker_index, std::stop_token stop_token);

  std::pmr::memory_resource* resource_;
  std::size_t max_pending_tasks_;
  std::atomic<std::size_t> pending_tasks_ = 0;
  std::atomic<std::size_t> next_queue_ = 0;
  std::atomic<bool> stopping_ = false;
  std::counting_semaphore<> available_tasks_{0};
  std::pmr::vector<std::unique_ptr<worker_queue>> queues_;
  std::pmr::vector<std::jthread> workers_;
  std::pmr::vector<std::size_t> worker_affinity_;
};
} // namespace modern::detail
