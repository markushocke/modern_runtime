module;

#include "../detail/move_only_function_support.hpp"
#include "../detail/shared_ptr.hpp"

#include <atomic>
#include <condition_variable>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>

export module modern.task_continuation;

export import modern.exec;
export import modern.memory;
export import modern.task_environment;
export import modern.task_detail;

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
} // namespace modern::detail

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

template<class T>
struct is_task : std::false_type {};

template<class T>
struct is_task<task<T>> : std::true_type {};

template<class T>
inline constexpr bool is_task_v = is_task<std::remove_cvref_t<T>>::value;

template<class T>
struct task_unwrap
{
  using type = T;
};

template<class T>
struct task_unwrap<task<T>>
{
  using type = T;
};

template<class T>
using task_unwrap_t = typename task_unwrap<std::remove_cvref_t<T>>::type;

template<class T, class F>
struct continuation_result
{
  using raw_type = std::invoke_result_t<F&, T>;
  using type = task_unwrap_t<raw_type>;
};

template<class F>
struct continuation_result<void, F>
{
  using raw_type = std::invoke_result_t<F&>;
  using type = task_unwrap_t<raw_type>;
};

struct task_frame_header
{
  std::pmr::memory_resource* resource;
  void* allocation;
  std::size_t bytes;
};

inline void* allocate_task_frame(std::size_t size)
{
  auto* resource = current_task_environment_value().frame_resource;
  auto total = size + sizeof(task_frame_header) + alignof(std::max_align_t);
  void* allocation = resource->allocate(total, alignof(std::max_align_t));
  void* candidate = static_cast<std::byte*>(allocation) + sizeof(task_frame_header);
  std::size_t space = total - sizeof(task_frame_header);
  if (!std::align(alignof(std::max_align_t), size, candidate, space))
  {
    resource->deallocate(allocation, total, alignof(std::max_align_t));
    throw std::bad_alloc();
  }
  auto* header = reinterpret_cast<task_frame_header*>(
    static_cast<std::byte*>(candidate) - sizeof(task_frame_header));
  *header = {resource, allocation, total};
  return candidate;
}

inline void deallocate_task_frame(void* pointer) noexcept
{
  if (!pointer)
    return;
  auto* header = reinterpret_cast<task_frame_header*>(
    static_cast<std::byte*>(pointer) - sizeof(task_frame_header));
  header->resource->deallocate(header->allocation, header->bytes, alignof(std::max_align_t));
}

template<class T, class F>
task<typename continuation_result<T, F>::type> compose_then(
  task<T> parent, scheduler executor, memory::memory_resource* resource, F fn);

template<class T, class F>
task<T> compose_catching(
  task<T> parent, scheduler executor, memory::memory_resource* resource, F fn);

template<class T, class F>
task<T> compose_finally(
  task<T> parent, scheduler executor, memory::memory_resource* resource, F fn);
} // namespace modern::detail

export namespace modern
{
template<class F, class... Args>
auto submit(scheduler executor, memory::memory_resource* resource, F&& f, Args&&... args)
  -> task<std::invoke_result_t<std::remove_cvref_t<F>&, std::remove_cvref_t<Args>&...>>;

template<class T>
class task
{
public:
  using value_type = T;

  class promise_type
  {
  public:
    promise_type()
      : environment_(current_task_environment_value())
    {
      if (!environment_.scheduler.valid())
        environment_.scheduler = inline_scheduler();
      if (!environment_.frame_resource)
        environment_.frame_resource = memory::get_default_resource();
      state_ = detail::make_shared_state<T>(
        environment_.scheduler,
        environment_.frame_resource,
        std::make_shared<task_environment>(environment_));
    }

    static void* operator new(std::size_t size)
    {
      return detail::allocate_task_frame(size);
    }

    static void operator delete(void* pointer) noexcept
    {
      detail::deallocate_task_frame(pointer);
    }

    static void operator delete(void* pointer, std::size_t) noexcept
    {
      detail::deallocate_task_frame(pointer);
    }

    task get_return_object() { return task{state_}; }
    std::suspend_never initial_suspend() const noexcept { return {}; }
    std::suspend_never final_suspend() const noexcept { return {}; }

    template<class U>
      requires std::constructible_from<T, U>
    void return_value(U&& value)
    {
      state_->set_value(T(std::forward<U>(value)));
    }

    void unhandled_exception() noexcept
    {
      try { state_->set_exception(std::current_exception()); }
      catch (...) {}
    }

