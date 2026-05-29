module;

#include "../detail/move_only_function_support.hpp"
#include "../detail/shared_ptr.hpp"

#include <functional>
#include <type_traits>
#include <utility>

export module modern.timer:delayed;

import modern.exec;
import :api;

namespace modern
{
template<class Clock, class Duration, class F>
void scheduled_executor::schedule_at(std::chrono::time_point<Clock, Duration> due, F&& f)
{
  schedule_at(due, std::forward<F>(f), [] {});
}

template<class Clock, class Duration, class F, class OnCancel>
void scheduled_executor::schedule_at(std::chrono::time_point<Clock, Duration> due, F&& f, OnCancel&& on_cancel)
{
  auto due_steady = to_steady_time(due);
  auto target = target_scheduler_of(state_);
  auto* resource = resource_of(state_);
  auto callable = std::remove_cvref_t<F>(std::forward<F>(f));
  auto cancel = detail::allocate_shared_object<std::remove_cvref_t<OnCancel>>(
    resource,
    std::forward<OnCancel>(on_cancel));

  enqueue_at_on(
    state_,
    due_steady,
    detail::move_only_function{resource,
      [target, callable = std::move(callable), cancel]() mutable
      {
        try
        {
          target.execute([callable = std::move(callable)]() mutable
          {
            std::invoke(callable);
          });
        }
        catch (...)
        {
          try
          {
            std::invoke(*cancel);
          }
          catch (...)
          {
          }
        }
      }},
    detail::move_only_function{resource,
      [cancel]() mutable
      {
        std::invoke(*cancel);
      }});
}

template<class Rep, class Period, class F>
void scheduled_executor::schedule_after(std::chrono::duration<Rep, Period> delay, F&& f)
{
  schedule_at(std::chrono::steady_clock::now() + delay, std::forward<F>(f));
}

template<class Rep, class Period, class F, class OnCancel>
void scheduled_executor::schedule_after(std::chrono::duration<Rep, Period> delay, F&& f, OnCancel&& on_cancel)
{
  schedule_at(std::chrono::steady_clock::now() + delay, std::forward<F>(f), std::forward<OnCancel>(on_cancel));
}
} // namespace modern
