module;

#include <atomic>
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

  while (true)
  {
    available_tasks_.acquire();

    if ((stopping_.load(std::memory_order_acquire) || stop_token.stop_requested()) &&
        pending_tasks_.load(std::memory_order_acquire) == 0)
    {
      return;
    }

    auto task = pop_local_task(worker_index);

    if (!task)
      task = steal_task(worker_index);

    if (!task)
    {
      if (stopping_.load(std::memory_order_acquire) || stop_token.stop_requested())
        return;

      continue;
    }

    pending_tasks_.fetch_sub(1, std::memory_order_release);

    if (!task)
    {
      if (stopping_.load(std::memory_order_acquire) || stop_token.stop_requested())
        return;

      continue;
    }

    task();
  }
}
} // namespace modern::detail
