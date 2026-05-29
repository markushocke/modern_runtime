module;

#include "../detail/move_only_function_support.hpp"
#include "../detail/shared_ptr.hpp"

#include <atomic>
#include <condition_variable>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>

export module modern.task:continuation;

export import modern.exec;
export import modern.memory;
import modern.task.detail;

namespace modern::detail
{
class task_scope_state
{
public:
  explicit task_scope_state(scheduler executor, memory::memory_resource* resource) noexcept
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

  void add_child()
  {
    std::lock_guard lock(mutex_);
    ++active_children_;
  }

  void child_completed() noexcept
  {
    std::lock_guard lock(mutex_);

    if (active_children_ > 0)
      --active_children_;

    if (active_children_ == 0)
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
        return active_children_ == 0;
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
  std::size_t active_children_ = 0;
  std::exception_ptr exception_;
};

template<class F, class... Args>
struct stoppable_submit_result;

template<class F, class... Args>
  requires std::is_invocable_v<F&, std::stop_token, Args...>
struct stoppable_submit_result<F, Args...>
{
  using type = std::invoke_result_t<F&, std::stop_token, Args...>;
  static constexpr bool uses_stop_token = true;
};

template<class F, class... Args>
  requires (!std::is_invocable_v<F&, std::stop_token, Args...> && std::is_invocable_v<F&, Args...>)
struct stoppable_submit_result<F, Args...>
{
  using type = std::invoke_result_t<F&, Args...>;
  static constexpr bool uses_stop_token = false;
};
}

export namespace modern
{

class task_scope
{
public:
  explicit task_scope(scheduler executor, memory::memory_resource* resource = memory::get_default_resource())
    : state_(detail::allocate_shared_object<detail::task_scope_state>(
        resource ? resource : memory::get_default_resource(),
        std::move(executor),
        resource ? resource : memory::get_default_resource()))
  {
  }

  task_scope(task_scope&&) noexcept = default;
  task_scope& operator=(task_scope&&) noexcept = default;

  task_scope(const task_scope&) = delete;
  task_scope& operator=(const task_scope&) = delete;

  ~task_scope()
  {
    if (state_)
    {
      state_->request_stop();
      state_->join_noexcept();
    }
  }

  template<class F, class... Args>
  void spawn(F&& f, Args&&... args)
  {
    using callable_type = std::remove_cvref_t<F>;
    auto state = state_or_throw();
    auto token = state->token();
    auto executor = state->executor();

    state->add_child();

    try
    {
      auto callable = callable_type(std::forward<F>(f));
      auto arguments = std::make_tuple(std::remove_cvref_t<Args>(std::forward<Args>(args))...);

      executor.execute([state,
                        token,
                        callable = std::move(callable),
                        arguments = std::move(arguments)]() mutable
      {
        struct child_guard final
        {
          std::shared_ptr<detail::task_scope_state> state;

          ~child_guard()
          {
            state->child_completed();
          }
        } guard{state};

        if (token.stop_requested())
          return;

        try
        {
          if constexpr (std::is_invocable_v<callable_type&, std::stop_token, std::remove_cvref_t<Args>&...>)
          {
            std::apply(
              [&](auto&&... unpacked)
              {
                std::invoke(callable, token, std::move(unpacked)...);
              },
              std::move(arguments));
          }
          else
          {
            std::apply(callable, std::move(arguments));
          }
        }
        catch (...)
        {
          state->record_exception(std::current_exception());
        }
      });
    }
    catch (...)
    {
      state->child_completed();
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
  std::shared_ptr<detail::task_scope_state> state_or_throw() const
  {
    if (!state_)
      throw std::logic_error("invalid task_scope");

    return state_;
  }

  std::shared_ptr<detail::task_scope_state> state_;
};

template<class T>
class task;

class task_scope;

} // namespace modern

export namespace modern::detail
{
template<class T>
task<T> make_task(std::shared_ptr<void> state);
}

export namespace modern
{

template<class T>
class task
{
public:
  using value_type = T;

