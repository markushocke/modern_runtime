module;

#include "../detail/shared_ptr.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <stop_token>
#include <stdexcept>
#include <type_traits>
#include <utility>

export module modern.runtime:timer_adapter;

export import modern.memory;
export import modern.task;
export import modern.timer;

import modern.task.detail;

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