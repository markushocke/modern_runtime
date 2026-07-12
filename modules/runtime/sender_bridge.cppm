module;

#include "../detail/shared_ptr.hpp"

#include <atomic>
#include <concepts>
#include <exception>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

export module modern.runtime:sender_bridge;

export import modern.exec;
export import modern.memory;
export import modern.task;

import modern.task_detail;

namespace modern::detail
{
template<class T>
inline constexpr bool sender_bridge_false = false;

template<class Sender, class Receiver>
decltype(auto) connect_sender(Sender&& sender, Receiver&& receiver)
{
  if constexpr (requires(Sender&& candidate, Receiver&& target)
  {
    connect(std::forward<Sender>(candidate), std::forward<Receiver>(target));
  })
  {
    return connect(std::forward<Sender>(sender), std::forward<Receiver>(receiver));
  }
  else if constexpr (requires(Sender&& candidate, Receiver&& target)
  {
    std::forward<Sender>(candidate).connect(std::forward<Receiver>(target));
  })
  {
    return std::forward<Sender>(sender).connect(std::forward<Receiver>(receiver));
  }
  else
  {
    static_assert(sender_bridge_false<Sender>,
      "sender bridge requires connect(sender, receiver) or sender.connect(receiver)");
    return connect(std::forward<Sender>(sender), std::forward<Receiver>(receiver));
  }
}

template<class Operation>
void start_sender(Operation& operation)
{
  if constexpr (requires(Operation& candidate)
  {
    start(candidate);
  })
  {
    start(operation);
  }
  else if constexpr (requires(Operation& candidate)
  {
    candidate.start();
  })
  {
    operation.start();
  }
  else
  {
    static_assert(sender_bridge_false<Operation>,
      "sender bridge requires start(operation) or operation.start()");
  }
}

inline std::exception_ptr make_sender_error(std::exception_ptr error)
{
  return error ? error : std::make_exception_ptr(std::runtime_error("sender error"));
}

template<class Error>
std::exception_ptr make_sender_error(Error&& error)
{
  using error_type = std::remove_cvref_t<Error>;

  if constexpr (std::same_as<error_type, std::exception_ptr>)
  {
    return make_sender_error(std::forward<Error>(error));
  }
  else if constexpr (std::derived_from<error_type, std::exception>)
  {
    return std::make_exception_ptr(std::forward<Error>(error));
  }
  else if constexpr (std::same_as<error_type, std::error_code>)
  {
    return std::make_exception_ptr(std::system_error(error));
  }
  else
  {
    return std::make_exception_ptr(std::runtime_error("sender error"));
  }
}

template<class R>
class sender_task_state
{
public:
  explicit sender_task_state(std::shared_ptr<shared_state<R>> result_state) noexcept
    : result_state_(std::move(result_state))
  {
  }

  void keep_alive(std::shared_ptr<void> operation) noexcept
  {
    operation_ = std::move(operation);
  }

  template<class... Args>
  void set_value(Args&&... args)
  {
    if (!try_complete())
      return;

    auto keep_alive = std::move(operation_);

    try
    {
      static_assert(sizeof...(Args) == 1,
        "sender bridge currently supports exactly one value for non-void task results");
      result_state_->set_value(R(std::forward<Args>(args)...));
    }
    catch (...)
    {
      result_state_->set_exception(std::current_exception());
    }
  }

  template<class Error>
  void set_error(Error&& error)
  {
    if (!try_complete())
      return;

    auto keep_alive = std::move(operation_);
    result_state_->set_exception(make_sender_error(std::forward<Error>(error)));
  }

  void set_stopped()
  {
    if (!try_complete())
      return;

    auto keep_alive = std::move(operation_);
    cancel_state(result_state_);
  }

private:
  [[nodiscard]] bool try_complete() noexcept
  {
    return !completed_.exchange(true, std::memory_order_acq_rel);
  }

