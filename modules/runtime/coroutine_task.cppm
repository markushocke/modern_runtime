module;

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <latch>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

export module modern.runtime:coroutine_task;

export import modern.exec;
export import modern.memory;
export import modern.trace;

export namespace modern::runtime
{
using Scheduler = modern::scheduler;
using StopToken = std::stop_token;

template<class T>
class Task;
using TraceContext = modern::trace::TraceContext;

struct TaskEnvironment
{
  Scheduler* scheduler{};
  std::pmr::memory_resource* frame_resource{};
  std::optional<TraceContext> trace_context{};
  StopToken stop_token{};
};

class TaskEnvironmentPolicy
{
public:
  virtual ~TaskEnvironmentPolicy() = default;
  virtual TaskEnvironment current_environment() noexcept = 0;
};

namespace detail
{
inline TaskEnvironmentPolicy*& task_environment_policy_storage() noexcept
{
  thread_local TaskEnvironmentPolicy* policy = nullptr;
  return policy;
}

inline std::pmr::memory_resource*& frame_resource_storage() noexcept
{
  thread_local std::pmr::memory_resource* resource = nullptr;
  return resource;
}

inline bool& trace_context_scope_active_storage() noexcept
{
  thread_local bool active = false;
  return active;
}

inline std::optional<TraceContext>& trace_context_storage() noexcept
{
  thread_local std::optional<TraceContext> context;
  return context;
}

inline TaskEnvironment current_task_environment_value() noexcept
{
  TaskEnvironment environment;

  if (auto* policy = task_environment_policy_storage())
    environment = policy->current_environment();

  if (auto* resource = frame_resource_storage())
    environment.frame_resource = resource;

  if (trace_context_scope_active_storage())
    environment.trace_context = trace_context_storage();

  if (!environment.frame_resource)
    environment.frame_resource = memory::get_default_resource();

  return environment;
}

inline std::pmr::memory_resource* current_frame_memory_resource() noexcept
{
  return current_task_environment_value().frame_resource;
}

struct TaskFrameAllocationHeader
{
  std::pmr::memory_resource* resource{};
  void* raw{};
  std::size_t raw_size{};
  std::size_t raw_alignment{};
};

inline void* allocate_task_frame(std::size_t bytes, std::size_t alignment)
{
  auto* resource = current_frame_memory_resource();
  const auto header_size = sizeof(TaskFrameAllocationHeader);
  const auto total_size = header_size + bytes + alignment;

  void* raw = resource->allocate(total_size, alignof(std::max_align_t));
  auto* begin = static_cast<std::byte*>(raw) + header_size;
  void* aligned = begin;
  auto space = total_size - header_size;

  if (std::align(alignment, bytes, aligned, space) == nullptr)
  {
    resource->deallocate(raw, total_size, alignof(std::max_align_t));
    throw std::bad_alloc();
  }

  auto* header = reinterpret_cast<TaskFrameAllocationHeader*>(
    static_cast<std::byte*>(aligned) - header_size);
  *header = TaskFrameAllocationHeader{resource, raw, total_size, alignof(std::max_align_t)};
  return aligned;
}

inline void deallocate_task_frame(void* ptr) noexcept
{
  if (!ptr)
    return;

  const auto header_size = sizeof(TaskFrameAllocationHeader);
  auto* header = reinterpret_cast<TaskFrameAllocationHeader*>(
    static_cast<std::byte*>(ptr) - header_size);
  header->resource->deallocate(header->raw, header->raw_size, header->raw_alignment);
}

inline void merge_environment(TaskEnvironment& target, const TaskEnvironment& source) noexcept
{
  if (!target.scheduler && source.scheduler)
    target.scheduler = source.scheduler;

  if (!target.frame_resource && source.frame_resource)
    target.frame_resource = source.frame_resource;

  if (!target.trace_context && source.trace_context)
    target.trace_context = source.trace_context;

  if (target.stop_token == StopToken{} && source.stop_token != StopToken{})
    target.stop_token = source.stop_token;
}

template<class TraceLike>
concept trace_context_like = requires(const TraceLike& trace)
{
  trace.trace_id;
  trace.span_id;
};

template<trace_context_like TraceLike>
inline TraceContext to_trace_context(const TraceLike& trace) noexcept
{
  TraceContext context;
  context.trace_id = trace.trace_id;
  context.span_id = trace.span_id;

  if constexpr (requires { trace.flags; })
    context.flags = trace.flags;

  return context;
}

template<class T>
struct is_task : std::false_type
{
};

template<class T>
struct is_task<Task<T>> : std::true_type
{
};

template<class T>
inline constexpr bool is_task_v = is_task<std::remove_cvref_t<T>>::value;

template<class T>
struct task_unwrap
{
  using type = T;
};

template<class T>
struct task_unwrap<Task<T>>
{
  using type = T;
};

template<class T>
using task_unwrap_t = typename task_unwrap<std::remove_cvref_t<T>>::type;

template<class R, class T>
struct is_task_of : std::false_type
{
};

template<class T>
struct is_task_of<Task<T>, T> : std::true_type
{
};

template<class R, class T>
inline constexpr bool is_task_of_v = is_task_of<std::remove_cvref_t<R>, T>::value;

template<class T>
struct is_expected : std::false_type
{
};

template<class T, class E>
struct is_expected<std::expected<T, E>> : std::true_type
{
};

template<class T>
inline constexpr bool is_expected_v = is_expected<std::remove_cvref_t<T>>::value;

template<class T>
struct expected_traits;

template<class T>
struct expected_traits
{
  using value_type = void;
  using error_type = void;

