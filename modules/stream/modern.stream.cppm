module;

#include "../detail/move_only_function_support.hpp"
#include "../detail/shared_ptr.hpp"

#include <chrono>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <deque>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

export module modern.stream;

export import modern.task;

export namespace modern
{
enum class stream_error_code
{
  closed,
  producer_failed,
  cancelled,
  consumer_gone,
  scheduler_stopped,
  protocol_error
};

struct stream_error
{
  stream_error_code code = stream_error_code::protocol_error;
  std::string message;
  std::exception_ptr cause;
};

template<class T>
using stream_result = std::expected<std::optional<T>, stream_error>;

enum class stream_terminal_state
{
  closed,
  failed,
  cancelled
};

struct stream_completion
{
  stream_terminal_state state = stream_terminal_state::closed;
  std::size_t produced_items = 0;
  std::size_t consumed_items = 0;
  std::chrono::nanoseconds duration{};
  std::optional<stream_error> error;
};

template<class T>
class stream;

template<class T>
class stream_source;
}

namespace modern::detail
{
inline stream_error make_stream_error(
  stream_error_code code,
  std::string message,
  std::exception_ptr cause = {})
{
  return stream_error{code, std::move(message), std::move(cause)};
}

inline stream_error producer_exception_error(std::exception_ptr cause)
{
  auto message = std::string{"stream producer failed"};
  if (cause)
  {
    try
    {
      std::rethrow_exception(cause);
    }
    catch (const std::exception& error)
    {
      message = error.what();
    }
    catch (...)
    {
    }
  }
  return make_stream_error(
    stream_error_code::producer_failed, std::move(message), std::move(cause));
}

template<class R>
struct task_completion
{
  std::shared_ptr<shared_state<R>> state;
  task<R> result;
};

template<class R>
task_completion<R> make_stream_task()
{
  auto environment = current_task_environment_value();
  if (!environment.scheduler.valid())
    environment.scheduler = inline_scheduler();
  if (!environment.frame_resource)
    environment.frame_resource = memory::get_default_resource();

  auto state = make_shared_state<R>(
    environment.scheduler,
    environment.frame_resource,
    std::make_shared<task_environment>(environment));
  return {state, make_task<R>(state)};
}

template<class R>
void set_task_value_noexcept(const std::shared_ptr<shared_state<R>>& state, R value) noexcept
{
  if (!state)
    return;
  try
  {
    state->set_value(std::move(value));
  }
  catch (...)
  {
    auto exception = std::current_exception();
    try
    {
      if (!state->done())
        state->set_exception(std::move(exception));
    }
    catch (...)
    {
    }
  }
}

struct stream_observer
{
  scheduler executor;
  std::function<void(const stream_completion&)> callback;
};

inline void dispatch_stream_observer(
  stream_observer observer,
  stream_completion completion) noexcept
{
  struct invocation_state
  {
    invocation_state(
      std::function<void(const stream_completion&)> callback_value,
      stream_completion completion_value)
      : callback(std::move(callback_value)),
        completion(std::move(completion_value))
    {
    }

    std::atomic<bool> invoked = false;
    std::function<void(const stream_completion&)> callback;
    stream_completion completion;
  };

  std::shared_ptr<invocation_state> state;
  try
  {
    state = std::make_shared<invocation_state>(
      std::move(observer.callback), std::move(completion));
  }
  catch (...)
  {
    try
    {
      observer.callback(completion);
    }
    catch (...)
    {
    }
    return;
  }

  auto invoke_state = [](const std::shared_ptr<invocation_state>& current) noexcept
  {
    if (current->invoked.exchange(true, std::memory_order_acq_rel))
      return;
    try
    {
      current->callback(current->completion);
    }
    catch (...)
    {
    }
  };

  if (observer.executor.valid())
  {
    try
    {
      observer.executor.execute([state, invoke_state]() noexcept
      {
        invoke_state(state);
      });
      return;
    }
    catch (...)
    {
    }
  }
  invoke_state(state);
}

template<class T>
class stream_state
{
public:
  using next_result = stream_result<T>;
  using send_result = std::expected<void, stream_error>;

