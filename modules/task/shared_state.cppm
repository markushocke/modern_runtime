module;

#include "../detail/move_only_function_support.hpp"
#include "../detail/shared_ptr.hpp"

#include <condition_variable>
#include <coroutine>
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

export module modern.task_detail;

export import modern.exec;
export import modern.memory;
export import modern.task_environment;

export namespace modern
{
class operation_cancelled : public std::runtime_error
{
public:
  operation_cancelled()
    : std::runtime_error("operation cancelled")
  {
  }
};
}

export namespace modern::detail
{
template<class T>
class shared_state
{
public:
  enum class status
  {
    pending,
    value,
    exception,
    stopped
  };

  inline explicit shared_state(
    scheduler default_scheduler,
    memory::memory_resource* resource = memory::get_default_resource(),
    std::shared_ptr<modern::task_environment> env = nullptr)
    : default_scheduler_(std::move(default_scheduler)),
      resource_(resource ? resource : memory::get_default_resource()),
      continuations_(resource_),
      environment_(std::move(env))
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

  [[nodiscard]] const modern::task_environment* environment() const noexcept
  {
    return environment_.get();
  }

  [[nodiscard]] modern::task_environment* environment() noexcept
  {
    return environment_.get();
  }

  void set_environment(std::shared_ptr<modern::task_environment> env) noexcept
  {
    environment_ = std::move(env);
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
    std::vector<std::coroutine_handle<>> awaiters;

    {
      std::lock_guard lock(mutex_);

      if (status_ != status::pending)
        throw std::logic_error("task state was already completed");

      value_.emplace(std::move(value));
      status_ = status::value;
      continuations = std::move(continuations_);
      awaiters = std::move(awaiters_);
    }

    cv_.notify_all();
    run_all(std::move(continuations));
    for (auto& handle : awaiters)
      handle.resume();
  }

  void set_exception(std::exception_ptr exception)
  {
    std::pmr::vector<move_only_function> continuations{resource_};
    std::vector<std::coroutine_handle<>> awaiters;

    {
      std::lock_guard lock(mutex_);

      if (status_ != status::pending)
        throw std::logic_error("task state was already completed");

      exception_ = exception ? exception : std::make_exception_ptr(
        std::runtime_error("unknown exception"));
      status_ = status::exception;
      continuations = std::move(continuations_);
      awaiters = std::move(awaiters_);
    }

    cv_.notify_all();
    run_all(std::move(continuations));
    for (auto& handle : awaiters)
      handle.resume();
  }

  void set_stopped()
  {
    std::pmr::vector<move_only_function> continuations{resource_};
    std::vector<std::coroutine_handle<>> awaiters;

    {
      std::lock_guard lock(mutex_);

      if (status_ != status::pending)
        throw std::logic_error("task state was already completed");

      status_ = status::stopped;
      continuations = std::move(continuations_);
      awaiters = std::move(awaiters_);
    }

    cv_.notify_all();
    run_all(std::move(continuations));
    for (auto& handle : awaiters)
      handle.resume();
  }

  [[nodiscard]] bool done() const noexcept
  {
    std::lock_guard lock(mutex_);
    return status_ != status::pending;
  }

  [[nodiscard]] bool stopped() const noexcept
  {
    std::lock_guard lock(mutex_);
    return status_ == status::stopped;
  }

  T take_value()
  {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return status_ != status::pending; });

    if (status_ == status::stopped)
      throw operation_cancelled{};

    if (exception_)
      std::rethrow_exception(exception_);

    return std::move(*value_);
  }

  void add_continuation(move_only_function continuation)
  {
    move_only_function run_now;

    {
      std::lock_guard lock(mutex_);

      if (status_ == status::pending)
      {
        continuations_.push_back(std::move(continuation));
        return;
      }

      run_now = std::move(continuation);
    }

    run_now();
  }

  void add_awaiter(std::coroutine_handle<> handle)
  {
    bool run_now = false;

    {
      std::lock_guard lock(mutex_);

      if (status_ == status::pending)
      {
        awaiters_.push_back(handle);
        return;
      }

      run_now = true;
    }

    if (run_now)
      handle.resume();
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
  status status_ = status::pending;
  bool consumed_ = false;
  std::optional<T> value_;
  std::exception_ptr exception_;
  std::pmr::vector<move_only_function> continuations_;
  std::vector<std::coroutine_handle<>> awaiters_;
  std::shared_ptr<modern::task_environment> environment_;
};