  template<class U>
  using rebind = U;
};

template<class T, class E>
struct expected_traits<std::expected<T, E>>
{
  using value_type = T;
  using error_type = E;

  template<class U>
  using rebind = std::expected<U, E>;
};

template<class T>
using expected_value_t = typename expected_traits<std::remove_cvref_t<T>>::value_type;

template<class T>
using expected_error_t = typename expected_traits<std::remove_cvref_t<T>>::error_type;

template<class Expected, class U>
using expected_rebind_t = typename expected_traits<std::remove_cvref_t<Expected>>::template rebind<U>;

template<
  class Expected,
  class F,
  bool IsExpected = is_expected_v<Expected>,
  bool IsVoid = std::same_as<expected_value_t<Expected>, void>>
struct expected_transform_result;

template<class Expected, class F>
struct expected_transform_result<Expected, F, false, false>
{
  using type = void;
};

template<class Expected, class F>
struct expected_transform_result<Expected, F, false, true>
{
  using type = void;
};

template<class Expected, class F>
struct expected_transform_result<Expected, F, true, false>
{
  using type = std::invoke_result_t<F&, expected_value_t<Expected>>;
};

template<class Expected, class F>
struct expected_transform_result<Expected, F, true, true>
{
  using type = std::invoke_result_t<F&>;
};

template<class Expected, class F>
using expected_transform_result_t = typename expected_transform_result<Expected, F>::type;
} // namespace detail

[[nodiscard]] inline TaskEnvironmentPolicy* task_environment_policy() noexcept
{
  return detail::task_environment_policy_storage();
}

inline TaskEnvironmentPolicy* set_task_environment_policy(TaskEnvironmentPolicy* policy) noexcept
{
  auto*& current = detail::task_environment_policy_storage();
  auto* previous = current;
  current = policy;
  return previous;
}

[[nodiscard]] inline TaskEnvironment current_task_environment_value() noexcept
{
  return detail::current_task_environment_value();
}

[[nodiscard]] inline std::pmr::memory_resource* frame_memory_resource() noexcept
{
  return detail::current_frame_memory_resource();
}

inline std::pmr::memory_resource* set_frame_memory_resource(std::pmr::memory_resource* resource) noexcept
{
  auto*& current = detail::frame_resource_storage();
  auto* previous = current;
  current = resource;
  return previous;
}

[[nodiscard]] inline std::optional<TraceContext> trace_context() noexcept
{
  return current_task_environment_value().trace_context;
}

inline std::optional<TraceContext> set_trace_context(std::optional<TraceContext> context) noexcept
{
  auto& active = detail::trace_context_scope_active_storage();
  auto& current = detail::trace_context_storage();
  auto previous = active ? current : std::optional<TraceContext>{};
  current = std::move(context);
  active = true;
  return previous;
}

class TaskEnvironmentScope
{
public:
  explicit TaskEnvironmentScope(TaskEnvironmentPolicy* policy) noexcept
    : previous_(set_task_environment_policy(policy))
  {
  }

  TaskEnvironmentScope(const TaskEnvironmentScope&) = delete;
  TaskEnvironmentScope& operator=(const TaskEnvironmentScope&) = delete;

