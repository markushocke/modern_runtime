module;

#include "../detail/shared_ptr.hpp"

#include <atomic>
#include <condition_variable>
#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

export module modern.runtime:fiber_scheduler;

export import modern.exec;
export import modern.memory;

namespace modern::detail
{
class fiber_scheduler_state;

template<class Callable>
class fiber_runner;

template<class T>
inline constexpr bool fiber_scheduler_false = false;
} // namespace modern::detail

export namespace modern
{
class fiber_context
{
public:
  void yield() noexcept
  {
    if (yield_requested_)
      *yield_requested_ = true;
  }

  [[nodiscard]] bool stop_requested() const noexcept;

  void request_stop() const noexcept;

private:
  template<class Callable>
  friend class detail::fiber_runner;

  fiber_context(std::shared_ptr<detail::fiber_scheduler_state> scheduler_state, bool* yield_requested) noexcept
    : scheduler_state_(std::move(scheduler_state)),
      yield_requested_(yield_requested)
  {
  }

  std::shared_ptr<detail::fiber_scheduler_state> scheduler_state_;
  bool* yield_requested_ = nullptr;
};
} // namespace modern

namespace modern::detail
{
class fiber_scheduler_state
{
public:
  explicit fiber_scheduler_state(scheduler executor, memory::memory_resource* resource) noexcept
    : executor_(std::move(executor)),
      resource_(resource ? resource : memory::get_default_resource())
  {
  }

  [[nodiscard]] scheduler executor() const
  {
    return executor_;
  }

  [[nodiscard]] memory::memory_resource* resource() const noexcept
  {
    return resource_;
  }

  [[nodiscard]] std::stop_token token() const noexcept
  {
    return stop_source_.get_token();
  }

  void add_fiber()
  {
    std::lock_guard lock(mutex_);
    ++active_fibers_;
  }

  void fiber_completed() noexcept
  {
    std::lock_guard lock(mutex_);

    if (active_fibers_ > 0)
      --active_fibers_;

    if (active_fibers_ == 0)
      cv_.notify_all();
  }

  void record_exception(std::exception_ptr exception) noexcept
  {
    bool should_cancel = false;

    {
      std::lock_guard lock(mutex_);

      if (!exception_)
      {
        exception_ = exception;
        should_cancel = true;
      }
    }

    if (should_cancel)
      request_stop();
  }

  void request_stop() noexcept
  {
    stop_source_.request_stop();
  }

  [[nodiscard]] bool stop_requested() const noexcept
  {
    return stop_source_.stop_requested();
  }

  void join()
  {
    std::exception_ptr exception;

    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this]
      {
        return active_fibers_ == 0;
      });

      exception = exception_;
    }

    if (exception)
      std::rethrow_exception(exception);
  }

  void join_noexcept() noexcept
  {
    try
    {
      join();
    }
    catch (...)
    {
    }
  }

private:
  scheduler executor_;
  memory::memory_resource* resource_;
  std::stop_source stop_source_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::size_t active_fibers_ = 0;
  std::exception_ptr exception_;
};

template<class Callable>
void invoke_fiber_callable(Callable& callable, std::stop_token token, fiber_context& context)
{
  if constexpr (std::is_invocable_v<Callable&, std::stop_token, fiber_context&>)
  {
    std::invoke(callable, token, context);
  }
  else if constexpr (std::is_invocable_v<Callable&, fiber_context&>)
  {
    std::invoke(callable, context);
  }
  else if constexpr (std::is_invocable_v<Callable&, std::stop_token>)
  {
    std::invoke(callable, token);
  }
  else if constexpr (std::is_invocable_v<Callable&>)
  {
    std::invoke(callable);
  }
  else
  {
    static_assert(fiber_scheduler_false<Callable>,
      "fiber callable must accept fiber_context&, std::stop_token + fiber_context&, std::stop_token, or no arguments");
  }
}

