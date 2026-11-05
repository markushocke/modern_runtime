module;

#include <atomic>
#include <exception>
#include "../detail/task_semaphore.hpp"
#include "../detail/move_only_function_support.hpp"
#include "../detail/intrusive_queue.hpp"

#include <mutex>
#include <stop_token>
#include <thread>
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
  (void)stop_token; // Shutdown and draining are controlled by work_state_.
  std::size_t next_victim = worker_index + 1;

  while (true)
  {
    available_tasks_.acquire();

    const auto state = work_state_.load(std::memory_order_acquire);
    if ((state & stop_bit) != 0 && (state & pending_mask) == 0)
      return;

    move_only_function task;
    while (!task)
    {
      task = queues_->pop_local(worker_index);
      if (!task)
        task = queues_->pop_injected(worker_index);
      if (!task)
        task = queues_->steal_local(worker_index, next_victim);
      if (!task)
        task = queues_->steal_injected(worker_index, next_victim);
      if (!task)
      {
        // A scan is not a snapshot. Another worker may have taken the
        // published task, or a producer may still hold the inbox mutex.
        // Keep the permit while retrying; never acquire twice.
        const auto remaining = work_state_.load(std::memory_order_acquire);
        if ((remaining & stop_bit) != 0 && (remaining & pending_mask) == 0)
          return;
        std::this_thread::yield();
      }
    }

    // The slot is released after dequeue, not after user-code execution.
    finish_pending_slot();
    task();
  }
}
} // namespace modern::detail
