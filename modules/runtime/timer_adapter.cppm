module;

#include "../detail/shared_ptr.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <memory_resource>
#include <stop_token>
#include <stdexcept>
#include <type_traits>
#include <utility>

export module modern.runtime:timer_adapter;

export import modern.memory;
export import modern.task;
export import modern.timer;

import modern.task_detail;

namespace modern::detail
{
template<class F>
struct stoppable_timer_result;

template<class F>
  requires std::is_invocable_v<F&, std::stop_token>
struct stoppable_timer_result<F>
{
  using type = std::invoke_result_t<F&, std::stop_token>;
  static constexpr bool uses_stop_token = true;
};

template<class F>
  requires (!std::is_invocable_v<F&, std::stop_token> && std::is_invocable_v<F&>)
struct stoppable_timer_result<F>
{
  using type = std::invoke_result_t<F&>;
  static constexpr bool uses_stop_token = false;
};
}

export namespace modern
{
enum class cancellation_outcome
{
  pending,
  completed,
  parent_cancelled,
  deadline_expired,
  runtime_shutdown
};

namespace detail
{
class deadline_cancellation_state
{
public:
  bool finish(cancellation_outcome desired, bool request_stop) noexcept
  {
    auto expected = cancellation_outcome::pending;
    if (!outcome_.compare_exchange_strong(
          expected, desired, std::memory_order_acq_rel, std::memory_order_acquire))
      return false;
    if (request_stop)
      stop_source_.request_stop();
    return true;
  }

  [[nodiscard]] std::stop_token token() const noexcept { return stop_source_.get_token(); }
  [[nodiscard]] cancellation_outcome outcome() const noexcept
  {
    return outcome_.load(std::memory_order_acquire);
  }

private:
  std::stop_source stop_source_;
  std::atomic<cancellation_outcome> outcome_{cancellation_outcome::pending};
};
} // namespace detail

class deadline_cancellation_controller
{
public:
  using parent_callback = std::stop_callback<std::function<void()>>;

  deadline_cancellation_controller(
    scheduled_executor& timers,
    std::stop_token parent,
    deadline due = deadline::unbounded(),
    memory::memory_resource* resource = memory::get_default_resource())
    : state_(detail::allocate_shared_object<detail::deadline_cancellation_state>(
        resource ? resource : memory::get_default_resource()))
  {
    if (parent.stop_possible())
    {
      parent_callback_ = std::make_unique<parent_callback>(
        parent,
        std::function<void()>{[state = state_]
        {
          state->finish(cancellation_outcome::parent_cancelled, true);
        }});
    }

    if (!due.bounded())
      return;

    if (due.expired())
    {
      state_->finish(cancellation_outcome::deadline_expired, true);
      return;
    }

    timers.schedule_at(
      *due.value(),
      [state = state_]
      {
        state->finish(cancellation_outcome::deadline_expired, true);
      },
      [state = state_]
      {
        state->finish(cancellation_outcome::runtime_shutdown, true);
      });
  }

  deadline_cancellation_controller(deadline_cancellation_controller&&) noexcept = default;
  deadline_cancellation_controller& operator=(deadline_cancellation_controller&&) noexcept = default;
  deadline_cancellation_controller(const deadline_cancellation_controller&) = delete;
  deadline_cancellation_controller& operator=(const deadline_cancellation_controller&) = delete;

  ~deadline_cancellation_controller()
  {
    complete();
  }

  [[nodiscard]] std::stop_token token() const noexcept { return state_->token(); }
  [[nodiscard]] cancellation_outcome outcome() const noexcept { return state_->outcome(); }

