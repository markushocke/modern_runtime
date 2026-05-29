module;

#include "../detail/move_only_function_support.hpp"
#include "../detail/shared_ptr.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

module modern.timer;

import :api;
import modern.exec;
import modern.memory;
import modern.thread;
import :state;

namespace modern
{
scheduled_executor::scheduled_executor(scheduler target, memory::memory_resource* resource)
  : state_(detail::allocate_shared_object<detail::timer_state>(
      resource ? resource : memory::get_default_resource(),
      std::move(target),
      resource))
{
  state_->start();
}

scheduled_executor::scheduled_executor(thread_pool& pool)
  : scheduled_executor(pool.get_scheduler(), pool.resource())
{
}

scheduled_executor::scheduled_executor(thread_pool& pool, memory::memory_resource* resource)
  : scheduled_executor(pool.get_scheduler(), resource)
{
}

scheduled_executor::~scheduled_executor()
{
  shutdown();
  join();
}

scheduler scheduled_executor::target_scheduler() const
{
  return target_scheduler_of(state_);
}

memory::memory_resource* scheduled_executor::resource() const noexcept
{
  return resource_of(state_);
}

scheduler scheduled_executor::target_scheduler_of(const std::shared_ptr<detail::timer_state>& state)
{
  return state->target_scheduler();
}

memory::memory_resource* scheduled_executor::resource_of(const std::shared_ptr<detail::timer_state>& state) noexcept
{
  return state ? state->resource() : memory::get_default_resource();
}

void scheduled_executor::enqueue_at_on(
  const std::shared_ptr<detail::timer_state>& state,
  std::chrono::steady_clock::time_point due,
  detail::move_only_function task,
  detail::move_only_function on_cancel)
{
  state->enqueue_at(due, std::move(task), std::move(on_cancel));
}

void scheduled_executor::shutdown() noexcept
{
  if (state_)
    state_->shutdown();
}

void scheduled_executor::join()
{
  if (state_)
    state_->join();
}

detail::timer_state::timer_state(scheduler target, std::pmr::memory_resource* resource)
  : target_(std::move(target)),
    resource_(resource ? resource : std::pmr::get_default_resource()),
    heap_(resource_)
{
}

detail::timer_state::~timer_state()
{
  shutdown();
  join();
}

scheduler detail::timer_state::target_scheduler() const
{
  return target_;
}

std::pmr::memory_resource* detail::timer_state::resource() const noexcept
{
  return resource_;
}

void detail::timer_state::start()
{
  timer_thread_ = std::jthread([this](std::stop_token stop_token)
  {
    timer_loop(stop_token);
  });
}

void detail::timer_state::enqueue_at(
  std::chrono::steady_clock::time_point due,
  detail::move_only_function task,
  detail::move_only_function on_cancel)
{
  {
    std::lock_guard lock(mutex_);

    if (stopping_)
      throw std::runtime_error("scheduled_executor is stopping");

    heap_.push_back(timer_item{due, sequence_++, std::move(task), std::move(on_cancel)});
    std::push_heap(heap_.begin(), heap_.end(), item_is_later{});
  }

  cv_.notify_one();
}

void detail::timer_state::shutdown() noexcept
{
  std::pmr::vector<detail::timer_state::timer_item> canceled{resource_};

  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    canceled = std::move(heap_);
    heap_.clear();
  }

  for (auto& item : canceled)
  {
    if (item.on_cancel)
    {
      try
      {
        item.on_cancel();
      }
      catch (...)
      {
      }
    }
  }

  timer_thread_.request_stop();
  cv_.notify_all();
}

void detail::timer_state::join()
{
  if (timer_thread_.joinable())
    timer_thread_.join();
}
} // namespace modern
