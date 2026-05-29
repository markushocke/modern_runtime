module;

#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
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

export namespace modern::runtime
{
using Scheduler = modern::scheduler;
using StopToken = std::stop_token;

struct TraceContext
{
  std::array<std::byte, 16> trace_id{};
  std::array<std::byte, 8> span_id{};
  std::uint8_t flags{};

  friend bool operator==(const TraceContext&, const TraceContext&) = default;
};

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
  else if constexpr (requires { trace.trace_flags; })
    context.flags = trace.trace_flags;

  return context;
}
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