  ~TaskEnvironmentScope()
  {
    set_task_environment_policy(previous_);
  }

private:
  TaskEnvironmentPolicy* previous_{};
};

class FrameMemoryResourceScope
{
public:
  explicit FrameMemoryResourceScope(std::pmr::memory_resource* resource) noexcept
    : previous_(set_frame_memory_resource(resource))
  {
  }

  FrameMemoryResourceScope(const FrameMemoryResourceScope&) = delete;
  FrameMemoryResourceScope& operator=(const FrameMemoryResourceScope&) = delete;

  ~FrameMemoryResourceScope()
  {
    set_frame_memory_resource(previous_);
  }

private:
  std::pmr::memory_resource* previous_{};
};

class TraceContextScope
{
public:
  explicit TraceContextScope(std::optional<TraceContext> context) noexcept
    : previous_active_(detail::trace_context_scope_active_storage()),
      previous_(detail::trace_context_storage())
  {
    detail::trace_context_storage() = std::move(context);
    detail::trace_context_scope_active_storage() = true;
  }

  TraceContextScope(const TraceContextScope&) = delete;
  TraceContextScope& operator=(const TraceContextScope&) = delete;

  ~TraceContextScope()
  {
    detail::trace_context_storage() = previous_;
    detail::trace_context_scope_active_storage() = previous_active_;
  }

private:
  bool previous_active_{};
  std::optional<TraceContext> previous_{};
};

class CurrentTaskEnvironmentAwaiter
{
public:
  [[nodiscard]] bool await_ready() const noexcept
  {
    return false;
  }

  template<class Promise>
  bool await_suspend(std::coroutine_handle<Promise> current) noexcept
  {
    if constexpr (requires(Promise& promise) { promise.environment(); })
      environment_ = current.promise().environment();
    else
      environment_ = current_task_environment_value();

    return false;
  }

  [[nodiscard]] TaskEnvironment await_resume() const noexcept
  {
    return environment_;
  }

private:
  TaskEnvironment environment_ = current_task_environment_value();
};

class CurrentTraceContextAwaiter
{
public:
  [[nodiscard]] bool await_ready() const noexcept
  {
    return false;
  }

  template<class Promise>
  bool await_suspend(std::coroutine_handle<Promise> current) noexcept
  {
    if constexpr (requires(Promise& promise) { promise.environment(); })
      trace_context_ = current.promise().environment().trace_context;
    else
      trace_context_ = trace_context();

    return false;
  }

  [[nodiscard]] std::optional<TraceContext> await_resume() const noexcept
  {
    return trace_context_;
  }

private:
  std::optional<TraceContext> trace_context_ = trace_context();
};

[[nodiscard]] inline CurrentTaskEnvironmentAwaiter current_task_environment() noexcept
{
  return {};
}

[[nodiscard]] inline CurrentTraceContextAwaiter current_trace_context() noexcept
{
  return {};
}

template<class T>
class Task
{
public:
  class promise_type;
  using value_type = T;
  using handle_type = std::coroutine_handle<promise_type>;

  Task() noexcept = default;

  explicit Task(handle_type handle) noexcept
    : handle_(handle)
  {
  }

  Task(Task&& other) noexcept
    : handle_(std::exchange(other.handle_, {}))
  {
  }

  Task& operator=(Task&& other) noexcept
  {
    if (this != &other)
    {
      if (handle_)
        handle_.destroy();

      handle_ = std::exchange(other.handle_, {});
    }

    return *this;
  }

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  ~Task()
  {
    if (handle_)
      handle_.destroy();
  }

  [[nodiscard]] bool valid() const noexcept
  {
    return static_cast<bool>(handle_);
  }

  [[nodiscard]] bool done() const noexcept
  {
    return !handle_ || handle_.done();
  }

  void start() noexcept
  {
    if (!handle_ || handle_.done())
      return;

    auto& promise = handle_.promise();
    promise.seed_environment_if_missing();

    if (!promise.started_)
    {
      promise.started_ = true;
      handle_.resume();
    }
  }

  void detach() noexcept
  {
    if (handle_)
      handle_.destroy();

    handle_ = {};
  }

  [[nodiscard]] bool await_ready() const noexcept
  {
    return done();
  }

  template<class Promise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> awaiting) noexcept
  {
    if (!handle_)
      return std::noop_coroutine();

    auto& promise = handle_.promise();
    promise.continuation_ = awaiting;
    promise.inherit_from(awaiting);
    promise.seed_environment_if_missing();

    if (!promise.started_)
    {
      promise.started_ = true;
      return handle_;
    }

    return std::noop_coroutine();
  }

