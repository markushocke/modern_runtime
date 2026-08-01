module;

#include <coroutine>
#include <chrono>
#include <memory_resource>
#include <optional>
#include <stop_token>
#include <utility>

export module modern.task_environment;

export import modern.exec;
export import modern.memory;
export import modern.trace;

export namespace modern
{
class deadline
{
public:
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;

  constexpr deadline() noexcept = default;
  explicit constexpr deadline(time_point value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr deadline unbounded() noexcept { return {}; }
  [[nodiscard]] static constexpr deadline at(time_point value) noexcept { return deadline{value}; }

  template<class Rep, class Period>
  [[nodiscard]] static deadline after(std::chrono::duration<Rep, Period> value) noexcept
  {
    return at(clock::now() + std::chrono::duration_cast<clock::duration>(value));
  }

  [[nodiscard]] constexpr bool bounded() const noexcept { return value_.has_value(); }
  [[nodiscard]] constexpr const std::optional<time_point>& value() const noexcept { return value_; }
  [[nodiscard]] bool expired(time_point now = clock::now()) const noexcept
  {
    return value_ && now >= *value_;
  }

  friend constexpr bool operator==(const deadline&, const deadline&) noexcept = default;

private:
  std::optional<time_point> value_{};
};

[[nodiscard]] inline constexpr deadline earlier(deadline left, deadline right) noexcept
{
  if (!left.bounded())
    return right;
  if (!right.bounded())
    return left;
  return *left.value() <= *right.value() ? left : right;
}

struct task_environment
{
  scheduler scheduler{};
  std::pmr::memory_resource* frame_resource{};
  std::optional<trace::TraceContext> trace_context{};
  std::stop_token stop_token{};
  deadline execution_deadline{};
};

inline void merge_environment(task_environment& target, const task_environment& source) noexcept
{
  if (!target.scheduler.valid() && source.scheduler.valid())
    target.scheduler = source.scheduler;
  if (!target.frame_resource && source.frame_resource)
    target.frame_resource = source.frame_resource;
  if (!target.trace_context && source.trace_context)
    target.trace_context = source.trace_context;
  if (!target.stop_token.stop_possible() && source.stop_token.stop_possible())
    target.stop_token = source.stop_token;
  target.execution_deadline = earlier(target.execution_deadline, source.execution_deadline);
}

namespace detail
{
inline task_environment& task_environment_storage() noexcept
{
  thread_local task_environment value{};
  return value;
}

inline bool& task_environment_active_storage() noexcept
{
  thread_local bool active = false;
  return active;
}
} // namespace detail

inline task_environment current_task_environment_value() noexcept
{
  auto env = detail::task_environment_active_storage()
    ? detail::task_environment_storage()
    : task_environment{};

  if (!env.frame_resource)
    env.frame_resource = memory::get_default_resource();
  return env;
}

class task_environment_scope
{
public:
  explicit task_environment_scope(task_environment env) noexcept
    : previous_(detail::task_environment_storage()),
      was_active_(detail::task_environment_active_storage())
  {
    if (!env.frame_resource)
      env.frame_resource = memory::get_default_resource();
    detail::task_environment_storage() = std::move(env);
    detail::task_environment_active_storage() = true;
  }

  ~task_environment_scope() noexcept
  {
    detail::task_environment_storage() = std::move(previous_);
    detail::task_environment_active_storage() = was_active_;
  }

  task_environment_scope(const task_environment_scope&) = delete;
  task_environment_scope& operator=(const task_environment_scope&) = delete;

private:
  task_environment previous_;
  bool was_active_;
};

class frame_resource_scope
{
public:
  explicit frame_resource_scope(std::pmr::memory_resource* resource) noexcept
    : scope_([&]
      {
        auto env = current_task_environment_value();
        env.frame_resource = resource ? resource : memory::get_default_resource();
        return env;
      }())
  {
  }

private:
  task_environment_scope scope_;
};

class trace_context_scope
{
public:
  explicit trace_context_scope(const trace::TraceContext& trace) noexcept
    : scope_([&]
      {
        auto env = current_task_environment_value();
        env.trace_context = trace;
        return env;
      }())
  {
  }

private:
  task_environment_scope scope_;
};

namespace this_task
{
struct environment_query {};
struct scheduler_query {};
struct memory_resource_query {};
struct trace_context_query {};
struct stop_token_query {};
struct deadline_query {};

inline environment_query environment() noexcept { return {}; }
inline scheduler_query scheduler() noexcept { return {}; }
inline memory_resource_query memory_resource() noexcept { return {}; }
inline trace_context_query trace_context() noexcept { return {}; }
inline stop_token_query stop_token() noexcept { return {}; }
inline deadline_query deadline() noexcept { return {}; }
} // namespace this_task

namespace detail
{
template<class T>
struct ready_awaiter
{
  T value;
  bool await_ready() const noexcept { return true; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  T await_resume() noexcept { return std::move(value); }
};
} // namespace detail
} // namespace modern