  class promise_type
  {
  public:
    promise_type()
      : state_(detail::make_shared_state<T>(inline_scheduler(), memory::get_default_resource()))
    {
    }

    task<T> get_return_object()
    {
      return task<T>{state_};
    }

    std::suspend_never initial_suspend() const noexcept
    {
      return {};
    }

    std::suspend_never final_suspend() const noexcept
    {
      return {};
    }

    template<class U>
      requires std::convertible_to<U, T>
    void return_value(U&& value)
    {
      state_->set_value(T(std::forward<U>(value)));
    }

    void unhandled_exception()
    {
      state_->set_exception(std::current_exception());
    }

  private:
    std::shared_ptr<detail::shared_state<T>> state_;
  };

  struct awaiter
  {
    std::shared_ptr<detail::shared_state<T>> state;

    [[nodiscard]] bool await_ready() const noexcept
    {
      return false;
    }

    void await_suspend(std::coroutine_handle<> handle)
    {
      state->add_continuation(detail::move_only_function{state->resource(), [handle]() mutable
      {
        handle.resume();
      }});
    }

    T await_resume()
    {
      return state->take_value();
    }
  };

  task() = default;

  task(task&&) noexcept = default;
  task& operator=(task&&) noexcept = default;

  task(const task&) = delete;
  task& operator=(const task&) = delete;

  [[nodiscard]] bool valid() const noexcept
  {
    return static_cast<bool>(state_);
  }

  T get()
  {
    auto state = consume_state();
    return state->take_value();
  }

  void detach() noexcept
  {
    state_.reset();
  }

  awaiter operator co_await() &&
  {
    return awaiter{consume_state()};
  }

  awaiter operator co_await() & = delete;

  template<class F>
  auto then(F&& f) &&
  {
    return std::move(*this).then_on(state_or_throw()->default_scheduler(), nullptr, std::forward<F>(f));
  }

  template<class F>
  auto then(memory::memory_resource* resource, F&& f) &&
  {
    return std::move(*this).then_on(state_or_throw()->default_scheduler(), resource, std::forward<F>(f));
  }

  template<class F>
  auto then(F&&) & = delete;

  template<class F>
  auto then(memory::memory_resource*, F&&) & = delete;

  template<class F>
  auto then_on(scheduler executor, F&& f) &&
  {
    return std::move(*this).then_on(std::move(executor), nullptr, std::forward<F>(f));
  }

  template<class F>
  auto then_on(scheduler executor, memory::memory_resource* resource, F&& f) &&
  {
    using R = std::invoke_result_t<std::remove_cvref_t<F>&, T>;

    auto parent = consume_state();
    auto* continuation_resource = resource ? resource : parent->resource();
    auto child = detail::make_shared_state<R>(executor, continuation_resource);
    auto result = detail::make_task<R>(child);

    parent->add_continuation(detail::move_only_function{continuation_resource,
      [parent, child, executor, fn = std::remove_cvref_t<F>(std::forward<F>(f))]() mutable
      {
        try
        {
          executor.execute([parent, child, fn = std::move(fn)]() mutable
          {
            detail::fulfill<R>(child, [&]() mutable -> R
            {
              T value = parent->take_value();

              if constexpr (std::is_void_v<R>)
              {
                std::invoke(fn, std::move(value));
              }
              else
              {
                return std::invoke(fn, std::move(value));
              }
            });
          });
        }
        catch (...)
        {
          child->set_exception(std::current_exception());
        }
      }});

    return result;
  }

  template<class F>
  auto then_on(scheduler, F&&) & = delete;

  template<class F>
  auto then_on(scheduler, memory::memory_resource*, F&&) & = delete;

  template<class F>
  task<T> catching(F&& f) &&
  {
    return std::move(*this).catching_on(state_or_throw()->default_scheduler(), nullptr, std::forward<F>(f));
  }

  template<class F>
  task<T> catching(memory::memory_resource* resource, F&& f) &&
  {
    return std::move(*this).catching_on(state_or_throw()->default_scheduler(), resource, std::forward<F>(f));
  }

  template<class F>
  task<T> catching(F&&) & = delete;