template<class Callable>
class fiber_runner final : public std::enable_shared_from_this<fiber_runner<Callable>>
{
public:
  fiber_runner(
    std::shared_ptr<fiber_scheduler_state> scheduler_state,
    scheduler_priority priority,
    Callable callable) noexcept(std::is_nothrow_move_constructible_v<Callable>)
    : scheduler_state_(std::move(scheduler_state)),
      priority_(priority),
      callable_(std::move(callable))
  {
  }

  void schedule()
  {
    auto self = this->shared_from_this();
    scheduler_state_->executor().execute(priority_, [self]() mutable
    {
      self->run();
    });
  }

private:
  void run()
  {
    bool yield_requested = false;
    fiber_context context{scheduler_state_, &yield_requested};

    try
    {
      invoke_fiber_callable(callable_, scheduler_state_->token(), context);
    }
    catch (...)
    {
      scheduler_state_->record_exception(std::current_exception());
      complete();
      return;
    }

    if (yield_requested && !scheduler_state_->stop_requested())
    {
      try
      {
        schedule();
        return;
      }
      catch (...)
      {
        scheduler_state_->record_exception(std::current_exception());
      }
    }

    complete();
  }

  void complete() noexcept
  {
    if (!completed_.exchange(true, std::memory_order_acq_rel))
      scheduler_state_->fiber_completed();
  }

  std::shared_ptr<fiber_scheduler_state> scheduler_state_;
  scheduler_priority priority_;
  Callable callable_;
  std::atomic<bool> completed_ = false;
};
} // namespace modern::detail

export namespace modern
{
inline bool fiber_context::stop_requested() const noexcept
{
  return scheduler_state_ ? scheduler_state_->stop_requested() : true;
}

inline void fiber_context::request_stop() const noexcept
{
  if (scheduler_state_)
    scheduler_state_->request_stop();
}

class fiber_scheduler
{
public:
  explicit fiber_scheduler(scheduler executor, memory::memory_resource* resource = memory::get_default_resource())
    : state_(detail::allocate_shared_object<detail::fiber_scheduler_state>(
        resource ? resource : memory::get_default_resource(),
        std::move(executor),
        resource ? resource : memory::get_default_resource()))
  {
  }

  fiber_scheduler(fiber_scheduler&&) noexcept = default;
  fiber_scheduler& operator=(fiber_scheduler&&) noexcept = default;

  fiber_scheduler(const fiber_scheduler&) = delete;
  fiber_scheduler& operator=(const fiber_scheduler&) = delete;

  ~fiber_scheduler()
  {
    if (state_)
    {
      state_->request_stop();
      state_->join_noexcept();
    }
  }

  template<class F>
  void spawn(F&& fiber)
  {
    spawn(scheduler_priority::normal, std::forward<F>(fiber));
  }

  template<class F>
  void spawn(scheduler_priority priority, F&& fiber)
  {
    using fiber_type = std::remove_cvref_t<F>;

    auto state = state_or_throw();
    auto runner = detail::allocate_shared_object<detail::fiber_runner<fiber_type>>(
      state->resource(),
      state,
      priority,
      fiber_type(std::forward<F>(fiber)));

    state->add_fiber();

    try
    {
      runner->schedule();
    }
    catch (...)
    {
      state->fiber_completed();
      throw;
    }
  }

  void request_stop() noexcept
  {
    if (state_)
      state_->request_stop();
  }

  [[nodiscard]] bool stop_requested() const noexcept
  {
    return state_ ? state_->stop_requested() : true;
  }

  void join()
  {
    state_or_throw()->join();
  }

private:
  std::shared_ptr<detail::fiber_scheduler_state> state_or_throw() const
  {
    if (!state_)
      throw std::logic_error("invalid fiber_scheduler");

    return state_;
  }

  std::shared_ptr<detail::fiber_scheduler_state> state_;
};
} // namespace modern