template<>
class shared_state<void>
{
public:
  inline explicit shared_state(
    scheduler default_scheduler,
    memory::memory_resource* resource = memory::get_default_resource(),
    std::shared_ptr<modern::task_environment> env = nullptr)
    : default_scheduler_(std::move(default_scheduler)),
      resource_(resource ? resource : memory::get_default_resource()),
      continuations_(resource_),
      environment_(std::move(env))
  {}

  scheduler default_scheduler() const
  {
    return default_scheduler_;
  }

  [[nodiscard]] memory::memory_resource* resource() const noexcept
  {
    return resource_;
  }

  [[nodiscard]] const modern::task_environment* environment() const noexcept
  {
    return environment_.get();
  }

  [[nodiscard]] modern::task_environment* environment() noexcept
  {
    return environment_.get();
  }

  void set_environment(std::shared_ptr<modern::task_environment> env) noexcept
  {
    environment_ = std::move(env);
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
    std::vector<std::coroutine_handle<>> awaiters;

    {
      std::lock_guard lock(mutex_);

      if (ready_)
        throw std::logic_error("task state was already completed");

      ready_ = true;
      continuations = std::move(continuations_);
      awaiters = std::move(awaiters_);
    }

    cv_.notify_all();
    run_all(std::move(continuations));
    for (auto& handle : awaiters)
      handle.resume();
  }

  void set_exception(std::exception_ptr exception)
  {
    std::pmr::vector<move_only_function> continuations{resource_};
    std::vector<std::coroutine_handle<>> awaiters;

    {
      std::lock_guard lock(mutex_);

      if (ready_)
        throw std::logic_error("task state was already completed");

      exception_ = exception ? exception : std::make_exception_ptr(
        std::runtime_error("unknown exception"));
      ready_ = true;
      continuations = std::move(continuations_);
      awaiters = std::move(awaiters_);
    }

    cv_.notify_all();
    run_all(std::move(continuations));
    for (auto& handle : awaiters)
      handle.resume();
  }

  void set_stopped()
  {
    std::pmr::vector<move_only_function> continuations{resource_};
    std::vector<std::coroutine_handle<>> awaiters;

    {
      std::lock_guard lock(mutex_);

      if (ready_)
        throw std::logic_error("task state was already completed");

      ready_ = true;
      stopped_ = true;
      continuations = std::move(continuations_);
      awaiters = std::move(awaiters_);
    }

    cv_.notify_all();
    run_all(std::move(continuations));
    for (auto& handle : awaiters)
      handle.resume();
  }

  void take_value()
  {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return ready_; });

    if (stopped_)
      throw operation_cancelled{};

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

  void add_awaiter(std::coroutine_handle<> handle)
  {
    bool run_now = false;

    {
      std::lock_guard lock(mutex_);

      if (!ready_)
      {
        awaiters_.push_back(handle);
        return;
      }

      run_now = true;
    }

    if (run_now)
      handle.resume();
  }

  [[nodiscard]] bool stopped() const noexcept
  {
    std::lock_guard lock(mutex_);
    return stopped_;
  }

  [[nodiscard]] bool done() const noexcept
  {
    std::lock_guard lock(mutex_);
    return ready_;
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
  bool stopped_ = false;
  bool consumed_ = false;
  std::exception_ptr exception_;
  std::pmr::vector<move_only_function> continuations_;
  std::vector<std::coroutine_handle<>> awaiters_;
  std::shared_ptr<modern::task_environment> environment_;
};

template<class R>
std::shared_ptr<shared_state<R>> make_shared_state(
  scheduler default_scheduler,
  memory::memory_resource* resource = memory::get_default_resource(),
  std::shared_ptr<modern::task_environment> env = nullptr)
{
  auto* actual_resource = resource ? resource : memory::get_default_resource();
  if (!env)
  {
    auto inherited = current_task_environment_value();
    inherited.scheduler = default_scheduler.valid() ? default_scheduler : inherited.scheduler;
    inherited.frame_resource = actual_resource;
    env = std::make_shared<modern::task_environment>(std::move(inherited));
  }

  return detail::allocate_shared_object<shared_state<R>>(
    actual_resource,
    std::move(default_scheduler),
    actual_resource,
    std::move(env));
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
  return std::make_exception_ptr(operation_cancelled{});
}

template<class R>
void cancel_state(const std::shared_ptr<shared_state<R>>& state)
{
  state->set_stopped();
}

} // namespace modern::detail
