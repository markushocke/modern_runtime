module;

#include "../detail/shared_ptr.hpp"

#include <atomic>
#include <concepts>
#include <exception>
#include <functional>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>

export module modern.runtime:io_bridge;

export import modern.exec;
export import modern.memory;
export import modern.task;

import modern.task.detail;

namespace modern::detail
{
template<class T>
inline constexpr bool always_false = false;

template<class R>
class io_task_completion_state
{
public:
  explicit io_task_completion_state(std::shared_ptr<shared_state<R>> result_state) noexcept
    : result_state_(std::move(result_state))
  {
  }

  void set_value(R&& value)
  {
    if (try_complete())
      result_state_->set_value(std::move(value));
  }

  void set_exception(std::exception_ptr exception)
  {
    if (try_complete())
      result_state_->set_exception(std::move(exception));
  }

  void cancel()
  {
    if (try_complete())
      cancel_state(result_state_);
  }

private:
  [[nodiscard]] bool try_complete() noexcept
  {
    return !completed_.exchange(true, std::memory_order_acq_rel);
  }

  std::shared_ptr<shared_state<R>> result_state_;
  std::atomic<bool> completed_ = false;
};

template<>
class io_task_completion_state<void>
{
public:
  explicit io_task_completion_state(std::shared_ptr<shared_state<void>> result_state) noexcept
    : result_state_(std::move(result_state))
  {
  }

  void set_value()
  {
    if (try_complete())
      result_state_->set_value();
  }

  void set_exception(std::exception_ptr exception)
  {
    if (try_complete())
      result_state_->set_exception(std::move(exception));
  }

  void cancel()
  {
    if (try_complete())
      cancel_state(result_state_);
  }

private:
  [[nodiscard]] bool try_complete() noexcept
  {
    return !completed_.exchange(true, std::memory_order_acq_rel);
  }

  std::shared_ptr<shared_state<void>> result_state_;
  std::atomic<bool> completed_ = false;
};

template<class Service>
scheduler io_bridge_scheduler(Service& service)
{
  if constexpr (requires(Service& candidate)
  {
    { candidate.target_scheduler() } -> std::convertible_to<scheduler>;
  })
  {
    return service.target_scheduler();
  }
  else if constexpr (requires(Service& candidate)
  {
    { candidate.get_scheduler() } -> std::convertible_to<scheduler>;
  })
  {
    return service.get_scheduler();
  }
  else if constexpr (requires(Service& candidate)
  {
    { candidate.scheduler() } -> std::convertible_to<scheduler>;
  })
  {
    return service.scheduler();
  }
  else
  {
    static_assert(always_false<Service>,
      "bind_io service must expose target_scheduler(), get_scheduler(), or scheduler() returning modern::scheduler");
    return {};
  }
}

template<class Service>
memory::memory_resource* io_bridge_resource(Service& service) noexcept
{
  if constexpr (requires(Service& candidate)
  {
    { candidate.resource() } -> std::convertible_to<memory::memory_resource*>;
  })
  {
    return service.resource();
  }
  else
  {
    return memory::get_default_resource();
  }
}
} // namespace modern::detail

export namespace modern
{
template<class T>
class io_task_completion
{
public:
  io_task_completion() = default;

  template<class State>
  explicit io_task_completion(std::shared_ptr<State> state)
    : state_(std::move(state)),
      set_value_([](void* erased_state, T&& value)
      {
        static_cast<State*>(erased_state)->set_value(std::move(value));
      }),
      set_exception_([](void* erased_state, std::exception_ptr exception)
      {
        static_cast<State*>(erased_state)->set_exception(std::move(exception));
      }),
      cancel_([](void* erased_state)
      {
        static_cast<State*>(erased_state)->cancel();
      })
  {
  }

  template<class U>
    requires std::convertible_to<U, T>
  void set_value(U&& value) const
  {
    validate();

    T converted(std::forward<U>(value));
    set_value_(state_.get(), std::move(converted));
  }

  void set_exception(std::exception_ptr exception) const
  {
    validate();
    set_exception_(state_.get(), std::move(exception));
  }

  void cancel() const
  {
    validate();
    cancel_(state_.get());
  }

  [[nodiscard]] bool valid() const noexcept
  {
    return static_cast<bool>(state_);
  }

private:
  using set_value_fn = void (*)(void*, T&&);
  using set_exception_fn = void (*)(void*, std::exception_ptr);
  using cancel_fn = void (*)(void*);

  void validate() const
  {
    if (!state_ || !set_value_ || !set_exception_ || !cancel_)
      throw std::runtime_error("empty io_task_completion");
  }

  std::shared_ptr<void> state_;
  set_value_fn set_value_ = nullptr;
  set_exception_fn set_exception_ = nullptr;
  cancel_fn cancel_ = nullptr;
};

template<>
class io_task_completion<void>
{
public:
  io_task_completion() = default;

  template<class State>
  explicit io_task_completion(std::shared_ptr<State> state)
    : state_(std::move(state)),
      set_value_([](void* erased_state)
      {
        static_cast<State*>(erased_state)->set_value();
      }),
      set_exception_([](void* erased_state, std::exception_ptr exception)
      {
        static_cast<State*>(erased_state)->set_exception(std::move(exception));
      }),
      cancel_([](void* erased_state)
      {
        static_cast<State*>(erased_state)->cancel();
      })
  {
  }