    detail::ready_awaiter<task_environment> await_transform(this_task::environment_query) noexcept
    {
      return {environment_};
    }

    detail::ready_awaiter<scheduler> await_transform(this_task::scheduler_query) noexcept
    {
      return {environment_.scheduler};
    }

    detail::ready_awaiter<std::pmr::memory_resource*> await_transform(this_task::memory_resource_query) noexcept
    {
      return {environment_.frame_resource};
    }

    detail::ready_awaiter<std::optional<trace::TraceContext>> await_transform(this_task::trace_context_query) noexcept
    {
      return {environment_.trace_context};
    }

    detail::ready_awaiter<std::stop_token> await_transform(this_task::stop_token_query) noexcept
    {
      return {environment_.stop_token};
    }

    template<class Awaitable>
    Awaitable&& await_transform(Awaitable&& awaitable) noexcept
    {
      return std::forward<Awaitable>(awaitable);
    }

  private:
    task_environment environment_;
    std::shared_ptr<detail::shared_state<T>> state_;
  };

  struct awaiter
  {
    std::shared_ptr<detail::shared_state<T>> state;

    bool await_ready() const noexcept { return state->done(); }

    void await_suspend(std::coroutine_handle<> handle)
    {
      auto environment = current_task_environment_value();
      auto executor = environment.scheduler.valid()
        ? environment.scheduler
        : state->default_scheduler();

      state->add_continuation(detail::move_only_function{
        state->resource(),
        [handle, environment = std::move(environment), executor]() mutable noexcept
        {
          auto resume = [handle, environment = std::move(environment)]() mutable noexcept
          {
            task_environment_scope scope{std::move(environment)};
            handle.resume();
          };

          if (executor.valid())
          {
            try
            {
              executor.execute(std::move(resume));
              return;
            }
            catch (...) {}
          }
          resume();
        }});
    }

    T await_resume() { return state->take_value(); }
  };

  task() = default;
  task(task&&) noexcept = default;
  task& operator=(task&&) noexcept = default;
  task(const task&) = delete;
  task& operator=(const task&) = delete;

  bool valid() const noexcept { return static_cast<bool>(state_); }
  bool ready() const noexcept { return state_ && state_->done(); }
  void start() const {}

  T get()
  {
    return consume_state()->take_value();
  }

  void detach() noexcept { state_.reset(); }

  awaiter operator co_await() &&
  {
    return {consume_state()};
  }

  awaiter operator co_await() & = delete;

  template<class F>
  auto then(F&& fn) &&
  {
    auto state = state_or_throw();
    return std::move(*this).then_on(
      state->default_scheduler(), state->resource(), std::forward<F>(fn));
  }

  template<class F>
  auto then(memory::memory_resource* resource, F&& fn) &&
  {
    return std::move(*this).then_on(
      state_or_throw()->default_scheduler(), resource, std::forward<F>(fn));
  }

  template<class F>
  auto then_on(scheduler executor, F&& fn) &&
  {
    return std::move(*this).then_on(std::move(executor), nullptr, std::forward<F>(fn));
  }

  template<class F>
  auto then_on(scheduler executor, memory::memory_resource* resource, F&& fn) &&
  {
    using function_type = std::remove_cvref_t<F>;
    auto state = state_or_throw();
    auto environment = state->environment()
      ? *state->environment()
      : current_task_environment_value();
    environment.scheduler = executor.valid() ? executor : inline_scheduler();
    environment.frame_resource = resource ? resource : state->resource();
    task_environment_scope scope{environment};
    return detail::compose_then<T, function_type>(
      std::move(*this), environment.scheduler, environment.frame_resource,
      function_type(std::forward<F>(fn)));
  }

  template<class F>
  task catching(F&& fn) &&
  {
    auto state = state_or_throw();
    return std::move(*this).catching_on(
      state->default_scheduler(), state->resource(), std::forward<F>(fn));
  }

  template<class F>
  task catching(memory::memory_resource* resource, F&& fn) &&
  {
    return std::move(*this).catching_on(
      state_or_throw()->default_scheduler(), resource, std::forward<F>(fn));
  }

  template<class F>
  task catching_on(scheduler executor, F&& fn) &&
  {
    return std::move(*this).catching_on(std::move(executor), nullptr, std::forward<F>(fn));
  }

