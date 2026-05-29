module;

#include "../detail/move_only_function_support.hpp"

#include <chrono>
#include <concepts>
#include <memory>
#include <stop_token>
#include <utility>

export module modern.timer:api;

export import modern.exec;
export import modern.memory;
export import modern.thread;

namespace modern::detail
{
class timer_state;
}

export namespace modern
{
class scheduled_executor
{
public:
  explicit scheduled_executor(
    scheduler target,
    memory::memory_resource* resource = memory::get_default_resource());

  explicit scheduled_executor(thread_pool& pool);

  scheduled_executor(thread_pool& pool, memory::memory_resource* resource);

  scheduled_executor(const scheduled_executor&) = delete;
  scheduled_executor& operator=(const scheduled_executor&) = delete;

  scheduled_executor(scheduled_executor&&) noexcept = default;
  scheduled_executor& operator=(scheduled_executor&&) noexcept = default;

  ~scheduled_executor();

  scheduler target_scheduler() const;

  [[nodiscard]] memory::memory_resource* resource() const noexcept;

  template<class Clock, class Duration, class F>
  void schedule_at(std::chrono::time_point<Clock, Duration> due, F&& f);

  template<class Clock, class Duration, class F, class OnCancel>
  void schedule_at(std::chrono::time_point<Clock, Duration> due, F&& f, OnCancel&& on_cancel);

  template<class Rep, class Period, class F>
  void schedule_after(std::chrono::duration<Rep, Period> delay, F&& f);

  template<class Rep, class Period, class F, class OnCancel>
  void schedule_after(std::chrono::duration<Rep, Period> delay, F&& f, OnCancel&& on_cancel);

  class periodic_handle
  {
  public:
    periodic_handle();

    periodic_handle(std::stop_source stop_source, std::shared_ptr<void> keep_alive)
      : stop_source_(std::move(stop_source)), keep_alive_(std::move(keep_alive))
    {
    }

    periodic_handle(periodic_handle&&) noexcept;
    periodic_handle& operator=(periodic_handle&&) noexcept;

    periodic_handle(const periodic_handle&) = delete;
    periodic_handle& operator=(const periodic_handle&) = delete;

    ~periodic_handle();

    void request_stop() noexcept;

    [[nodiscard]] bool stop_requested() const noexcept;

  private:
    std::stop_source stop_source_;
    std::shared_ptr<void> keep_alive_;
  };

  template<class InitialRep, class InitialPeriod,
           class PeriodRep, class PeriodPeriod,
           class F>
  periodic_handle schedule_fixed_rate(
    std::chrono::duration<InitialRep, InitialPeriod> initial_delay,
    std::chrono::duration<PeriodRep, PeriodPeriod> period,
    F&& f);

  void shutdown() noexcept;

  void join();

private:
  template<class Clock, class Duration>
  static std::chrono::steady_clock::time_point to_steady_time(
    std::chrono::time_point<Clock, Duration> time)
  {
    if constexpr (std::same_as<Clock, std::chrono::steady_clock>)
    {
      return std::chrono::time_point_cast<std::chrono::steady_clock::duration>(time);
    }
    else
    {
      auto now_clock = Clock::now();
      auto now_steady = std::chrono::steady_clock::now();
      return now_steady + std::chrono::duration_cast<std::chrono::steady_clock::duration>(time - now_clock);
    }
  }

  static scheduler target_scheduler_of(const std::shared_ptr<detail::timer_state>& state);

  static memory::memory_resource* resource_of(const std::shared_ptr<detail::timer_state>& state) noexcept;

  static void enqueue_at_on(
    const std::shared_ptr<detail::timer_state>& state,
    std::chrono::steady_clock::time_point due,
    detail::move_only_function task,
    detail::move_only_function on_cancel = {});

  std::shared_ptr<detail::timer_state> state_;
};
} // namespace modern