  bool complete() noexcept
  {
    return state_ && state_->finish(cancellation_outcome::completed, false);
  }

private:
  std::shared_ptr<detail::deadline_cancellation_state> state_;
  std::unique_ptr<parent_callback> parent_callback_;
};

template<class Clock, class Duration, class F>
auto schedule_at(
  scheduled_executor& executor,
  std::chrono::time_point<Clock, Duration> due,
  memory::memory_resource* resource,
  F&& f)
{
  using R = std::invoke_result_t<std::remove_cvref_t<F>&>;

  auto callable = std::remove_cvref_t<F>(std::forward<F>(f));
  auto result_state = detail::make_shared_state<R>(executor.target_scheduler(), resource);
  auto result = detail::make_task<R>(result_state);

  executor.schedule_at(
    due,
    [result_state, callable = std::move(callable)]() mutable
    {
      detail::fulfill<R>(result_state, [&]() mutable -> R
      {
        if constexpr (std::is_void_v<R>)
        {
          std::invoke(callable);
        }
        else
        {
          return std::invoke(callable);
        }
      });
    },
    [result_state]
    {
      result_state->set_exception(std::make_exception_ptr(
        std::runtime_error("scheduled_executor was stopped before task became due")));
    });

  return result;
}

template<class Clock, class Duration, class F>
auto schedule_at(scheduled_executor& executor, std::chrono::time_point<Clock, Duration> due, F&& f)
{
  return modern::schedule_at(executor, due, executor.resource(), std::forward<F>(f));
}

template<class Clock, class Duration, class F>
auto schedule_at(
  scheduled_executor& executor,
  std::chrono::time_point<Clock, Duration> due,
  std::stop_token token,
  memory::memory_resource* resource,
  F&& f)
{
  using callable_type = std::remove_cvref_t<F>;
  using result_traits = detail::stoppable_timer_result<callable_type>;
  using R = typename result_traits::type;

  auto* actual_resource = resource ? resource : executor.resource();
  auto callable = callable_type(std::forward<F>(f));
  auto result_state = detail::make_shared_state<R>(executor.target_scheduler(), actual_resource);
  auto result = detail::make_task<R>(result_state);

  if (token.stop_requested())
  {
    detail::cancel_state(result_state);
    return result;
  }

  using callback_type = std::stop_callback<std::function<void()>>;
  auto started = detail::allocate_shared_object<std::atomic<bool>>(actual_resource, false);
  auto callback = detail::allocate_shared_object<callback_type>(
    actual_resource,
    token,
    std::function<void()>{[result_state, started]
    {
      if (!started->exchange(true, std::memory_order_acq_rel))
        detail::cancel_state(result_state);
    }});

  executor.schedule_at(
    due,
    [result_state, started, callback, token, callable = std::move(callable)]() mutable
    {
      if (started->exchange(true, std::memory_order_acq_rel))
        return;

      if (token.stop_requested())
      {
        detail::cancel_state(result_state);
        return;
      }

      detail::fulfill<R>(result_state, [&]() mutable -> R
      {
        if constexpr (result_traits::uses_stop_token)
        {
          if constexpr (std::is_void_v<R>)
          {
            std::invoke(callable, token);
          }
          else
          {
            return std::invoke(callable, token);
          }
        }
        else
        {
          if constexpr (std::is_void_v<R>)
          {
            std::invoke(callable);
          }
          else
          {
            return std::invoke(callable);
          }
        }
      });
    },
    [result_state, started]
    {
      if (!started->exchange(true, std::memory_order_acq_rel))
      {
        result_state->set_exception(std::make_exception_ptr(
          std::runtime_error("scheduled_executor was stopped before task became due")));
      }
    });

  return result;
}

template<class Clock, class Duration, class F>
auto schedule_at(
  scheduled_executor& executor,
  std::chrono::time_point<Clock, Duration> due,
  std::stop_token token,
  F&& f)
{
  return modern::schedule_at(executor, due, token, executor.resource(), std::forward<F>(f));
}

template<class Rep, class Period, class F>
auto schedule_after(
  scheduled_executor& executor,
  std::chrono::duration<Rep, Period> delay,
  memory::memory_resource* resource,
  F&& f)
{
  return modern::schedule_at(
    executor,
    std::chrono::steady_clock::now() + delay,
    resource,
    std::forward<F>(f));
}

template<class Rep, class Period, class F>
auto schedule_after(
  scheduled_executor& executor,
  std::chrono::duration<Rep, Period> delay,
  std::stop_token token,
  memory::memory_resource* resource,
  F&& f)
{
  return modern::schedule_at(
    executor,
    std::chrono::steady_clock::now() + delay,
    token,
    resource,
    std::forward<F>(f));
}

template<class Rep, class Period, class F>
auto schedule_after(scheduled_executor& executor, std::chrono::duration<Rep, Period> delay, F&& f)
{
  return modern::schedule_after(executor, delay, executor.resource(), std::forward<F>(f));
}

template<class Rep, class Period, class F>
auto schedule_after(
  scheduled_executor& executor,
  std::chrono::duration<Rep, Period> delay,
  std::stop_token token,
  F&& f)
{
  return modern::schedule_after(executor, delay, token, executor.resource(), std::forward<F>(f));
}
} // namespace modern