  template<class F>
  task catching_on(scheduler executor, memory::memory_resource* resource, F&& fn) &&
  {
    using function_type = std::remove_cvref_t<F>;
    auto state = state_or_throw();
    auto environment = state->environment()
      ? *state->environment()
      : current_task_environment_value();
    environment.scheduler = executor.valid() ? executor : inline_scheduler();
    environment.frame_resource = resource ? resource : state->resource();
    task_environment_scope scope{environment};
    return detail::compose_catching<T, function_type>(
      std::move(*this), environment.scheduler, environment.frame_resource,
      function_type(std::forward<F>(fn)));
  }

  template<class F>
  task finally(F&& fn) &&
  {
    auto state = state_or_throw();
    return std::move(*this).finally_on(
      state->default_scheduler(), state->resource(), std::forward<F>(fn));
  }

  template<class F>
  task finally(memory::memory_resource* resource, F&& fn) &&
  {
    return std::move(*this).finally_on(
      state_or_throw()->default_scheduler(), resource, std::forward<F>(fn));
  }

  template<class F>
  task finally_on(scheduler executor, F&& fn) &&
  {
    return std::move(*this).finally_on(std::move(executor), nullptr, std::forward<F>(fn));
  }

  template<class F>
  task finally_on(scheduler executor, memory::memory_resource* resource, F&& fn) &&
  {
    using function_type = std::remove_cvref_t<F>;
    auto state = state_or_throw();
    auto environment = state->environment()
      ? *state->environment()
      : current_task_environment_value();
    environment.scheduler = executor.valid() ? executor : inline_scheduler();
    environment.frame_resource = resource ? resource : state->resource();
    task_environment_scope scope{environment};
    return detail::compose_finally<T, function_type>(
      std::move(*this), environment.scheduler, environment.frame_resource,
      function_type(std::forward<F>(fn)));
  }

  template<class F> auto then(F&&) & = delete;
  template<class F> auto then_on(scheduler, F&&) & = delete;
  template<class F> task catching(F&&) & = delete;
  template<class F> task finally(F&&) & = delete;

private:
  template<class U>
  friend task<U> detail::make_task(std::shared_ptr<void>);

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
      : environment_(current_task_environment_value())
    {
      if (!environment_.scheduler.valid())
        environment_.scheduler = inline_scheduler();
      if (!environment_.frame_resource)
        environment_.frame_resource = memory::get_default_resource();
      state_ = detail::make_shared_state<void>(
        environment_.scheduler,
        environment_.frame_resource,
        std::make_shared<task_environment>(environment_));
    }

    static void* operator new(std::size_t size) { return detail::allocate_task_frame(size); }
    static void operator delete(void* pointer) noexcept { detail::deallocate_task_frame(pointer); }
    static void operator delete(void* pointer, std::size_t) noexcept { detail::deallocate_task_frame(pointer); }

    task get_return_object() { return task{state_}; }
    std::suspend_never initial_suspend() const noexcept { return {}; }
    std::suspend_never final_suspend() const noexcept { return {}; }
    void return_void() { state_->set_value(); }

    void unhandled_exception() noexcept
    {
      try { state_->set_exception(std::current_exception()); }
      catch (...) {}
    }

    detail::ready_awaiter<task_environment> await_transform(this_task::environment_query) noexcept { return {environment_}; }
    detail::ready_awaiter<scheduler> await_transform(this_task::scheduler_query) noexcept { return {environment_.scheduler}; }
    detail::ready_awaiter<std::pmr::memory_resource*> await_transform(this_task::memory_resource_query) noexcept { return {environment_.frame_resource}; }
    detail::ready_awaiter<std::optional<trace::TraceContext>> await_transform(this_task::trace_context_query) noexcept { return {environment_.trace_context}; }
    detail::ready_awaiter<std::stop_token> await_transform(this_task::stop_token_query) noexcept { return {environment_.stop_token}; }

    template<class Awaitable>
    Awaitable&& await_transform(Awaitable&& awaitable) noexcept
    {
      return std::forward<Awaitable>(awaitable);
    }

  private:
    task_environment environment_;
    std::shared_ptr<detail::shared_state<void>> state_;
  };

  struct awaiter
  {
    std::shared_ptr<detail::shared_state<void>> state;
    bool await_ready() const noexcept { return state->done(); }