  template<class F>
  task<T> catching(memory::memory_resource*, F&&) & = delete;

  template<class F>
  task<T> catching_on(scheduler executor, F&& f) &&
  {
    return std::move(*this).catching_on(std::move(executor), nullptr, std::forward<F>(f));
  }

  template<class F>
  task<T> catching_on(scheduler executor, memory::memory_resource* resource, F&& f) &&
  {
    using handler_result = std::invoke_result_t<std::remove_cvref_t<F>&, std::exception_ptr>;
    static_assert(std::same_as<handler_result, T>,
      "catching handler for task<T> must return T");

    auto parent = consume_state();
    auto* continuation_resource = resource ? resource : parent->resource();
    auto child = detail::make_shared_state<T>(executor, continuation_resource);
    auto result = detail::make_task<T>(child);

    parent->add_continuation(detail::move_only_function{continuation_resource,
      [parent, child, executor, fn = std::remove_cvref_t<F>(std::forward<F>(f))]() mutable
      {
        try
        {
          executor.execute([parent, child, fn = std::move(fn)]() mutable
          {
            try
            {
              child->set_value(parent->take_value());
            }
            catch (...)
            {
              detail::fulfill<T>(child, [&]() mutable -> T
              {
                return std::invoke(fn, std::current_exception());
              });
            }
          });
        }
        catch (...)
        {
          child->set_exception(std::current_exception());
        }
      }});

    return result;
  }

  template<class F>
  task<T> catching_on(scheduler, F&&) & = delete;

  template<class F>
  task<T> catching_on(scheduler, memory::memory_resource*, F&&) & = delete;

  template<class F>
  task<T> finally(F&& f) &&
  {
    return std::move(*this).finally_on(state_or_throw()->default_scheduler(), nullptr, std::forward<F>(f));
  }

  template<class F>
  task<T> finally(memory::memory_resource* resource, F&& f) &&
  {
    return std::move(*this).finally_on(state_or_throw()->default_scheduler(), resource, std::forward<F>(f));
  }

  template<class F>
  task<T> finally(F&&) & = delete;

  template<class F>
  task<T> finally(memory::memory_resource*, F&&) & = delete;

  template<class F>
  task<T> finally_on(scheduler executor, F&& f) &&
  {
    return std::move(*this).finally_on(std::move(executor), nullptr, std::forward<F>(f));
  }

  template<class F>
  task<T> finally_on(scheduler executor, memory::memory_resource* resource, F&& f) &&
  {
    using finally_result = std::invoke_result_t<std::remove_cvref_t<F>&>;
    static_assert(std::is_void_v<finally_result>,
      "finally handler must return void");

    auto parent = consume_state();
    auto* continuation_resource = resource ? resource : parent->resource();
    auto child = detail::make_shared_state<T>(executor, continuation_resource);
    auto result = detail::make_task<T>(child);

    parent->add_continuation(detail::move_only_function{continuation_resource,
      [parent, child, executor, fn = std::remove_cvref_t<F>(std::forward<F>(f))]() mutable
      {
        try
        {
          executor.execute([parent, child, fn = std::move(fn)]() mutable
          {
            std::optional<T> value;
            std::exception_ptr original_exception;

            try
            {
              value.emplace(parent->take_value());
            }
            catch (...)
            {
              original_exception = std::current_exception();
            }

            try
            {
              std::invoke(fn);
            }
            catch (...)
            {
              child->set_exception(std::current_exception());
              return;
            }

            if (original_exception)
              child->set_exception(original_exception);
            else
              child->set_value(std::move(*value));
          });
        }
        catch (...)
        {
          child->set_exception(std::current_exception());
        }
      }});

    return result;
  }

  template<class F>
  task<T> finally_on(scheduler, F&&) & = delete;

  template<class F>
  task<T> finally_on(scheduler, memory::memory_resource*, F&&) & = delete;

private:
  template<class U>
  friend task<U> detail::make_task(std::shared_ptr<void> state);

  explicit task(std::shared_ptr<detail::shared_state<T>> state)
    : state_(std::move(state))
  {
  }

