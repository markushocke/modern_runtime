module;

#include <atomic>
#include "../detail/task_semaphore.hpp"
#include "../detail/worker_parking.hpp"
#include "../detail/move_only_function_support.hpp"
#include "../detail/shared_ptr.hpp"

#include "../detail/intrusive_queue.hpp"

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <utility>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

module modern.thread;

import :api;
import modern.memory;
import modern.platform;
import :state;

namespace modern::detail
{
// Concrete implementation behind queue_strategy. Local queues and the
// producer-injection inboxes use independent mutexes and PMR-backed storage.
// A future Chase-Lev implementation needs only implement queue_strategy.
class mutex_queue_strategy final : public queue_strategy
{
public:
  mutex_queue_strategy(std::pmr::memory_resource* resource,
                       std::size_t worker_count, bool injection)
    : locals_(resource), injections_(resource), injection_(injection)
  {
    locals_.reserve(worker_count);
    if (injection_)
      injections_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i)
    {
      locals_.push_back(std::make_unique<worker_queue>(resource));
      if (injection_)
        injections_.push_back(std::make_unique<worker_queue>(resource));
    }
  }

  [[nodiscard]] std::size_t size() const noexcept override
  {
    return locals_.size();
  }

  void push(std::size_t target, bool from_worker, move_only_function task,
            scheduler_priority priority) override
  {
    auto& queue = injection_ && !from_worker
      ? *injections_[target] : *locals_[target];
    std::lock_guard lock(queue.mutex);
    queue.push(std::move(task), priority);
  }

  [[nodiscard]] move_only_function pop_local(std::size_t worker) override
  {
    auto& queue = *locals_[worker];
    std::lock_guard lock(queue.mutex);
    return queue.pop_front();
  }

  [[nodiscard]] move_only_function pop_injected(std::size_t worker) override
  {
    if (!injection_)
      return {};
    auto& queue = *injections_[worker];
    std::lock_guard lock(queue.mutex);
    return queue.pop_front();
  }

  [[nodiscard]] move_only_function steal_local(
    std::size_t worker, std::size_t& next_victim) override
  {
    return steal_from(locals_, worker, next_victim);
  }

  [[nodiscard]] move_only_function steal_injected(
    std::size_t worker, std::size_t& next_victim) override
  {
    if (!injection_)
      return {};
    // The pool semaphore is shared, not targeted to inbox owners.
    // Without assistance, a worker might hold the last permit while the
    // only published work sits in another worker's inbox.
    return steal_from(injections_, worker, next_victim);
  }

private:
  using queue_vector = std::pmr::vector<std::unique_ptr<worker_queue>>;

  [[nodiscard]] static move_only_function steal_from(
    queue_vector& queues, std::size_t worker, std::size_t& next_victim)
  {
    const auto count = queues.size();
    if (count < 2)
      return {};

    const auto first = next_victim % count;
    for (std::size_t offset = 0; offset < count; ++offset)
    {
      const auto victim = (first + offset) % count;
      if (victim == worker)
        continue;
      auto& queue = *queues[victim];
      std::unique_lock lock(queue.mutex, std::try_to_lock);
      if (!lock.owns_lock())
        continue;
      if (!queue.empty())
      {
        next_victim = (victim + 1) % count;
        return queue.pop_back();
      }
    }
    next_victim = (first + 1) % count;
    return {};
  }

  queue_vector locals_;
  queue_vector injections_;
  bool injection_;
};

