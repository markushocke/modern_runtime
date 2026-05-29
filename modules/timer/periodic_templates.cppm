module;

#include "../detail/move_only_function_support.hpp"
#include "../detail/shared_ptr.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

export module modern.timer:periodic;

import modern.exec;
import :api;

namespace modern
{
template<class InitialRep, class InitialPeriod,
         class PeriodRep, class PeriodPeriod,
         class F>
scheduled_executor::periodic_handle scheduled_executor::schedule_fixed_rate(
  std::chrono::duration<InitialRep, InitialPeriod> initial_delay,
  std::chrono::duration<PeriodRep, PeriodPeriod> period,
  F&& f)
{
  auto period_steady = std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);

  if (period_steady <= std::chrono::steady_clock::duration::zero())
    throw std::invalid_argument("period must be positive");

  auto stop_source = std::stop_source{};
  auto* resource = resource_of(state_);
  auto callable = detail::allocate_shared_object<std::remove_cvref_t<F>>(
    resource,
    std::forward<F>(f));
  using callback_type = std::function<void(std::chrono::steady_clock::time_point)>;
  auto callback = detail::allocate_shared_object<callback_type>(resource);
  std::weak_ptr<callback_type> weak_callback = callback;

  *callback = [state = state_,
               target = target_scheduler_of(state_),
               token = stop_source.get_token(),
               stop_source,
               callable,
               resource,
               period_steady,
               weak_callback](std::chrono::steady_clock::time_point due) mutable
  {
    if (token.stop_requested())
      return;

    try
    {
      target.execute([state,
                      token,
                      stop_source,
                      callable,
                      resource,
                      period_steady,
                      weak_callback,
                      due]() mutable
      {
        if (token.stop_requested())
          return;

        try
        {
          std::invoke(*callable);
        }
        catch (...)
        {
          stop_source.request_stop();
          return;
        }

        if (token.stop_requested())
          return;

        auto next_due = due + period_steady;
        auto now = std::chrono::steady_clock::now();
        if (next_due < now)
          next_due = now;

        if (auto cb = weak_callback.lock())
        {
          try
          {
            enqueue_at_on(state, next_due, detail::move_only_function{resource,
              [cb, next_due]
              {
                (*cb)(next_due);
              }});
          }
          catch (...)
          {
            stop_source.request_stop();
          }
        }
      });
    }
    catch (...)
    {
      stop_source.request_stop();
    }
  };

  auto first_due = std::chrono::steady_clock::now() +
    std::chrono::duration_cast<std::chrono::steady_clock::duration>(initial_delay);

  enqueue_at_on(state_, first_due, detail::move_only_function{resource,
    [callback, first_due]
    {
      (*callback)(first_due);
    }});

  return periodic_handle{std::move(stop_source), callback};
}
} // namespace modern