  std::shared_ptr<detail::shared_state<T>> state_or_throw() const
  {
    if (!state_)
      throw std::logic_error("invalid task");

    return state_;
  }

  std::shared_ptr<detail::shared_state<T>> consume_state()
  {
    auto state = state_or_throw();
    state->mark_consumed();
    state_.reset();
    return state;
  }

  std::shared_ptr<detail::shared_state<T>> state_;
};

template<>
class task<void>
{
public:
  using value_type = void;

  class promise_type
  {
  public:
    promise_type()
      : state_(detail::make_shared_state<void>(inline_scheduler(), memory::get_default_resource()))
    {
    }

    task<void> get_return_object()
    {
      return task<void>{state_};
    }

    std::suspend_never initial_suspend() const noexcept
    {
      return {};
    }

    std::suspend_never final_suspend() const noexcept
    {
      return {};
    }

    void return_void()
    {
      state_->set_value();
    }

    void unhandled_exception()
    {
      state_->set_exception(std::current_exception());
    }

  private:
    std::shared_ptr<detail::shared_state<void>> state_;
  };

  struct awaiter
  {
    std::shared_ptr<detail::shared_state<void>> state;

    [[nodiscard]] bool await_ready() const noexcept
    {
      return false;
    }

    void await_suspend(std::coroutine_handle<> handle)
    {
      state->add_continuation(detail::move_only_function{state->resource(), [handle]() mutable
      {
        handle.resume();
      }});
    }

    void await_resume()
    {
      state->take_value();
    }
  };

  task() = default;

  task(task&&) noexcept = default;
  task& operator=(task&&) noexcept = default;

  task(const task&) = delete;
  task& operator=(const task&) = delete;

  [[nodiscard]] bool valid() const noexcept
  {
    return static_cast<bool>(state_);
  }

  void get()
  {
    auto state = consume_state();
    state->take_value();
  }

  void detach() noexcept
  {
    state_.reset();
  }

  awaiter operator co_await() &&
  {
    return awaiter{consume_state()};
  }

  awaiter operator co_await() & = delete;

  template<class F>
  auto then(F&& f) &&
  {
    return std::move(*this).then_on(state_or_throw()->default_scheduler(), nullptr, std::forward<F>(f));
  }

  template<class F>
  auto then(memory::memory_resource* resource, F&& f) &&
  {
    return std::move(*this).then_on(state_or_throw()->default_scheduler(), resource, std::forward<F>(f));
  }

  template<class F>
  auto then(F&&) & = delete;

  template<class F>
  auto then(memory::memory_resource*, F&&) & = delete;

  template<class F>
  auto then_on(scheduler executor, F&& f) &&
  {
    return std::move(*this).then_on(std::move(executor), nullptr, std::forward<F>(f));
  }

  template<class F>
  auto then_on(scheduler executor, memory::memory_resource* resource, F&& f) &&
  {
    using R = std::invoke_result_t<std::remove_cvref_t<F>&>;

    auto parent = consume_state();
    auto* continuation_resource = resource ? resource : parent->resource();
    auto child = detail::make_shared_state<R>(executor, continuation_resource);
    auto result = detail::make_task<R>(child);

    parent->add_continuation(detail::move_only_function{continuation_resource,
      [parent, child, executor, fn = std::remove_cvref_t<F>(std::forward<F>(f))]() mutable
      {
        try
        {
          executor.execute([parent, child, fn = std::move(fn)]() mutable
          {
            detail::fulfill<R>(child, [&]() mutable -> R
            {
              parent->take_value();

              if constexpr (std::is_void_v<R>)
              {
                std::invoke(fn);
              }
              else
              {
                return std::invoke(fn);
              }
            });
          });
        }
        catch (...)
        {
          child->set_exception(std::current_exception());
        }
      }});

    return result;
  }

  template<class F>
  auto then_on(scheduler, F&&) & = delete;

  template<class F>
  auto then_on(scheduler, memory::memory_resource*, F&&) & = delete;