    void await_suspend(std::coroutine_handle<> handle)
    {
      auto environment = current_task_environment_value();
      auto executor = environment.scheduler.valid()
        ? environment.scheduler
        : state->default_scheduler();
      state->add_continuation(detail::move_only_function{
        state->resource(),
        [handle, environment = std::move(environment), executor]() mutable noexcept
        {
          auto resume = [handle, environment = std::move(environment)]() mutable noexcept
          {
            task_environment_scope scope{std::move(environment)};
            handle.resume();
          };
          if (executor.valid())
          {
            try { executor.execute(std::move(resume)); return; }
            catch (...) {}
          }
          resume();
        }});
    }

    void await_resume() { state->take_value(); }
  };

  task() = default;
  task(task&&) noexcept = default;
  task& operator=(task&&) noexcept = default;
  task(const task&) = delete;
  task& operator=(const task&) = delete;

  bool valid() const noexcept { return static_cast<bool>(state_); }
  bool ready() const noexcept { return state_ && state_->done(); }
  void start() const {}
  void get() { consume_state()->take_value(); }
  void detach() noexcept { state_.reset(); }
  awaiter operator co_await() && { return {consume_state()}; }
  awaiter operator co_await() & = delete;

  template<class F>
  auto then(F&& fn) &&
  {
    auto state = state_or_throw();
    return std::move(*this).then_on(
      state->default_scheduler(), state->resource(), std::forward<F>(fn));
  }

  template<class F>
  auto then(memory::memory_resource* resource, F&& fn) &&
  {
    return std::move(*this).then_on(
      state_or_throw()->default_scheduler(), resource, std::forward<F>(fn));
  }

  template<class F>
  auto then_on(scheduler executor, F&& fn) &&
  {
    return std::move(*this).then_on(std::move(executor), nullptr, std::forward<F>(fn));
  }

  template<class F>
  auto then_on(scheduler executor, memory::memory_resource* resource, F&& fn) &&
  {
    using function_type = std::remove_cvref_t<F>;
    auto state = state_or_throw();
    auto environment = state->environment() ? *state->environment() : current_task_environment_value();
    environment.scheduler = executor.valid() ? executor : inline_scheduler();
    environment.frame_resource = resource ? resource : state->resource();
    task_environment_scope scope{environment};
    return detail::compose_then<void, function_type>(
      std::move(*this), environment.scheduler, environment.frame_resource,
      function_type(std::forward<F>(fn)));
  }

  template<class F>
  task catching(F&& fn) &&
  {
    auto state = state_or_throw();
    return std::move(*this).catching_on(
      state->default_scheduler(), state->resource(), std::forward<F>(fn));
  }

  template<class F>
  task catching(memory::memory_resource* resource, F&& fn) &&
  {
    return std::move(*this).catching_on(
      state_or_throw()->default_scheduler(), resource, std::forward<F>(fn));
  }

  template<class F>
  task catching_on(scheduler executor, F&& fn) &&
  {
    return std::move(*this).catching_on(std::move(executor), nullptr, std::forward<F>(fn));
  }

  template<class F>
  task catching_on(scheduler executor, memory::memory_resource* resource, F&& fn) &&
  {
    using function_type = std::remove_cvref_t<F>;
    auto state = state_or_throw();
    auto environment = state->environment() ? *state->environment() : current_task_environment_value();
    environment.scheduler = executor.valid() ? executor : inline_scheduler();
    environment.frame_resource = resource ? resource : state->resource();
    task_environment_scope scope{environment};
    return detail::compose_catching<void, function_type>(
      std::move(*this), environment.scheduler, environment.frame_resource,
      function_type(std::forward<F>(fn)));
  }

  template<class F>
  task finally(F&& fn) &&
  {
    auto state = state_or_throw();
    return std::move(*this).finally_on(
      state->default_scheduler(), state->resource(), std::forward<F>(fn));
  }

  template<class F>
  task finally(memory::memory_resource* resource, F&& fn) &&
  {
    return std::move(*this).finally_on(
      state_or_throw()->default_scheduler(), resource, std::forward<F>(fn));
  }

  template<class F>
  task finally_on(scheduler executor, F&& fn) &&
  {
    return std::move(*this).finally_on(std::move(executor), nullptr, std::forward<F>(fn));
  }

  template<class F>
  task finally_on(scheduler executor, memory::memory_resource* resource, F&& fn) &&
  {
    using function_type = std::remove_cvref_t<F>;
    auto state = state_or_throw();
    auto environment = state->environment() ? *state->environment() : current_task_environment_value();
    environment.scheduler = executor.valid() ? executor : inline_scheduler();
    environment.frame_resource = resource ? resource : state->resource();
    task_environment_scope scope{environment};
    return detail::compose_finally<void, function_type>(
      std::move(*this), environment.scheduler, environment.frame_resource,
      function_type(std::forward<F>(fn)));
  }

private:
  template<class U>
  friend task<U> detail::make_task(std::shared_ptr<void>);

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

template<class T, class E>
using result_task = task<std::expected<T, E>>;

template<class E>
using status_task = task<std::expected<void, E>>;
} // namespace modern

