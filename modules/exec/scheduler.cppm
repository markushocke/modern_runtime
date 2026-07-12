module;

#include "../detail/move_only_function_support.hpp"

#include <concepts>
#include <functional>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>
#include <stdexcept>

export module modern.exec:scheduler;

export namespace modern
{
enum class scheduler_priority
{
  high,
  normal,
  low,
};

class scheduler
{
public:
  scheduler() = default;

  template<class Executor>
    requires requires(Executor& executor, detail::move_only_function task)
    {
      executor.execute(std::move(task));
    }
  explicit scheduler(std::shared_ptr<Executor> impl)
    : impl_(std::move(impl))
    , execute_([](void* erased_impl, detail::move_only_function task, scheduler_priority priority)
      {
        if constexpr (requires(Executor& executor, detail::move_only_function prioritized_task)
          {
            executor.execute(std::move(prioritized_task), scheduler_priority::normal);
          })
        {
          static_cast<Executor*>(erased_impl)->execute(std::move(task), priority);
        }
        else
        {
          static_cast<Executor*>(erased_impl)->execute(std::move(task));
        }
      })
    , resource_([](const void* erased_impl) noexcept -> std::pmr::memory_resource*
      {
        if constexpr (requires(const Executor& executor)
          {
            { executor.resource() } -> std::convertible_to<std::pmr::memory_resource*>;
          })
        {
          return static_cast<const Executor*>(erased_impl)->resource();
        }
        else
        {
          return std::pmr::get_default_resource();
        }
      })
  {
  }

  template<class F>
    requires std::invocable<std::remove_cvref_t<F>&>
  void execute(F&& task) const
  {
    execute_impl(detail::move_only_function{resource(), std::forward<F>(task)}, scheduler_priority::normal);
  }

  template<class F>
    requires std::invocable<std::remove_cvref_t<F>&>
  void execute(scheduler_priority priority, F&& task) const
  {
    execute_impl(detail::move_only_function{resource(), std::forward<F>(task)}, priority);
  }

  [[nodiscard]] bool valid() const noexcept
  {
    return static_cast<bool>(impl_);
  }

private:
  using execute_fn = void (*)(void*, detail::move_only_function, scheduler_priority);
  using resource_fn = std::pmr::memory_resource* (*)(const void*) noexcept;

  [[nodiscard]] std::pmr::memory_resource* resource() const noexcept
  {
    return impl_ && resource_ ? resource_(impl_.get()) : std::pmr::get_default_resource();
  }

  void execute_impl(detail::move_only_function task, scheduler_priority priority) const;

  std::shared_ptr<void> impl_;
  execute_fn execute_ = nullptr;
  resource_fn resource_ = nullptr;
};

[[nodiscard]] scheduler inline_scheduler();
} // namespace modern

namespace modern::detail
{
class inline_scheduler_executor
{
public:
  void execute(move_only_function task)
  {
    task();
  }

  [[nodiscard]] std::pmr::memory_resource* resource() const noexcept
  {
    return std::pmr::get_default_resource();
  }
};
} // namespace modern::detail

namespace modern
{
inline void scheduler::execute_impl(detail::move_only_function task, scheduler_priority priority) const
{
  if (!impl_ || !execute_)
    throw std::logic_error("scheduler has no target");

  execute_(impl_.get(), std::move(task), priority);
}

inline scheduler inline_scheduler()
{
  static auto instance = std::make_shared<detail::inline_scheduler_executor>();
  return scheduler{instance};
}
} // namespace modern
