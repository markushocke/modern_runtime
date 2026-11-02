module;

#include <atomic>
#include <condition_variable>
#include "../detail/move_only_function_support.hpp"
#include "../detail/intrusive_queue.hpp"

#include <mutex>
#include <stop_token>
#include <utility>

module modern.thread;

import :state;

namespace modern::detail
{
namespace
{
class worker_context final
{
public:
  worker_context(thread_pool_state* state, std::size_t worker_index) noexcept
  {
    current_thread_pool = state;
    current_worker_index = worker_index;
  }

  ~worker_context()
  {
    current_thread_pool = nullptr;
    current_worker_index = 0;
  }
};
} // namespace

void thread_pool_state::worker_loop(std::size_t worker_index, std::stop_token stop_token)
{
  worker_context context{this, worker_index};
  (void)stop_token; // shutdown() owns the drain policy, not jthread stop requests.

  while (true)
  {
    {
      std::unique_lock wake_lock(wake_mutex_);
      wake_cv_.wait(wake_lock, [this]
      {
        return ready_tasks_ != 0 ||
               (stopping_.load(std::memory_order_acquire) &&
                pending_tasks_.load(std::memory_order_acquire) == 0);
      });

      if (ready_tasks_ == 0)
        return; // shutdown and all accepted queued work has been claimed

      // Reserve one queued job while holding the same lock as enqueue.
      --ready_tasks_;
    }

    auto task = pop_local_task(worker_index);
    if (!task)
      task = steal_task(worker_index);

    if (!task)
    {
      // Defensive recovery: another worker cannot consume more than its
      // reservations, so normally at least one queued job must exist.
      // Do not silently discard a wake-up on a transient empty scan.
      {
        std::lock_guard wake_lock(wake_mutex_);
        ++ready_tasks_;
      }
      wake_cv_.notify_one();
      continue;
    }

    std::size_t previous_pending;
    {
      std::lock_guard wake_lock(wake_mutex_);
      previous_pending = pending_tasks_.fetch_sub(1, std::memory_order_acq_rel);
    }

    if (previous_pending == 1 && stopping_.load(std::memory_order_acquire))
      wake_cv_.notify_all();

    task();
  }
}
} // namespace modern::detail