export namespace modern::detail
{
template<class T>
task<T> make_task(std::shared_ptr<void> state)
{
  return task<T>{std::static_pointer_cast<shared_state<T>>(std::move(state))};
}

template<class T, class F>
task<typename continuation_result<T, F>::type> compose_then(
  task<T> parent, scheduler executor, memory::memory_resource* resource, F fn)
{
  using raw_type = typename continuation_result<T, F>::raw_type;
  using result_type = typename continuation_result<T, F>::type;

  if constexpr (std::is_void_v<T>)
  {
    co_await std::move(parent);
    if constexpr (std::is_void_v<raw_type>)
    {
      co_await submit(executor, resource, std::move(fn));
      co_return;
    }
    else
    {
      auto raw = co_await submit(executor, resource, std::move(fn));
      if constexpr (is_task_v<raw_type>)
      {
        if constexpr (std::is_void_v<result_type>)
        {
          co_await std::move(raw);
          co_return;
        }
        else
          co_return co_await std::move(raw);
      }
      else
        co_return std::move(raw);
    }
  }
  else
  {
    auto value = co_await std::move(parent);
    auto invoke = [fn = std::move(fn), value = std::move(value)]() mutable -> raw_type
    {
      return std::invoke(fn, std::move(value));
    };

    if constexpr (std::is_void_v<raw_type>)
    {
      co_await submit(executor, resource, std::move(invoke));
      co_return;
    }
    else
    {
      auto raw = co_await submit(executor, resource, std::move(invoke));
      if constexpr (is_task_v<raw_type>)
      {
        if constexpr (std::is_void_v<result_type>)
        {
          co_await std::move(raw);
          co_return;
        }
        else
          co_return co_await std::move(raw);
      }
      else
        co_return std::move(raw);
    }
  }
}

template<class T, class F>
task<T> compose_catching(
  task<T> parent, scheduler executor, memory::memory_resource* resource, F fn)
{
  std::exception_ptr exception;
  std::optional<std::conditional_t<std::is_void_v<T>, bool, T>> value;

  try
  {
    if constexpr (std::is_void_v<T>)
    {
      co_await std::move(parent);
      value.emplace(true);
    }
    else
      value.emplace(co_await std::move(parent));
  }
  catch (...)
  {
    exception = std::current_exception();
  }

  if (!exception)
  {
    if constexpr (std::is_void_v<T>)
      co_return;
    else
      co_return std::move(*value);
  }

  using raw_type = std::invoke_result_t<F&, std::exception_ptr>;
  static_assert(std::same_as<task_unwrap_t<raw_type>, T>,
    "catching handler must return T, void, or task<T>");

  auto invoke = [fn = std::move(fn), exception]() mutable -> raw_type
  {
    return std::invoke(fn, exception);
  };

  if constexpr (std::is_void_v<raw_type>)
  {
    co_await submit(executor, resource, std::move(invoke));
    co_return;
  }
  else
  {
    auto raw = co_await submit(executor, resource, std::move(invoke));
    if constexpr (is_task_v<raw_type>)
    {
      if constexpr (std::is_void_v<T>)
      {
        co_await std::move(raw);
        co_return;
      }
      else
        co_return co_await std::move(raw);
    }
    else
      co_return std::move(raw);
  }
}

template<class T, class F>
task<T> compose_finally(
  task<T> parent, scheduler executor, memory::memory_resource* resource, F fn)
{
  std::exception_ptr original;
  std::optional<std::conditional_t<std::is_void_v<T>, bool, T>> value;

  try
  {
    if constexpr (std::is_void_v<T>)
    {
      co_await std::move(parent);
      value.emplace(true);
    }
    else
      value.emplace(co_await std::move(parent));
  }
  catch (...)
  {
    original = std::current_exception();
  }

  using raw_type = std::invoke_result_t<F&>;
  static_assert(std::is_void_v<task_unwrap_t<raw_type>>,
    "finally handler must return void or task<void>");

  if constexpr (std::is_void_v<raw_type>)
    co_await submit(executor, resource, std::move(fn));
  else
  {
    auto cleanup = co_await submit(executor, resource, std::move(fn));
    co_await std::move(cleanup);
  }

  if (original)
    std::rethrow_exception(original);

  if constexpr (std::is_void_v<T>)
    co_return;
  else
    co_return std::move(*value);
}
} // namespace modern::detail

