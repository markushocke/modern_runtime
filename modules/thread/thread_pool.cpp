module;

#include <atomic>
#include "../detail/move_only_function_support.hpp"
#include "../detail/shared_ptr.hpp"

#include "../detail/intrusive_queue.hpp"

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <thread>

module modern.thread;

import :api;
import modern.memory;
import modern.platform;
import :state;

namespace modern::detail
{
thread_pool_state::thread_pool_state(
  std::pmr::memory_resource* resource,
  std::size_t max_pending_tasks,
  std::span<const std::size_t> worker_affinity)
  : resource_(resource ? resource : std::pmr::get_default_resource()),
    max_pending_tasks_(max_pending_tasks ? max_pending_tasks : 1),
    queues_(resource_),
    workers_(resource_),
    worker_affinity_(worker_affinity.begin(), worker_affinity.end(), resource_)
{
}

thread_pool_state::~thread_pool_state()
{
  shutdown();
  join();
}

std::pmr::memory_resource* thread_pool_state::resource() const noexcept
{
  return resource_;
}

void thread_pool_state::start(std::size_t thread_count)
{
  if (!workers_.empty())
    throw std::logic_error("thread_pool was already started");

  queues_.reserve(thread_count);
  workers_.reserve(thread_count);

  for (std::size_t i = 0; i < thread_count; ++i)
    queues_.push_back(std::make_unique<worker_queue>(resource_));

  for (std::size_t i = 0; i < thread_count; ++i)
  {
    workers_.emplace_back([this, i](std::stop_token stop_token)
    {
      worker_loop(i, stop_token);
    });

    if (!worker_affinity_.empty())
    {
      auto cpu_index = worker_affinity_[i % worker_affinity_.size()];

      if (!platform::set_thread_affinity(workers_.back().native_handle(), cpu_index))
        throw std::runtime_error("failed to set thread affinity");
    }
  }
}

void thread_pool_state::execute(move_only_function task, scheduler_priority priority)
{
  if (stopping_.load(std::memory_order_acquire))
    throw std::runtime_error("thread_pool is stopping");

  auto pending = pending_tasks_.load(std::memory_order_relaxed);

  while (true)
  {
    if (pending >= max_pending_tasks_)
      throw std::runtime_error("thread_pool queue is full");

    if (pending_tasks_.compare_exchange_weak(
          pending,
          pending + 1,
          std::memory_order_acq_rel,
          std::memory_order_relaxed))
    {
      break;
    }
  }

  const auto queue_count = queues_.size();
  std::size_t queue_index = 0;

  if (current_thread_pool == this && current_worker_index < queue_count)
  {
    queue_index = current_worker_index;
  }
  else if (queue_count != 0)
  {
    queue_index = next_queue_.fetch_add(1, std::memory_order_relaxed) % queue_count;
  }

  {
    auto& queue = *queues_[queue_index];
    std::lock_guard lock(queue.mutex);

    if (stopping_.load(std::memory_order_relaxed))
    {
      pending_tasks_.fetch_sub(1, std::memory_order_release);
      throw std::runtime_error("thread_pool is stopping");
    }

    try
    {
      queue.push(std::move(task), priority);
    }
    catch (...)
    {
      pending_tasks_.fetch_sub(1, std::memory_order_release);
      throw;
    }
  }

  available_tasks_.release();
}

void thread_pool_state::shutdown() noexcept
{
  stopping_.store(true, std::memory_order_release);

  for (auto& worker : workers_)
    worker.request_stop();

  available_tasks_.release(static_cast<std::ptrdiff_t>(workers_.size()));
}

void thread_pool_state::join()
{
  for (auto& worker : workers_)
  {
    if (worker.joinable())
      worker.join();
  }
}

move_only_function thread_pool_state::pop_local_task(std::size_t worker_index)
{
  auto& queue = *queues_[worker_index];
  std::lock_guard lock(queue.mutex);

  return queue.pop_front();
}

move_only_function thread_pool_state::steal_task(std::size_t worker_index)
{
  const auto queue_count = queues_.size();

  for (std::size_t offset = 1; offset < queue_count; ++offset)
  {
    auto victim_index = (worker_index + offset) % queue_count;
    auto& queue = *queues_[victim_index];
    std::lock_guard lock(queue.mutex);

    if (!queue.empty())
      return queue.pop_back();
  }

  return {};
}
} // namespace modern::detail

namespace modern
{
thread_pool::thread_pool(
  std::size_t thread_count,
  memory::memory_resource* resource,
  std::size_t max_pending_tasks,
  std::span<const std::size_t> worker_affinity)
  : state_(detail::allocate_shared_object<detail::thread_pool_state>(
      resource ? resource : memory::get_default_resource(),
      resource,
      max_pending_tasks,
      worker_affinity))
{
  if (thread_count == 0)
    thread_count = 1;

  state_->start(thread_count);
}

thread_pool::~thread_pool()
{
  shutdown();
  join();
}

scheduler thread_pool::get_scheduler() const
{
  return scheduler{state_};
}

memory::memory_resource* thread_pool::resource() const noexcept
{
  return state_ ? state_->resource() : memory::get_default_resource();
}

void thread_pool::shutdown() noexcept
{
  if (state_)
    state_->shutdown();
}

void thread_pool::join()
{
  if (state_)
    state_->join();
}
} // namespace modern