  template<class F>
  task<void> catching(F&& f) &&
  {
    return std::move(*this).catching_on(state_or_throw()->default_scheduler(), nullptr, std::forward<F>(f));
  }

  template<class F>
  task<void> catching(memory::memory_resource* resource, F&& f) &&
  {
    return std::move(*this).catching_on(state_or_throw()->default_scheduler(), resource, std::forward<F>(f));
  }

  template<class F>
  task<void> catching(F&&) & = delete;

  template<class F>
  task<void> catching(memory::memory_resource*, F&&) & = delete;

  template<class F>
  task<void> catching_on(scheduler executor, F&& f) &&
  {
    return std::move(*this).catching_on(std::move(executor), nullptr, std::forward<F>(f));
  }

  template<class F>
  task<void> catching_on(scheduler executor, memory::memory_resource* resource, F&& f) &&
  {
    using handler_result = std::invoke_result_t<std::remove_cvref_t<F>&, std::exception_ptr>;
    static_assert(std::is_void_v<handler_result>,
      "catching handler for task<void> must return void");

    auto parent = consume_state();
    auto* continuation_resource = resource ? resource : parent->resource();
    auto child = detail::make_shared_state<void>(executor, continuation_resource);
    auto result = detail::make_task<void>(child);

    parent->add_continuation(detail::move_only_function{continuation_resource,
      [parent, child, executor, fn = std::remove_cvref_t<F>(std::forward<F>(f))]() mutable
      {
        try
        {
          executor.execute([parent, child, fn = std::move(fn)]() mutable
          {
            try
            {
              parent->take_value();
              child->set_value();
            }
            catch (...)
            {
              detail::fulfill<void>(child, [&]() mutable
              {
                std::invoke(fn, std::current_exception());
              });
            }
          });
        }
        catch (...)
        {
          child->set_exception(std::current_exception());
        }
      }});

    return result;
  }

  template<class F>
  task<void> catching_on(scheduler, F&&) & = delete;

  template<class F>
  task<void> catching_on(scheduler, memory::memory_resource*, F&&) & = delete;

  template<class F>
  task<void> finally(F&& f) &&
  {
    return std::move(*this).finally_on(state_or_throw()->default_scheduler(), nullptr, std::forward<F>(f));
  }

  template<class F>
  task<void> finally(memory::memory_resource* resource, F&& f) &&
  {
    return std::move(*this).finally_on(state_or_throw()->default_scheduler(), resource, std::forward<F>(f));
  }

  template<class F>
  task<void> finally(F&&) & = delete;

  template<class F>
  task<void> finally(memory::memory_resource*, F&&) & = delete;

  template<class F>
  task<void> finally_on(scheduler executor, F&& f) &&
  {
    return std::move(*this).finally_on(std::move(executor), nullptr, std::forward<F>(f));
  }

  template<class F>
  task<void> finally_on(scheduler executor, memory::memory_resource* resource, F&& f) &&
  {
    using finally_result = std::invoke_result_t<std::remove_cvref_t<F>&>;
    static_assert(std::is_void_v<finally_result>,
      "finally handler must return void");

    auto parent = consume_state();
    auto* continuation_resource = resource ? resource : parent->resource();
    auto child = detail::make_shared_state<void>(executor, continuation_resource);
    auto result = detail::make_task<void>(child);

    parent->add_continuation(detail::move_only_function{continuation_resource,
      [parent, child, executor, fn = std::remove_cvref_t<F>(std::forward<F>(f))]() mutable
      {
        try
        {
          executor.execute([parent, child, fn = std::move(fn)]() mutable
          {
            std::exception_ptr original_exception;

            try
            {
              parent->take_value();
            }
            catch (...)
            {
              original_exception = std::current_exception();
            }

            try
            {
              std::invoke(fn);
            }
            catch (...)
            {
              child->set_exception(std::current_exception());
              return;
            }

            if (original_exception)
              child->set_exception(original_exception);
            else
              child->set_value();
          });
        }
        catch (...)
        {
          child->set_exception(std::current_exception());
        }
      }});

    return result;
  }

