module;

#include <atomic>
#include <exception>
#include "../detail/task_semaphore.hpp"
#include "../detail/worker_parking.hpp"
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
  if (parking_)
  {
    worker_loop_idle_parking(worker_index);
    return;
  }

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
// Idle-park variant: never consume a semaphore permit per task. Queue scans
// are nonblocking; accepted-but-unclaimed count prevents parking when a
// publication is in flight or a scan transiently misses a queued task.
// Shutdown wakes every sleeper only after the last accepted task is claimed.
void thread_pool_state::worker_loop_idle_parking(std::size_t worker_index)
{
  std::size_t next_victim = worker_index + 1;
  std::size_t empty_scans = 0;
  diagnostic_counts* counts = diagnostic_enabled_ ? &diagnostics_[worker_index] : nullptr;

  auto idle_retry = [&]() noexcept
  {
    // A contended scan must not call sched_yield() on every missing task.
    // Keep a brief CPU-relax window before yielding to oversubscribed peers.
    if ((++empty_scans & 63u) == 0)
      std::this_thread::yield();
#if defined(__i386__) || defined(__x86_64__)
    else
      __builtin_ia32_pause();
#else
    else
      std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
  };

  auto find_task = [&]() -> move_only_function
  {
    if (counts) ++counts->scans;
    if (auto task = queues_->pop_local(worker_index))
    {
      if (counts) ++counts->local_hits;
      return task;
    }
    if (auto task = queues_->pop_injected(worker_index))
    {
      if (counts) ++counts->injected_hits;
      return task;
    }
    if (auto task = queues_->steal_local(worker_index, next_victim))
    {
      if (counts) ++counts->steal_hits;
      return task;
    }
    if (counts) ++counts->steal_misses;
    return queues_->steal_injected(worker_index, next_victim);
  };

  while (true)
  {
    if (auto task = find_task())
    {
      empty_scans = 0;
      finish_pending_slot();
      if (counts) ++counts->tasks_executed;
      task();
      continue;
    }

    auto state = work_state_.load(std::memory_order_acquire);
    if ((state & stop_bit) != 0 && (state & pending_mask) == 0)
      return;

    // A pending task might be in the process of publication, or another
    // worker may have won a racing CAS. Do not park while any accepted task
    // remains unclaimed, otherwise the entire pool might sleep on work.
    if ((state & pending_mask) != 0)
    {
      if (counts) ++counts->pending_retries;
      idle_retry();
      continue;
    }

    empty_scans = 0;
    if (counts) ++counts->park_prepares;
    const auto epoch = parking_->prepare(worker_index);
    // Recheck AFTER becoming visible to notifiers. A producer that
    // published before registration cannot cause a lost wakeup.
    if (auto task = find_task())
    {
      if (counts) ++counts->park_cancels;
      parking_->cancel(worker_index);
      finish_pending_slot();
      if (counts) ++counts->tasks_executed;
      task();
      continue;
    }

    // Seq-cst must pair with the publisher's acceptance CAS. Otherwise a
    // worker could miss both pending work and the publisher's parked-count
    // check, and enter atomic::wait despite a queued task.
    state = work_state_.load(std::memory_order_seq_cst);
    if ((state & stop_bit) != 0 && (state & pending_mask) == 0)
    {
      if (counts) ++counts->park_cancels;
      parking_->cancel(worker_index);
      return;
    }

    if ((state & pending_mask) != 0)
    {
      if (counts) { ++counts->park_cancels; ++counts->pending_retries; }
      parking_->cancel(worker_index);
      idle_retry();
      continue;
    }

    if (counts) ++counts->park_waits;
    parking_->wait(worker_index, epoch);
    if (counts) ++counts->park_returns;
  }
}
} // namespace modern::detail