  stream_state(
    std::size_t capacity,
    memory::memory_resource* resource,
    task_environment environment)
    : capacity_(capacity),
      resource_(resource ? resource : memory::get_default_resource()),
      buffer_(resource_),
      observers_(resource_),
      environment_(std::move(environment)),
      started_at_(std::chrono::steady_clock::now())
  {
    if (capacity_ == 0)
      throw std::invalid_argument("stream capacity must be at least one");
    if (!environment_.scheduler.valid())
      environment_.scheduler = inline_scheduler();
    if (!environment_.frame_resource)
      environment_.frame_resource = resource_;
  }

  task<next_result> next()
  {
    auto completion = make_stream_task<next_result>();
    auto result = std::move(completion.result);
    std::shared_ptr<shared_state<next_result>> reader_to_complete;
    std::optional<next_result> reader_value;
    std::shared_ptr<shared_state<send_result>> writer_to_complete;
    std::optional<send_result> writer_value;
    std::vector<std::pair<stream_observer, stream_completion>> observers;

    {
      std::lock_guard lock(mutex_);

      if (pending_reader_)
      {
        reader_to_complete = completion.state;
        reader_value.emplace(std::unexpected(make_stream_error(
          stream_error_code::protocol_error,
          "only one next() operation may be pending")));
      }
      else if (!buffer_.empty())
      {
        auto value = std::move(buffer_.front());
        buffer_.pop_front();
        ++consumed_items_;
        reader_to_complete = completion.state;
        reader_value.emplace(std::optional<T>{std::move(value)});

        if (pending_writer_ && lifecycle_ == lifecycle::open)
        {
          buffer_.push_back(std::move(pending_writer_->value));
          ++produced_items_;
          writer_to_complete = std::move(pending_writer_->completion);
          writer_value.emplace();
          pending_writer_.reset();
        }

        if (lifecycle_ == lifecycle::closing && buffer_.empty())
        {
          lifecycle_ = lifecycle::closed;
          observers = finish_locked();
        }
      }
      else if (lifecycle_ == lifecycle::closing)
      {
        lifecycle_ = lifecycle::closed;
        reader_to_complete = completion.state;
        reader_value.emplace(std::optional<T>{});
        observers = finish_locked();
      }
      else if (lifecycle_ == lifecycle::closed)
      {
        reader_to_complete = completion.state;
        reader_value.emplace(std::optional<T>{});
      }
      else if (lifecycle_ == lifecycle::failed || lifecycle_ == lifecycle::cancelled)
      {
        reader_to_complete = completion.state;
        reader_value.emplace(std::unexpected(*terminal_error_));
      }
      else
      {
        pending_reader_ = completion.state;
      }
    }

    if (reader_value)
      set_task_value_noexcept(reader_to_complete, std::move(*reader_value));
    if (writer_value)
      set_task_value_noexcept(writer_to_complete, std::move(*writer_value));
    dispatch_observers(std::move(observers));
    return result;
  }