  T await_resume()
  {
    auto& promise = state_or_throw();

    if (promise.exception_)
      std::rethrow_exception(promise.exception_);

    return std::move(*promise.value_);
  }

  T sync_wait()
  {
    auto& promise = state_or_throw();

    if (!handle_.done())
    {
      std::latch done(1);
      promise.wait_latch_ = &done;
      start();

      if (!handle_.done())
        done.wait();

      promise.wait_latch_ = nullptr;
    }

    return await_resume();
  }

  T get()
  {
    return sync_wait();
  }

  [[nodiscard]] handle_type native_handle() const noexcept
  {
    return handle_;
  }

  [[nodiscard]] TaskEnvironment environment() const noexcept
  {
    return handle_ ? handle_.promise().environment() : TaskEnvironment{};
  }

  void set_environment(TaskEnvironment environment) noexcept
  {
    if (handle_)
      handle_.promise().set_environment(std::move(environment));
  }

  template<detail::trace_context_like TraceLike>
  void set_trace_context(const TraceLike& trace) noexcept
  {
    auto environment = this->environment();
    environment.trace_context = detail::to_trace_context(trace);
    set_environment(std::move(environment));
  }

  [[nodiscard]] std::optional<TraceContext> trace_context() const noexcept
  {
    return environment().trace_context;
  }

  template<class F>
    requires (!detail::is_expected_v<T> && std::invocable<std::remove_reference_t<F>&, T>)
  auto then(F&& continuation) &&
    -> Task<detail::task_unwrap_t<std::invoke_result_t<std::remove_reference_t<F>&, T>>>
  {
    using continuation_type = std::remove_cvref_t<F>;
    using raw_result = std::invoke_result_t<continuation_type&, T>;
    using result_type = detail::task_unwrap_t<raw_result>;

    return then_impl<continuation_type, raw_result, result_type>(
      std::move(*this),
      continuation_type(std::forward<F>(continuation)));
  }

  template<class F>
    requires (!detail::is_expected_v<T> && std::invocable<std::remove_reference_t<F>&, T>)
  auto then_on(Scheduler& scheduler, F&& continuation) &&
    -> Task<detail::task_unwrap_t<std::invoke_result_t<std::remove_reference_t<F>&, T>>>
  {
    auto chained = std::move(*this).then(std::forward<F>(continuation));
    auto environment = chained.environment();
    environment.scheduler = &scheduler;
    chained.set_environment(std::move(environment));
    return chained;
  }

  template<class F>
    requires (!detail::is_expected_v<T> && std::invocable<std::remove_reference_t<F>&, std::exception_ptr>)
  auto catching(F&& handler) && -> Task<T>
  {
    using handler_type = std::remove_cvref_t<F>;
    using handler_result = std::invoke_result_t<handler_type&, std::exception_ptr>;

    static_assert(
      std::same_as<handler_result, T> || detail::is_task_of_v<handler_result, T>,
      "Task<T>::catching expects a handler returning T or Task<T>");

    return catching_impl<handler_type, handler_result>(
      std::move(*this),
      handler_type(std::forward<F>(handler)));
  }

  template<class F>
    requires (!detail::is_expected_v<T> && std::invocable<std::remove_reference_t<F>&>)
  auto finally(F&& finalizer) && -> Task<T>
  {
    using finalizer_type = std::remove_cvref_t<F>;
    using finalizer_result = std::invoke_result_t<finalizer_type&>;

    static_assert(
      std::same_as<finalizer_result, void> || detail::is_task_of_v<finalizer_result, void>,
      "Task<T>::finally expects a finalizer returning void or Task<void>");

    return finally_impl<finalizer_type, finalizer_result>(
      std::move(*this),
      finalizer_type(std::forward<F>(finalizer)));
  }

  template<class F>
    requires (detail::is_expected_v<T>
      && ((std::same_as<detail::expected_value_t<T>, void> && std::invocable<std::remove_reference_t<F>&>)
        || (!std::same_as<detail::expected_value_t<T>, void>
          && std::invocable<std::remove_reference_t<F>&, detail::expected_value_t<T>>)))
  auto transform(F&& continuation) &&
    -> Task<detail::expected_rebind_t<T, detail::expected_transform_result_t<T, std::remove_reference_t<F>>>>
  {
    using continuation_type = std::remove_cvref_t<F>;
    using raw_result = detail::expected_transform_result_t<T, continuation_type>;

    static_assert(
      !detail::is_expected_v<raw_result> && !detail::is_task_v<raw_result>,
      "ResultTask::transform expects a synchronous value-mapper returning a plain value");

    return transform_impl<continuation_type, raw_result>(
      std::move(*this),
      continuation_type(std::forward<F>(continuation)));
  }