  std::shared_ptr<shared_state<R>> result_state_;
  std::shared_ptr<void> operation_;
  std::atomic<bool> completed_ = false;
};

template<>
class sender_task_state<void>
{
public:
  explicit sender_task_state(std::shared_ptr<shared_state<void>> result_state) noexcept
    : result_state_(std::move(result_state))
  {
  }

  void keep_alive(std::shared_ptr<void> operation) noexcept
  {
    operation_ = std::move(operation);
  }

  template<class... Args>
  void set_value(Args&&...)
  {
    if (!try_complete())
      return;

    auto keep_alive = std::move(operation_);
    result_state_->set_value();
  }

  template<class Error>
  void set_error(Error&& error)
  {
    if (!try_complete())
      return;

    auto keep_alive = std::move(operation_);
    result_state_->set_exception(make_sender_error(std::forward<Error>(error)));
  }

  void set_stopped()
  {
    if (!try_complete())
      return;

    auto keep_alive = std::move(operation_);
    cancel_state(result_state_);
  }

private:
  [[nodiscard]] bool try_complete() noexcept
  {
    return !completed_.exchange(true, std::memory_order_acq_rel);
  }

  std::shared_ptr<shared_state<void>> result_state_;
  std::shared_ptr<void> operation_;
  std::atomic<bool> completed_ = false;
};

template<class R>
class task_receiver
{
public:
  explicit task_receiver(std::shared_ptr<sender_task_state<R>> state) noexcept
    : state_(std::move(state))
  {
  }

  template<class... Args>
  void set_value(Args&&... args) &&
  {
    state_->set_value(std::forward<Args>(args)...);
  }

  template<class Error>
  void set_error(Error&& error) &&
  {
    state_->set_error(std::forward<Error>(error));
  }

  void set_stopped() &&
  {
    state_->set_stopped();
  }

  template<class... Args>
  friend void set_value(task_receiver&& receiver, Args&&... args)
  {
    receiver.state_->set_value(std::forward<Args>(args)...);
  }

  template<class Error>
  friend void set_error(task_receiver&& receiver, Error&& error)
  {
    receiver.state_->set_error(std::forward<Error>(error));
  }

  friend void set_stopped(task_receiver&& receiver)
  {
    receiver.state_->set_stopped();
  }

private:
  std::shared_ptr<sender_task_state<R>> state_;
};
} // namespace modern::detail

export namespace modern
{
template<class R, class Sender>
auto as_task(Sender&& sender, scheduler completion_scheduler, memory::memory_resource* resource)
{
  using sender_type = std::remove_cvref_t<Sender>;
  using receiver_type = detail::task_receiver<R>;
  using operation_type = std::remove_cvref_t<decltype(detail::connect_sender(
    std::declval<sender_type>(),
    std::declval<receiver_type>()))>;

  auto* actual_resource = resource ? resource : memory::get_default_resource();
  auto result_state = detail::make_shared_state<R>(completion_scheduler, actual_resource);
  auto result = detail::make_task<R>(result_state);
  auto bridge_state = detail::allocate_shared_object<detail::sender_task_state<R>>(
    actual_resource,
    result_state);

  try
  {
    auto receiver = receiver_type{bridge_state};
    auto operation = detail::allocate_shared_object<operation_type>(
      actual_resource,
      detail::connect_sender(sender_type(std::forward<Sender>(sender)), std::move(receiver)));
    bridge_state->keep_alive(operation);
    detail::start_sender(*operation);
  }
  catch (...)
  {
    bridge_state->set_error(std::current_exception());
  }

  return result;
}

template<class R, class Sender>
auto as_task(Sender&& sender, scheduler completion_scheduler)
{
  return modern::as_task<R>(
    std::forward<Sender>(sender),
    std::move(completion_scheduler),
    memory::get_default_resource());
}

template<class R, class Sender>
auto as_task(Sender&& sender)
{
  return modern::as_task<R>(
    std::forward<Sender>(sender),
    inline_scheduler(),
    memory::get_default_resource());
}
} // namespace modern