  task<send_result> send(T value)
  {
    auto completion = make_stream_task<send_result>();
    auto result = std::move(completion.result);
    std::shared_ptr<shared_state<next_result>> reader_to_complete;
    std::optional<next_result> reader_value;
    std::shared_ptr<shared_state<send_result>> writer_to_complete;
    std::optional<send_result> writer_value;

    {
      std::lock_guard lock(mutex_);

      if (lifecycle_ != lifecycle::open)
      {
        writer_to_complete = completion.state;
        writer_value.emplace(std::unexpected(error_for_send_locked()));
      }
      else if (pending_writer_)
      {
        writer_to_complete = completion.state;
        writer_value.emplace(std::unexpected(make_stream_error(
          stream_error_code::protocol_error,
          "only one send() operation may be pending")));
      }
      else if (pending_reader_)
      {
        ++produced_items_;
        ++consumed_items_;
        reader_to_complete = std::move(pending_reader_);
        reader_value.emplace(std::optional<T>{std::move(value)});
        writer_to_complete = completion.state;
        writer_value.emplace();
      }
      else if (buffer_.size() < capacity_)
      {
        buffer_.push_back(std::move(value));
        ++produced_items_;
        writer_to_complete = completion.state;
        writer_value.emplace();
      }
      else
      {
        pending_writer_.emplace(pending_send{std::move(value), completion.state});
      }
    }

    if (reader_value)
      set_task_value_noexcept(reader_to_complete, std::move(*reader_value));
    if (writer_value)
      set_task_value_noexcept(writer_to_complete, std::move(*writer_value));
    return result;
  }

  void close() noexcept
  {
    std::shared_ptr<shared_state<next_result>> reader_to_complete;
    std::shared_ptr<shared_state<send_result>> writer_to_complete;
    std::vector<std::pair<stream_observer, stream_completion>> observers;

    {
      std::lock_guard lock(mutex_);
      if (lifecycle_ != lifecycle::open)
        return;

      lifecycle_ = lifecycle::closing;
      if (pending_writer_)
      {
        writer_to_complete = std::move(pending_writer_->completion);
        pending_writer_.reset();
      }

      if (buffer_.empty())
      {
        lifecycle_ = lifecycle::closed;
        reader_to_complete = std::move(pending_reader_);
        observers = finish_locked();
      }
    }

    if (writer_to_complete)
      set_task_value_noexcept(writer_to_complete, send_result{std::unexpected(make_stream_error(
        stream_error_code::closed, "stream was closed"))});
    if (reader_to_complete)
      set_task_value_noexcept(reader_to_complete, next_result{std::optional<T>{}});
    dispatch_observers(std::move(observers));
  }

  void fail(stream_error error) noexcept
  {
    terminate(lifecycle::failed, std::move(error));
  }

  void cancel(bool consumer_gone) noexcept
  {
    auto code = consumer_gone
      ? stream_error_code::consumer_gone
      : stream_error_code::cancelled;
    auto message = consumer_gone
      ? "stream consumer was destroyed"
      : "stream cancellation requested";
    terminate(lifecycle::cancelled, make_stream_error(code, message), true);
  }

  void producer_done() noexcept
  {
    std::vector<std::pair<stream_observer, stream_completion>> observers;
    {
      std::lock_guard lock(mutex_);
      producer_done_ = true;
      observers = finish_locked();
    }
    dispatch_observers(std::move(observers));
  }

  template<class F>
    requires std::invocable<F&, const stream_completion&>
  void observe_completion(scheduler executor, F&& callback)
  {
    std::optional<std::pair<stream_observer, stream_completion>> ready;
    {
      std::lock_guard lock(mutex_);
      stream_observer observer{
        executor.valid() ? std::move(executor) : environment_.scheduler,
        std::function<void(const stream_completion&)>{std::forward<F>(callback)}};
      if (completion_)
        ready.emplace(std::move(observer), *completion_);
      else
        observers_.push_back(std::move(observer));
    }
    if (ready)
      dispatch_stream_observer(std::move(ready->first), std::move(ready->second));
  }

private:
  enum class lifecycle
  {
    open,
    closing,
    closed,
    failed,
    cancelled
  };

  struct pending_send
  {
    T value;
    std::shared_ptr<shared_state<send_result>> completion;
  };

  stream_error error_for_send_locked() const
  {
    if (terminal_error_)
      return *terminal_error_;
    return make_stream_error(stream_error_code::closed, "stream is closed");
  }