  template<class F>
  task<void> finally_on(scheduler, F&&) & = delete;

  template<class F>
  task<void> finally_on(scheduler, memory::memory_resource*, F&&) & = delete;

private:
  template<class U>
  friend task<U> detail::make_task(std::shared_ptr<void> state);

  explicit task(std::shared_ptr<detail::shared_state<void>> state)
    : state_(std::move(state))
  {
  }

  std::shared_ptr<detail::shared_state<void>> state_or_throw() const
  {
    if (!state_)
      throw std::logic_error("invalid task");

    return state_;
  }

  std::shared_ptr<detail::shared_state<void>> consume_state()
  {
    auto state = state_or_throw();
    state->mark_consumed();
    state_.reset();
    return state;
  }

  std::shared_ptr<detail::shared_state<void>> state_;
};

} // namespace modern

export namespace modern::detail
{
template<class T>
task<T> make_task(std::shared_ptr<void> state)
{
  return task<T>{std::static_pointer_cast<shared_state<T>>(std::move(state))};
}
}

export namespace modern
{
template<class F, class... Args>
auto submit(scheduler executor, memory::memory_resource* resource, F&& f, Args&&... args)
{
  using R = std::invoke_result_t<std::remove_cvref_t<F>&, std::remove_cvref_t<Args>&...>;

  auto result_state = detail::make_shared_state<R>(executor, resource);
  auto result = detail::make_task<R>(result_state);

  auto callable = std::remove_cvref_t<F>(std::forward<F>(f));
  auto arguments = std::make_tuple(std::remove_cvref_t<Args>(std::forward<Args>(args))...);

  executor.execute([result_state,
                    callable = std::move(callable),
                    arguments = std::move(arguments)]() mutable
  {
    detail::fulfill<R>(result_state, [&]() mutable -> R
    {
      if constexpr (std::is_void_v<R>)
      {
        std::apply(callable, std::move(arguments));
      }
      else
      {
        return std::apply(callable, std::move(arguments));
      }
    });
  });

  return result;
}

template<class F, class... Args>
auto submit(scheduler executor, std::stop_token token, memory::memory_resource* resource, F&& f, Args&&... args)
{
  using callable_type = std::remove_cvref_t<F>;
  using result_traits = detail::stoppable_submit_result<callable_type, std::remove_cvref_t<Args>...>;
  using R = typename result_traits::type;

  auto* actual_resource = resource ? resource : memory::get_default_resource();
  auto result_state = detail::make_shared_state<R>(executor, actual_resource);
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

  auto callable = callable_type(std::forward<F>(f));
  auto arguments = std::make_tuple(std::remove_cvref_t<Args>(std::forward<Args>(args))...);

  executor.execute([result_state,
                    started,
                    callback,
                    token,
                    callable = std::move(callable),
                    arguments = std::move(arguments)]() mutable
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
          std::apply(
            [&](auto&&... unpacked)
            {
              std::invoke(callable, token, std::move(unpacked)...);
            },
            std::move(arguments));
        }
        else
        {
          return std::apply(
            [&](auto&&... unpacked) -> R
            {
              return std::invoke(callable, token, std::move(unpacked)...);
            },
            std::move(arguments));
        }
      }
      else
      {
        if constexpr (std::is_void_v<R>)
        {
          std::apply(callable, std::move(arguments));
        }
        else
        {
          return std::apply(callable, std::move(arguments));
        }
      }
    });
  });

  return result;
}

template<class F, class... Args>
  requires (!std::convertible_to<std::remove_cvref_t<F>, memory::memory_resource*>)
auto submit(scheduler executor, F&& f, Args&&... args)
{
  return modern::submit(
    executor,
    memory::get_default_resource(),
    std::forward<F>(f),
    std::forward<Args>(args)...);
}

template<class F, class... Args>
auto submit(scheduler executor, std::stop_token token, F&& f, Args&&... args)
{
  return modern::submit(
    executor,
    token,
    memory::get_default_resource(),
    std::forward<F>(f),
    std::forward<Args>(args)...);
}
} // namespace modern