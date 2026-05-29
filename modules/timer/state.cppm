module;

#include "../detail/move_only_function_support.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

module modern.timer:state;

import modern.exec;

namespace modern::detail
{
class timer_state
{
public:
  explicit timer_state(
    scheduler target,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

  ~timer_state();

  scheduler target_scheduler() const;

  [[nodiscard]] std::pmr::memory_resource* resource() const noexcept;

  void start();

  void enqueue_at(
    std::chrono::steady_clock::time_point due,
    move_only_function task,
    move_only_function on_cancel = {});

  void shutdown() noexcept;

  void join();

private:
  struct timer_item
  {
    std::chrono::steady_clock::time_point due;
    std::uint64_t sequence;
    move_only_function task;
    move_only_function on_cancel;
  };

  struct item_is_later
  {
    bool operator()(const timer_item& lhs, const timer_item& rhs) const noexcept
    {
      if (lhs.due == rhs.due)
        return lhs.sequence > rhs.sequence;

      return lhs.due > rhs.due;
    }
  };

  void timer_loop(std::stop_token stop_token);

  scheduler target_;
  std::pmr::memory_resource* resource_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stopping_ = false;
  std::uint64_t sequence_ = 0;
  std::pmr::vector<timer_item> heap_;
  std::jthread timer_thread_;
};
} // namespace modern::detail