  void terminate(lifecycle terminal, stream_error error, bool override_closing = false) noexcept
  {
    std::shared_ptr<shared_state<next_result>> reader_to_complete;
    std::shared_ptr<shared_state<send_result>> writer_to_complete;
    std::pmr::deque<T> discarded{resource_};
    std::vector<std::pair<stream_observer, stream_completion>> observers;

    {
      std::lock_guard lock(mutex_);
      if (lifecycle_ != lifecycle::open
          && !(override_closing && lifecycle_ == lifecycle::closing))
        return;

      lifecycle_ = terminal;
      terminal_error_ = std::move(error);
      discarded.swap(buffer_);
      reader_to_complete = std::move(pending_reader_);
      if (pending_writer_)
      {
        writer_to_complete = std::move(pending_writer_->completion);
        pending_writer_.reset();
      }
      observers = finish_locked();
    }

    if (reader_to_complete)
      set_task_value_noexcept(reader_to_complete, next_result{std::unexpected(*terminal_error_)});
    if (writer_to_complete)
      set_task_value_noexcept(writer_to_complete, send_result{std::unexpected(*terminal_error_)});
    dispatch_observers(std::move(observers));
  }

  std::vector<std::pair<stream_observer, stream_completion>> finish_locked()
  {
    const auto terminal = lifecycle_ == lifecycle::closed
      || lifecycle_ == lifecycle::failed
      || lifecycle_ == lifecycle::cancelled;
    if (!terminal || !producer_done_)
      return {};

    if (!completion_)
    {
      auto terminal_state = stream_terminal_state::closed;
      if (lifecycle_ == lifecycle::failed)
        terminal_state = stream_terminal_state::failed;
      else if (lifecycle_ == lifecycle::cancelled)
        terminal_state = stream_terminal_state::cancelled;

      completion_.emplace(stream_completion{
        terminal_state,
        produced_items_,
        consumed_items_,
        std::chrono::steady_clock::now() - started_at_,
        terminal_error_});
    }

    std::vector<std::pair<stream_observer, stream_completion>> ready;
    ready.reserve(observers_.size());
    for (auto& observer : observers_)
      ready.emplace_back(std::move(observer), *completion_);
    observers_.clear();
    return ready;
  }

  static void dispatch_observers(
    std::vector<std::pair<stream_observer, stream_completion>> observers) noexcept
  {
    for (auto& [observer, completion] : observers)
      dispatch_stream_observer(std::move(observer), std::move(completion));
  }

  std::size_t capacity_;
  memory::memory_resource* resource_;
  std::pmr::deque<T> buffer_;
  std::pmr::vector<stream_observer> observers_;
  task_environment environment_;
  std::chrono::steady_clock::time_point started_at_;
  mutable std::mutex mutex_;
  lifecycle lifecycle_ = lifecycle::open;
  std::optional<stream_error> terminal_error_;
  std::optional<stream_completion> completion_;
  std::shared_ptr<shared_state<next_result>> pending_reader_;
  std::optional<pending_send> pending_writer_;
  std::size_t produced_items_ = 0;
  std::size_t consumed_items_ = 0;
  bool producer_done_ = false;
};

template<class T, class Producer>
task<void> run_stream_producer(
  Producer producer,
  stream_source<T> source)
{
  co_await std::invoke(producer, std::move(source));
}
} // namespace modern::detail

export namespace modern
{
template<class T>
class stream
{
public:
  using value_type = T;

  stream() = default;

  // Internal construction surface. stream_state is not exported from this
  // module, so callers cannot manufacture a stream through this constructor.
  explicit stream(std::shared_ptr<detail::stream_state<T>> state)
    : state_(std::move(state))
  {
  }

  stream(stream&& other) noexcept
    : state_(std::move(other.state_))
  {
  }

  stream& operator=(stream&& other) noexcept
  {
    if (this == &other)
      return *this;
    request_stop_for_consumer();
    state_ = std::move(other.state_);
    return *this;
  }

  stream(const stream&) = delete;
  stream& operator=(const stream&) = delete;

  ~stream()
  {
    request_stop_for_consumer();
  }