  template<class F>
    requires (detail::is_expected_v<T>
      && std::invocable<std::remove_reference_t<F>&, detail::expected_error_t<T>>)
  auto or_else(F&& handler) && -> Task<T>
  {
    using handler_type = std::remove_cvref_t<F>;
    using handler_result = std::invoke_result_t<handler_type&, detail::expected_error_t<T>>;

    static_assert(
      std::same_as<std::remove_cvref_t<handler_result>, T>,
      "ResultTask::or_else expects a synchronous handler returning the same expected type");

    return or_else_impl<handler_type>(
      std::move(*this),
      handler_type(std::forward<F>(handler)));
  }

  template<class F>
    requires (detail::is_expected_v<T>
      && ((std::same_as<detail::expected_value_t<T>, void> && std::invocable<std::remove_reference_t<F>&>)
        || (!std::same_as<detail::expected_value_t<T>, void>
          && std::invocable<std::remove_reference_t<F>&, detail::expected_value_t<T>>)))
  auto then_value(F&& continuation) &&
    -> Task<detail::expected_rebind_t<T, detail::expected_transform_result_t<T, std::remove_reference_t<F>>>>
  {
    return std::move(*this).transform(std::forward<F>(continuation));
  }

  template<class F>
    requires (detail::is_expected_v<T>
      && ((std::same_as<detail::expected_value_t<T>, void> && std::invocable<std::remove_reference_t<F>&>)
        || (!std::same_as<detail::expected_value_t<T>, void>
          && std::invocable<std::remove_reference_t<F>&, detail::expected_value_t<T>>)))
  auto then_value_on(Scheduler& scheduler, F&& continuation) &&
    -> Task<detail::expected_rebind_t<T, detail::expected_transform_result_t<T, std::remove_reference_t<F>>>>
  {
    auto chained = std::move(*this).then_value(std::forward<F>(continuation));
    auto environment = chained.environment();
    environment.scheduler = &scheduler;
    chained.set_environment(std::move(environment));
    return chained;
  }

  template<class F>
    requires (detail::is_expected_v<T>
      && std::invocable<std::remove_reference_t<F>&, detail::expected_error_t<T>>)
  auto then_error(F&& handler) && -> Task<T>
  {
    return std::move(*this).or_else(std::forward<F>(handler));
  }

  class promise_type
  {
  public:
    promise_type() noexcept
      : environment_(current_task_environment_value())
    {
    }

    static void* operator new(std::size_t bytes)
    {
      return detail::allocate_task_frame(bytes, alignof(promise_type));
    }

    static void operator delete(void* ptr) noexcept
    {
      detail::deallocate_task_frame(ptr);
    }

    static void operator delete(void* ptr, std::size_t) noexcept
    {
      detail::deallocate_task_frame(ptr);
    }

    Task get_return_object() noexcept
    {
      return Task{handle_type::from_promise(*this)};
    }

    std::suspend_always initial_suspend() const noexcept
    {
      return {};
    }

    struct final_awaitable
    {
      [[nodiscard]] bool await_ready() const noexcept
      {
        return false;
      }

      std::coroutine_handle<> await_suspend(handle_type handle) const noexcept
      {
        auto& promise = handle.promise();

        if (promise.wait_latch_)
          promise.wait_latch_->count_down();

        if (promise.continuation_)
          return promise.continuation_;

        return std::noop_coroutine();
      }

      void await_resume() const noexcept
      {
      }
    };

    final_awaitable final_suspend() const noexcept
    {
      return {};
    }

    void return_value(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
    {
      value_.emplace(std::move(value));
    }

    template<class U>
      requires (!std::same_as<std::remove_cvref_t<U>, T> && std::constructible_from<T, U&&>)
    void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)
    {
      value_.emplace(std::forward<U>(value));
    }

    void unhandled_exception() noexcept
    {
      exception_ = std::current_exception();
    }

    [[nodiscard]] TaskEnvironment environment() const noexcept
    {
      return environment_;
    }

    void set_environment(TaskEnvironment environment) noexcept
    {
      environment_ = std::move(environment);

      if (!environment_.frame_resource)
        environment_.frame_resource = memory::get_default_resource();
    }