export namespace modern
{
template<class F, class... Args>
auto submit(scheduler executor, memory::memory_resource* resource, F&& f, Args&&... args)
  -> task<std::invoke_result_t<std::remove_cvref_t<F>&, std::remove_cvref_t<Args>&...>>
{
  using result_type = std::invoke_result_t<std::remove_cvref_t<F>&, std::remove_cvref_t<Args>&...>;
  auto* actual_resource = resource ? resource : memory::get_default_resource();
  auto environment = current_task_environment_value();
  environment.scheduler = executor.valid() ? executor : inline_scheduler();
  environment.frame_resource = actual_resource;
  auto state = detail::make_shared_state<result_type>(
    environment.scheduler, actual_resource, std::make_shared<task_environment>(environment));
  auto result = detail::make_task<result_type>(state);
  auto callable = std::remove_cvref_t<F>(std::forward<F>(f));
  auto arguments = std::make_tuple(std::remove_cvref_t<Args>(std::forward<Args>(args))...);

  environment.scheduler.execute(
    [state, environment, callable = std::move(callable), arguments = std::move(arguments)]() mutable
    {
      task_environment_scope scope{environment};
      detail::fulfill<result_type>(state, [&]() mutable -> result_type
      {
        if constexpr (std::is_void_v<result_type>)
          std::apply(callable, std::move(arguments));
        else
          return std::apply(callable, std::move(arguments));
      });
    });
  return result;
}

template<class F, class... Args>
auto submit(scheduler executor, std::stop_token token, memory::memory_resource* resource, F&& f, Args&&... args)
{
  using callable_type = std::remove_cvref_t<F>;
  using traits = detail::stoppable_submit_result<callable_type, std::remove_cvref_t<Args>...>;
  using result_type = typename traits::type;
  auto* actual_resource = resource ? resource : memory::get_default_resource();
  auto environment = current_task_environment_value();
  environment.scheduler = executor.valid() ? executor : inline_scheduler();
  environment.frame_resource = actual_resource;
  environment.stop_token = token;
  auto state = detail::make_shared_state<result_type>(
    environment.scheduler, actual_resource, std::make_shared<task_environment>(environment));
  auto result = detail::make_task<result_type>(state);

  if (token.stop_requested())
  {
    state->set_stopped();
    return result;
  }

  auto started = detail::allocate_shared_object<std::atomic<bool>>(actual_resource, false);
  using callback_type = std::stop_callback<std::function<void()>>;
  auto callback = detail::allocate_shared_object<callback_type>(
    actual_resource, token, std::function<void()>{[state, started]
    {
      if (!started->exchange(true, std::memory_order_acq_rel))
        state->set_stopped();
    }});

  auto callable = callable_type(std::forward<F>(f));
  auto arguments = std::make_tuple(std::remove_cvref_t<Args>(std::forward<Args>(args))...);
  environment.scheduler.execute(
    [state, started, callback, environment, token, callable = std::move(callable),
     arguments = std::move(arguments)]() mutable
    {
      if (started->exchange(true, std::memory_order_acq_rel))
        return;
      task_environment_scope scope{environment};
      detail::fulfill<result_type>(state, [&]() mutable -> result_type
      {
        if constexpr (traits::uses_stop_token)
        {
          return std::apply([&](auto&&... unpacked) -> result_type
          {
            return std::invoke(callable, token, std::move(unpacked)...);
          }, std::move(arguments));
        }
        else
        {
          if constexpr (std::is_void_v<result_type>)
            std::apply(callable, std::move(arguments));
          else
            return std::apply(callable, std::move(arguments));
        }
      });
    });
  return result;
}

template<class F, class... Args>
  requires (!std::convertible_to<std::remove_cvref_t<F>, memory::memory_resource*>)
auto submit(scheduler executor, F&& f, Args&&... args)
{
  return submit(executor, memory::get_default_resource(),
    std::forward<F>(f), std::forward<Args>(args)...);
}

template<class F, class... Args>
auto submit(scheduler executor, std::stop_token token, F&& f, Args&&... args)
{
  return submit(executor, token, memory::get_default_resource(),
    std::forward<F>(f), std::forward<Args>(args)...);
}
} // namespace modern