thread_pool_state::thread_pool_state(
  std::pmr::memory_resource* resource,
  std::size_t max_pending_tasks,
  std::span<const std::size_t> worker_affinity,
  queue_backend backend,
  worker_wakeup wakeup)
  : resource_(resource ? resource : std::pmr::get_default_resource()),
    max_pending_tasks_(max_pending_tasks == 0
      ? 1
      : (max_pending_tasks > pending_mask ? pending_mask : max_pending_tasks)),
    backend_(backend),
    wakeup_(wakeup),
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

  // Reserve room for shutdown permits as well as queued work.
  const auto permit_limit = static_cast<std::size_t>(task_semaphore::max());
  if (thread_count >= permit_limit)
    throw std::length_error("too many thread_pool workers");
  if (max_pending_tasks_ > permit_limit - thread_count)
    max_pending_tasks_ = permit_limit - thread_count;

  // Initialize queues entirely before workers can acquire any permits.
  switch (backend_)
  {
    case queue_backend::direct_mutex:
      queues_ = std::make_unique<mutex_queue_strategy>(resource_, thread_count, false);
      break;
    case queue_backend::injection_mutex_local:
      queues_ = std::make_unique<mutex_queue_strategy>(resource_, thread_count, true);
      break;
    case queue_backend::injection_chase_lev:
      queues_ = make_chase_lev_queue_strategy(
        resource_, thread_count, max_pending_tasks_);
      break;
    default:
      throw std::invalid_argument("unknown queue_backend");
  }
  if (wakeup_ == worker_wakeup::idle_parking ||
      wakeup_ == worker_wakeup::idle_parking_futex)
    parking_ = std::make_unique<worker_parking>(
      thread_count, resource_, wakeup_ == worker_wakeup::idle_parking_futex);
  else if (wakeup_ != worker_wakeup::per_task_semaphore)
    throw std::invalid_argument("unknown worker_wakeup");
  workers_.reserve(thread_count);

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

void thread_pool_state::finish_pending_slot() noexcept
{
  const auto previous = work_state_.fetch_sub(1, std::memory_order_acq_rel);
  if ((previous & stop_bit) != 0 && (previous & pending_mask) == 1)
  {
    if (parking_)
      parking_->notify_all();
    else
      available_tasks_.release(static_cast<std::ptrdiff_t>(workers_.size()));
  }
}

void thread_pool_state::execute(move_only_function task, scheduler_priority priority)
{
  if (!task)
    throw std::invalid_argument("cannot schedule an empty task");

  auto state = work_state_.load(std::memory_order_relaxed);
  while (true)
  {
    if ((state & stop_bit) != 0)
      throw std::runtime_error("thread_pool is stopping");

    if ((state & pending_mask) >= max_pending_tasks_)
      throw std::runtime_error("thread_pool queue is full");

    if (work_state_.compare_exchange_weak(
          state, state + 1,
          parking_
            ? std::memory_order_seq_cst : std::memory_order_acq_rel,
          std::memory_order_relaxed))
      break;
  }

  const auto queue_count = queues_->size();
  const bool from_worker = current_thread_pool == this &&
                           current_worker_index < queue_count;
  // The Chase-Lev strategy has one global MPMC injection ring; selecting a
  // particular inbox there only adds contention on next_queue_.
  const auto queue_index = from_worker
    ? current_worker_index
    : backend_ == queue_backend::injection_chase_lev
      ? 0
      : next_queue_.fetch_add(1, std::memory_order_relaxed) % queue_count;

  try
  {
    // A submission already reserved before shutdown is drained normally.
    queues_->push(queue_index, from_worker, std::move(task), priority);
  }
  catch (...)
  {
    finish_pending_slot();
    throw;
  }

  // Publish precedes notification. Idle parking reserves at most one
  // sleeping worker per notification; active workers drain queues directly.
  if (parking_)
    parking_->notify_one();
  else
    available_tasks_.release();
}

void thread_pool_state::shutdown() noexcept
{
  const auto previous = work_state_.fetch_or(
    stop_bit,
    parking_
      ? std::memory_order_seq_cst : std::memory_order_acq_rel);
  if ((previous & stop_bit) == 0 && (previous & pending_mask) == 0)
  {
    if (parking_)
      parking_->notify_all();
    else
      available_tasks_.release(static_cast<std::ptrdiff_t>(workers_.size()));
  }

  for (auto& worker : workers_)
    worker.request_stop();
}

void thread_pool_state::join()
{
  for (auto& worker : workers_)
  {
    if (worker.joinable())
      worker.join();
  }
}

} // namespace modern::detail

namespace modern
{
thread_pool::thread_pool(thread_pool_options options)
  : thread_pool(options.workers,
                options.resource,
                options.max_pending_tasks,
                options.worker_affinity,
                options.backend,
                options.wakeup)
{
}

thread_pool::thread_pool(
  std::size_t thread_count,
  memory::memory_resource* resource,
  std::size_t max_pending_tasks,
  std::span<const std::size_t> worker_affinity,
  queue_backend backend,
  worker_wakeup wakeup)
  : state_(detail::allocate_shared_object<detail::thread_pool_state>(
      resource ? resource : memory::get_default_resource(),
      resource,
      max_pending_tasks,
      worker_affinity,
      backend,
      wakeup))
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