    void seed_environment_if_missing() noexcept
    {
      auto current = current_task_environment_value();
      detail::merge_environment(environment_, current);
    }

    void inherit_environment(const TaskEnvironment& parent) noexcept
    {
      detail::merge_environment(environment_, parent);
    }

    template<class Promise>
    void inherit_from(std::coroutine_handle<Promise> parent) noexcept
    {
      if constexpr (requires(Promise& promise) { promise.environment(); })
        inherit_environment(parent.promise().environment());
    }

  private:
    friend class Task;

    std::optional<T> value_;
    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_;
    std::latch* wait_latch_ = nullptr;
    TaskEnvironment environment_;
    bool started_ = false;
  };

private:
  template<class Continuation, class RawResult, class Result>
  static Task<Result> then_impl(Task<T> task, Continuation continuation)
  {
    auto value = co_await task;

    if constexpr (detail::is_task_v<RawResult>)
    {
      if constexpr (std::is_void_v<Result>)
      {
        co_await std::invoke(continuation, std::move(value));
        co_return;
      }
      else
      {
        co_return co_await std::invoke(continuation, std::move(value));
      }
    }
    else
    {
      if constexpr (std::is_void_v<RawResult>)
      {
        std::invoke(continuation, std::move(value));
        co_return;
      }
      else
      {
        co_return std::invoke(continuation, std::move(value));
      }
    }
  }

  template<class Handler, class HandlerResult>
  static Task<T> catching_impl(Task<T> task, Handler handler)
  {
    std::exception_ptr error;

    try
    {
      co_return co_await task;
    }
    catch (...)
    {
      error = std::current_exception();
    }

    if constexpr (detail::is_task_v<HandlerResult>)
      co_return co_await std::invoke(handler, error);
    else
      co_return std::invoke(handler, error);
  }

  template<class Finalizer, class FinalizerResult>
  static Task<T> finally_impl(Task<T> task, Finalizer finalizer)
  {
    std::exception_ptr error;
    std::optional<T> value;

    try
    {
      value.emplace(co_await task);
    }
    catch (...)
    {
      error = std::current_exception();
    }

    if constexpr (detail::is_task_v<FinalizerResult>)
      co_await std::invoke(finalizer);
    else
      std::invoke(finalizer);

    if (error)
      std::rethrow_exception(error);

    co_return std::move(*value);
  }

  template<class Continuation, class Result>
  static Task<detail::expected_rebind_t<T, Result>> transform_impl(Task<T> task, Continuation continuation)
  {
    using expected_type = T;
    using value_type = detail::expected_value_t<expected_type>;
    using mapped_type = detail::expected_rebind_t<expected_type, Result>;

    auto expected = co_await task;

    if (!expected)
      co_return mapped_type(std::unexpected(std::move(expected.error())));

    if constexpr (std::same_as<value_type, void>)
    {
      if constexpr (std::same_as<Result, void>)
      {
        std::invoke(continuation);
        co_return mapped_type{};
      }
      else
      {
        co_return mapped_type(std::invoke(continuation));
      }
    }
    else
    {
      if constexpr (std::same_as<Result, void>)
      {
        std::invoke(continuation, std::move(*expected));
        co_return mapped_type{};
      }
      else
      {
        co_return mapped_type(std::invoke(continuation, std::move(*expected)));
      }
    }
  }

  template<class Handler>
  static Task<T> or_else_impl(Task<T> task, Handler handler)
  {
    auto expected = co_await task;

    if (expected)
      co_return expected;

    co_return std::invoke(handler, std::move(expected.error()));
  }

  promise_type& state_or_throw() const
  {
    if (!handle_)
      throw std::logic_error("invalid task");

    return handle_.promise();
  }

  handle_type handle_{};
};

template<>
class Task<void>
{
public:
  class promise_type;
  using value_type = void;
  using handle_type = std::coroutine_handle<promise_type>;

  Task() noexcept = default;

  explicit Task(handle_type handle) noexcept
    : handle_(handle)
  {
  }

  Task(Task&& other) noexcept
    : handle_(std::exchange(other.handle_, {}))
  {
  }

  Task& operator=(Task&& other) noexcept
  {
    if (this != &other)
    {
      if (handle_)
        handle_.destroy();

      handle_ = std::exchange(other.handle_, {});
    }

    return *this;
  }

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  ~Task()
  {
    if (handle_)
      handle_.destroy();
  }

