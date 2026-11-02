module;

#include <atomic>
#include <exception>
#include <semaphore>
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
  (void)stop_token; // Shutdown and draining are controlled by work_state_.

  while (true)
  {
    available_tasks_.acquire();

    const auto state = work_state_.load(std::memory_order_acquire);
    if ((state & stop_bit) != 0 && (state & pending_mask) == 0)
      return;

    auto task = pop_local_task(worker_index);
    if (!task)
      task = steal_task(worker_index);

    // One published task corresponds to exactly one semaphore permit.
    if (!task)
      std::terminate();

    // The slot is released after dequeue, not after user-code execution.
    finish_pending_slot();
    task();
  }
}
} // namespace modern::detail
