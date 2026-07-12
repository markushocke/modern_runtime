module;

#include "../detail/move_only_function_support.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

module modern.timer;

import :api;
import modern.exec;
import :state;

namespace modern::detail
{
void timer_state::timer_loop(std::stop_token stop_token)
{
  while (true)
  {
    move_only_function task;

    {
      std::unique_lock lock(mutex_);

      while (!stopping_ && !stop_token.stop_requested() && heap_.empty())
        cv_.wait(lock);

      if ((stopping_ || stop_token.stop_requested()) && heap_.empty())
        return;

      while (!heap_.empty())
      {
        auto due = heap_.front().due;

        if (cv_.wait_until(lock, due, [this, &stop_token, due]
            {
              return stopping_ || stop_token.stop_requested() || heap_.empty() || heap_.front().due != due;
            }))
        {
          if ((stopping_ || stop_token.stop_requested()) && heap_.empty())
            return;

          continue;
        }

        std::pop_heap(heap_.begin(), heap_.end(), item_is_later{});
        auto item = std::move(heap_.back());
        heap_.pop_back();
        task = std::move(item.task);
        break;
      }
    }

    if (task)
      task();
  }
}
} // namespace modern::detail

namespace modern
{
scheduled_executor::periodic_handle::periodic_handle() = default;

scheduled_executor::periodic_handle::periodic_handle(periodic_handle&&) noexcept = default;

scheduled_executor::periodic_handle& scheduled_executor::periodic_handle::operator=(periodic_handle&&) noexcept = default;

scheduled_executor::periodic_handle::~periodic_handle()
{
  request_stop();
}

void scheduled_executor::periodic_handle::request_stop() noexcept
{
  if (stop_source_.stop_possible())
    stop_source_.request_stop();
}

bool scheduled_executor::periodic_handle::stop_requested() const noexcept
{
  return stop_source_.stop_requested();
}
} // namespace modern