  task<stream_result<T>> next()
  {
    if (!state_)
    {
      auto completion = detail::make_stream_task<stream_result<T>>();
      detail::set_task_value_noexcept(completion.state, stream_result<T>{std::unexpected(
        detail::make_stream_error(
          stream_error_code::protocol_error, "invalid stream handle"))});
      return std::move(completion.result);
    }
    return state_->next();
  }

  void request_stop() noexcept
  {
    if (state_)
      state_->cancel(false);
  }

  [[nodiscard]] bool valid() const noexcept
  {
    return static_cast<bool>(state_);
  }

  template<class F>
    requires std::invocable<F&, const stream_completion&>
  void observe_completion(scheduler executor, F&& callback)
  {
    if (!state_)
      throw std::logic_error("invalid stream handle");
    state_->observe_completion(std::move(executor), std::forward<F>(callback));
  }

  template<class F>
    requires std::invocable<F&, const stream_completion&>
  void observe_completion(F&& callback)
  {
    observe_completion({}, std::forward<F>(callback));
  }

private:
  void request_stop_for_consumer() noexcept
  {
    if (state_)
      state_->cancel(true);
  }

  std::shared_ptr<detail::stream_state<T>> state_;

};

template<class T>
class stream_source
{
public:
  // Internal construction surface; stream_state is module-private.
  explicit stream_source(std::shared_ptr<detail::stream_state<T>> state)
    : state_(std::move(state))
  {
  }

  stream_source(stream_source&&) noexcept = default;
  stream_source& operator=(stream_source&&) noexcept = default;
  stream_source(const stream_source&) = delete;
  stream_source& operator=(const stream_source&) = delete;

  task<std::expected<void, stream_error>> send(T value)
  {
    if (!state_)
    {
      auto completion = detail::make_stream_task<std::expected<void, stream_error>>();
      detail::set_task_value_noexcept(
        completion.state,
        std::expected<void, stream_error>{std::unexpected(detail::make_stream_error(
          stream_error_code::protocol_error, "invalid stream source"))});
      return std::move(completion.result);
    }
    return state_->send(std::move(value));
  }

  void close() noexcept
  {
    if (state_)
      state_->close();
  }

  void fail(stream_error error) noexcept
  {
    if (state_)
      state_->fail(std::move(error));
  }

  [[nodiscard]] bool valid() const noexcept
  {
    return static_cast<bool>(state_);
  }

private:
  std::shared_ptr<detail::stream_state<T>> state_;

};

template<class T, class Producer>
  requires std::same_as<
    std::remove_cvref_t<std::invoke_result_t<std::remove_cvref_t<Producer>&, stream_source<T>>>,
    task<void>>
stream<T> make_stream(std::size_t capacity, Producer&& producer)
{
  if (capacity == 0)
    throw std::invalid_argument("stream capacity must be at least one");

  auto environment = current_task_environment_value();
  if (!environment.scheduler.valid())
    environment.scheduler = inline_scheduler();
  if (!environment.frame_resource)
    environment.frame_resource = memory::get_default_resource();

  auto state = detail::allocate_shared_object<detail::stream_state<T>>(
    environment.frame_resource,
    capacity,
    environment.frame_resource,
    environment);

  try
  {
    task_environment_scope scope{environment};
    auto producer_task = detail::run_stream_producer<T>(
      std::remove_cvref_t<Producer>(std::forward<Producer>(producer)),
      stream_source<T>{state});
    auto observer = std::move(producer_task)
      .then([state]
      {
        state->close();
        state->producer_done();
      })
      .catching([state](std::exception_ptr exception)
      {
        state->fail(detail::producer_exception_error(std::move(exception)));
        state->producer_done();
      });
    observer.detach();
  }
  catch (...)
  {
    state->fail(detail::producer_exception_error(std::current_exception()));
    state->producer_done();
  }

  return stream<T>{std::move(state)};
}
} // namespace modern
