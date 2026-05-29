module;

#include "../detail/move_only_function_support.hpp"
#include "../detail/shared_ptr.hpp"

#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

export module modern.task.detail;

export import modern.exec;
export import modern.memory;

export namespace modern::detail
{
template<class T>
class shared_state
{
public:
  explicit shared_state(
    scheduler default_scheduler,
    memory::memory_resource* resource = memory::get_default_resource())
    : default_scheduler_(std::move(default_scheduler)),
      resource_(resource ? resource : memory::get_default_resource()),
      continuations_(resource_)
  {
  }

  scheduler default_scheduler() const
  {
    return default_scheduler_;
  }

  [[nodiscard]] memory::memory_resource* resource() const noexcept
  {
    return resource_;
  }

  void mark_consumed()
  {
    std::lock_guard lock(mutex_);

    if (consumed_)
      throw std::logic_error("task state was already consumed");

    consumed_ = true;
  }

  void set_value(T value)
  {
    std::pmr::vector<move_only_function> continuations{resource_};

    {
      std::lock_guard lock(mutex_);

      if (ready_)
        throw std::logic_error("task state was already completed");

      value_.emplace(std::move(value));
      ready_ = true;
      continuations = std::move(continuations_);
    }

    cv_.notify_all();
    run_all(std::move(continuations));
  }

  void set_exception(std::exception_ptr exception)
  {
    std::pmr::vector<move_only_function> continuations{resource_};

    {
      std::lock_guard lock(mutex_);

      if (ready_)
        throw std::logic_error("task state was already completed");

      exception_ = exception ? exception : std::make_exception_ptr(
        std::runtime_error("unknown exception"));
      ready_ = true;
      continuations = std::move(continuations_);
    }

    cv_.notify_all();
    run_all(std::move(continuations));
  }

  T take_value()
  {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return ready_; });

    if (exception_)
      std::rethrow_exception(exception_);

    return std::move(*value_);
  }

  void add_continuation(move_only_function continuation)
  {
    move_only_function run_now;

    {
      std::lock_guard lock(mutex_);

      if (!ready_)
      {
        continuations_.push_back(std::move(continuation));
        return;
      }

      run_now = std::move(continuation);
    }

    run_now();
  }

private:
  static void run_all(std::pmr::vector<move_only_function> continuations)
  {
    for (auto& continuation : continuations)
      continuation();
  }

  scheduler default_scheduler_;
  memory::memory_resource* resource_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool ready_ = false;
  bool consumed_ = false;
  std::optional<T> value_;
  std::exception_ptr exception_;
  std::pmr::vector<move_only_function> continuations_;
};

template<>
class shared_state<void>
{
public:
  explicit shared_state(
    scheduler default_scheduler,
    memory::memory_resource* resource = memory::get_default_resource())
    : default_scheduler_(std::move(default_scheduler)),
      resource_(resource ? resource : memory::get_default_resource()),
      continuations_(resource_)
  {
  }

  scheduler default_scheduler() const
  {
    return default_scheduler_;
  }

  [[nodiscard]] memory::memory_resource* resource() const noexcept
  {
    return resource_;
  }

  void mark_consumed()
  {
    std::lock_guard lock(mutex_);

    if (consumed_)
      throw std::logic_error("task state was already consumed");

    consumed_ = true;
  }

  void set_value()
  {
    std::pmr::vector<move_only_function> continuations{resource_};

    {
      std::lock_guard lock(mutex_);

      if (ready_)
        throw std::logic_error("task state was already completed");

      ready_ = true;
      continuations = std::move(continuations_);
    }

    cv_.notify_all();
    run_all(std::move(continuations));
  }

  void set_exception(std::exception_ptr exception)
  {
    std::pmr::vector<move_only_function> continuations{resource_};

    {
      std::lock_guard lock(mutex_);

      if (ready_)
        throw std::logic_error("task state was already completed");

      exception_ = exception ? exception : std::make_exception_ptr(
        std::runtime_error("unknown exception"));
      ready_ = true;
      continuations = std::move(continuations_);
    }

    cv_.notify_all();
    run_all(std::move(continuations));
  }

  void take_value()
  {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return ready_; });

    if (exception_)
      std::rethrow_exception(exception_);
  }

  void add_continuation(move_only_function continuation)
  {
    move_only_function run_now;

    {
      std::lock_guard lock(mutex_);

      if (!ready_)
      {
        continuations_.push_back(std::move(continuation));
        return;
      }

      run_now = std::move(continuation);
    }

    run_now();
  }

private:
  static void run_all(std::pmr::vector<move_only_function> continuations)
  {
    for (auto& continuation : continuations)
      continuation();
  }

  scheduler default_scheduler_;
  memory::memory_resource* resource_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool ready_ = false;
  bool consumed_ = false;
  std::exception_ptr exception_;
  std::pmr::vector<move_only_function> continuations_;
};

template<class R>
std::shared_ptr<shared_state<R>> make_shared_state(
  scheduler default_scheduler,
  memory::memory_resource* resource = memory::get_default_resource())
{
  auto* actual_resource = resource ? resource : memory::get_default_resource();

  return detail::allocate_shared_object<shared_state<R>>(
    actual_resource,
    std::move(default_scheduler),
    actual_resource);
}

template<class R, class F>
void fulfill(std::shared_ptr<shared_state<R>> state, F&& f)
{
  try
  {
    if constexpr (std::is_void_v<R>)
    {
      std::invoke(std::forward<F>(f));
      state->set_value();
    }
    else
    {
      state->set_value(std::invoke(std::forward<F>(f)));
    }
  }
  catch (...)
  {
    state->set_exception(std::current_exception());
  }
}

inline std::exception_ptr make_cancellation_exception()
{
  return std::make_exception_ptr(std::runtime_error("task cancelled"));
}

template<class R>
void cancel_state(const std::shared_ptr<shared_state<R>>& state)
{
  state->set_exception(make_cancellation_exception());
}
} // namespace modern::detail