  [[nodiscard]] bool valid() const noexcept
  {
    return static_cast<bool>(handle_);
  }

  [[nodiscard]] bool done() const noexcept
  {
    return !handle_ || handle_.done();
  }

  void start() noexcept
  {
    if (!handle_ || handle_.done())
      return;

    auto& promise = handle_.promise();
    promise.seed_environment_if_missing();

    if (!promise.started_)
    {
      promise.started_ = true;
      handle_.resume();
    }
  }

  void detach() noexcept
  {
    if (handle_)
      handle_.destroy();

    handle_ = {};
  }

  [[nodiscard]] bool await_ready() const noexcept
  {
    return done();
  }

  template<class Promise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> awaiting) noexcept
  {
    if (!handle_)
      return std::noop_coroutine();

    auto& promise = handle_.promise();
    promise.continuation_ = awaiting;
    promise.inherit_from(awaiting);
    promise.seed_environment_if_missing();

    if (!promise.started_)
    {
      promise.started_ = true;
      return handle_;
    }

    return std::noop_coroutine();
  }

  void await_resume()
  {
    auto& promise = state_or_throw();

    if (promise.exception_)
      std::rethrow_exception(promise.exception_);
  }

  void sync_wait()
  {
    auto& promise = state_or_throw();

    if (!handle_.done())
    {
      std::latch done(1);
      promise.wait_latch_ = &done;
      start();

      if (!handle_.done())
        done.wait();

      promise.wait_latch_ = nullptr;
    }

    await_resume();
  }

  void get()
  {
    sync_wait();
  }

  [[nodiscard]] handle_type native_handle() const noexcept
  {
    return handle_;
  }

  [[nodiscard]] TaskEnvironment environment() const noexcept
  {
    return handle_ ? handle_.promise().environment() : TaskEnvironment{};
  }

  void set_environment(TaskEnvironment environment) noexcept
  {
    if (handle_)
      handle_.promise().set_environment(std::move(environment));
  }

  template<detail::trace_context_like TraceLike>
  void set_trace_context(const TraceLike& trace) noexcept
  {
    auto environment = this->environment();
    environment.trace_context = detail::to_trace_context(trace);
    set_environment(std::move(environment));
  }

  [[nodiscard]] std::optional<TraceContext> trace_context() const noexcept
  {
    return environment().trace_context;
  }

  template<class F>
    requires std::invocable<std::remove_reference_t<F>&>
  auto then(F&& continuation) &&
    -> Task<detail::task_unwrap_t<std::invoke_result_t<std::remove_reference_t<F>&>>>
  {
    using continuation_type = std::remove_cvref_t<F>;
    using raw_result = std::invoke_result_t<continuation_type&>;
    using result_type = detail::task_unwrap_t<raw_result>;

    return then_impl<continuation_type, raw_result, result_type>(
      std::move(*this),
      continuation_type(std::forward<F>(continuation)));
  }

  template<class F>
    requires std::invocable<std::remove_reference_t<F>&>
  auto then_on(Scheduler& scheduler, F&& continuation) &&
    -> Task<detail::task_unwrap_t<std::invoke_result_t<std::remove_reference_t<F>&>>>
  {
    auto chained = std::move(*this).then(std::forward<F>(continuation));
    auto environment = chained.environment();
    environment.scheduler = &scheduler;
    chained.set_environment(std::move(environment));
    return chained;
  }

  template<class F>
    requires std::invocable<std::remove_reference_t<F>&, std::exception_ptr>
  auto catching(F&& handler) && -> Task<void>
  {
    using handler_type = std::remove_cvref_t<F>;
    using handler_result = std::invoke_result_t<handler_type&, std::exception_ptr>;

    static_assert(
      std::same_as<handler_result, void> || detail::is_task_of_v<handler_result, void>,
      "Task<void>::catching expects a handler returning void or Task<void>");

    return catching_impl<handler_type, handler_result>(
      std::move(*this),
      handler_type(std::forward<F>(handler)));
  }

  template<class F>
    requires std::invocable<std::remove_reference_t<F>&>
  auto finally(F&& finalizer) && -> Task<void>
  {
    using finalizer_type = std::remove_cvref_t<F>;
    using finalizer_result = std::invoke_result_t<finalizer_type&>;

    static_assert(
      std::same_as<finalizer_result, void> || detail::is_task_of_v<finalizer_result, void>,
      "Task<void>::finally expects a finalizer returning void or Task<void>");

    return finally_impl<finalizer_type, finalizer_result>(
      std::move(*this),
      finalizer_type(std::forward<F>(finalizer)));
  }

  class promise_type
  {
  public:
    promise_type() noexcept
      : environment_(current_task_environment_value())
    {
    }

    static void* operator new(std::size_t bytes)
    {
      return detail::allocate_task_frame(bytes, alignof(promise_type));
    }

    static void operator delete(void* ptr) noexcept
    {
      detail::deallocate_task_frame(ptr);
    }

    static void operator delete(void* ptr, std::size_t) noexcept
    {
      detail::deallocate_task_frame(ptr);
    }

    Task get_return_object() noexcept
    {
      return Task{handle_type::from_promise(*this)};
    }

    std::suspend_always initial_suspend() const noexcept
    {
      return {};
    }

    struct final_awaitable
    {
      [[nodiscard]] bool await_ready() const noexcept
      {
        return false;
      }

      std::coroutine_handle<> await_suspend(handle_type handle) const noexcept
      {
        auto& promise = handle.promise();

        if (promise.wait_latch_)
          promise.wait_latch_->count_down();

        if (promise.continuation_)
          return promise.continuation_;

        return std::noop_coroutine();
      }

      void await_resume() const noexcept
      {
      }
    };

    final_awaitable final_suspend() const noexcept
    {
      return {};
    }

    void return_void() noexcept
    {
    }

    void unhandled_exception() noexcept
    {
      exception_ = std::current_exception();
    }

    [[nodiscard]] TaskEnvironment environment() const noexcept
    {
      return environment_;
    }

    void set_environment(TaskEnvironment environment) noexcept
    {
      environment_ = std::move(environment);

      if (!environment_.frame_resource)
        environment_.frame_resource = memory::get_default_resource();
    }

    void seed_environment_if_missing() noexcept
    {
      auto current = current_task_environment_value();
      detail::merge_environment(environment_, current);
    }

    void inherit_environment(const TaskEnvironment& parent) noexcept
    {
      detail::merge_environment(environment_, parent);
    }

    template<class Promise>
    void inherit_from(std::coroutine_handle<Promise> parent) noexcept
    {
      if constexpr (requires(Promise& promise) { promise.environment(); })
        inherit_environment(parent.promise().environment());
    }

  private:
    friend class Task;

    std::exception_ptr exception_;
    std::coroutine_handle<> continuation_;
    std::latch* wait_latch_ = nullptr;
    TaskEnvironment environment_;
    bool started_ = false;
  };