  void set_value() const
  {
    validate();
    set_value_(state_.get());
  }

  void set_exception(std::exception_ptr exception) const
  {
    validate();
    set_exception_(state_.get(), std::move(exception));
  }

  void cancel() const
  {
    validate();
    cancel_(state_.get());
  }

  [[nodiscard]] bool valid() const noexcept
  {
    return static_cast<bool>(state_);
  }

private:
  using set_value_fn = void (*)(void*);
  using set_exception_fn = void (*)(void*, std::exception_ptr);
  using cancel_fn = void (*)(void*);

  void validate() const
  {
    if (!state_ || !set_value_ || !set_exception_ || !cancel_)
      throw std::runtime_error("empty io_task_completion");
  }

  std::shared_ptr<void> state_;
  set_value_fn set_value_ = nullptr;
  set_exception_fn set_exception_ = nullptr;
  cancel_fn cancel_ = nullptr;
};

template<class R, class Start>
auto bind_io(scheduler completion_scheduler, memory::memory_resource* resource, Start&& start)
{
  using start_type = std::remove_cvref_t<Start>;

  static_assert(std::is_invocable_v<start_type&, io_task_completion<R>>,
    "bind_io start callable must accept modern::io_task_completion<R>");

  auto* actual_resource = resource ? resource : memory::get_default_resource();
  auto result_state = detail::make_shared_state<R>(completion_scheduler, actual_resource);
  auto result = detail::make_task<R>(result_state);
  auto completion_state = detail::allocate_shared_object<detail::io_task_completion_state<R>>(
    actual_resource,
    result_state);
  auto completion = io_task_completion<R>{completion_state};

  try
  {
    auto starter = start_type(std::forward<Start>(start));
    std::invoke(starter, std::move(completion));
  }
  catch (...)
  {
    completion_state->set_exception(std::current_exception());
  }

  return result;
}

template<class R, class Start>
auto bind_io(scheduler completion_scheduler, Start&& start)
{
  return modern::bind_io<R>(completion_scheduler, memory::get_default_resource(), std::forward<Start>(start));
}

template<class R, class Start>
auto bind_io(
  scheduler completion_scheduler,
  std::stop_token token,
  memory::memory_resource* resource,
  Start&& start)
{
  using start_type = std::remove_cvref_t<Start>;

  static_assert(
    std::is_invocable_v<start_type&, io_task_completion<R>, std::stop_token>
      || std::is_invocable_v<start_type&, io_task_completion<R>>,
    "bind_io stoppable start callable must accept modern::io_task_completion<R> with optional std::stop_token");

  auto* actual_resource = resource ? resource : memory::get_default_resource();
  auto result_state = detail::make_shared_state<R>(completion_scheduler, actual_resource);
  auto result = detail::make_task<R>(result_state);

  if (token.stop_requested())
  {
    detail::cancel_state(result_state);
    return result;
  }

  auto completion_state = detail::allocate_shared_object<detail::io_task_completion_state<R>>(
    actual_resource,
    result_state);
  auto completion = io_task_completion<R>{completion_state};

  try
  {
    auto starter = start_type(std::forward<Start>(start));

    if constexpr (std::is_invocable_v<start_type&, io_task_completion<R>, std::stop_token>)
      std::invoke(starter, std::move(completion), token);
    else
      std::invoke(starter, std::move(completion));
  }
  catch (...)
  {
    completion_state->set_exception(std::current_exception());
  }

  return result;
}

template<class R, class Start>
auto bind_io(scheduler completion_scheduler, std::stop_token token, Start&& start)
{
  return modern::bind_io<R>(
    completion_scheduler,
    token,
    memory::get_default_resource(),
    std::forward<Start>(start));
}

template<class R, class Service, class Start>
  requires (!std::same_as<std::remove_cvref_t<Service>, scheduler>)
auto bind_io(Service& service, memory::memory_resource* resource, Start&& start)
{
  return modern::bind_io<R>(
    detail::io_bridge_scheduler(service),
    resource ? resource : detail::io_bridge_resource(service),
    std::forward<Start>(start));
}

template<class R, class Service, class Start>
  requires (!std::same_as<std::remove_cvref_t<Service>, scheduler>)
auto bind_io(Service& service, Start&& start)
{
  return modern::bind_io<R>(
    detail::io_bridge_scheduler(service),
    detail::io_bridge_resource(service),
    std::forward<Start>(start));
}

template<class R, class Service, class Start>
  requires (!std::same_as<std::remove_cvref_t<Service>, scheduler>)
auto bind_io(Service& service, std::stop_token token, memory::memory_resource* resource, Start&& start)
{
  return modern::bind_io<R>(
    detail::io_bridge_scheduler(service),
    token,
    resource ? resource : detail::io_bridge_resource(service),
    std::forward<Start>(start));
}

template<class R, class Service, class Start>
  requires (!std::same_as<std::remove_cvref_t<Service>, scheduler>)
auto bind_io(Service& service, std::stop_token token, Start&& start)
{
  return modern::bind_io<R>(
    detail::io_bridge_scheduler(service),
    token,
    detail::io_bridge_resource(service),
    std::forward<Start>(start));
}
} // namespace modern