private:
  template<class Continuation, class RawResult, class Result>
  static Task<Result> then_impl(Task<void> task, Continuation continuation)
  {
    co_await task;

    if constexpr (detail::is_task_v<RawResult>)
    {
      if constexpr (std::is_void_v<Result>)
      {
        co_await std::invoke(continuation);
        co_return;
      }
      else
      {
        co_return co_await std::invoke(continuation);
      }
    }
    else
    {
      if constexpr (std::is_void_v<RawResult>)
      {
        std::invoke(continuation);
        co_return;
      }
      else
      {
        co_return std::invoke(continuation);
      }
    }
  }

  template<class Handler, class HandlerResult>
  static Task<void> catching_impl(Task<void> task, Handler handler)
  {
    std::exception_ptr error;

    try
    {
      co_await task;
      co_return;
    }
    catch (...)
    {
      error = std::current_exception();
    }

    if constexpr (detail::is_task_v<HandlerResult>)
      co_await std::invoke(handler, error);
    else
      std::invoke(handler, error);
  }

  template<class Finalizer, class FinalizerResult>
  static Task<void> finally_impl(Task<void> task, Finalizer finalizer)
  {
    std::exception_ptr error;

    try
    {
      co_await task;
    }
    catch (...)
    {
      error = std::current_exception();
    }

    if constexpr (detail::is_task_v<FinalizerResult>)
      co_await std::invoke(finalizer);
    else
      std::invoke(finalizer);

    if (error)
      std::rethrow_exception(error);
  }

  promise_type& state_or_throw() const
  {
    if (!handle_)
      throw std::logic_error("invalid task");

    return handle_.promise();
  }

  handle_type handle_{};
};

template<class T, class E>
using ResultTask = Task<std::expected<T, E>>;

template<class E>
using StatusTask = Task<std::expected<void, E>>;
} // namespace modern::runtime