#ifndef MODERN_RUNTIME_INTRUSIVE_QUEUE_HPP
#define MODERN_RUNTIME_INTRUSIVE_QUEUE_HPP

#include <cstddef>
#include <deque>
#include <memory_resource>
#include <utility>

namespace modern::detail
{
// Facade around FIFO storage so the queue strategy can change without touching users.
template<class T>
class intrusive_queue
{
public:
  explicit intrusive_queue(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
    : items_(resource)
  {
  }

  [[nodiscard]] bool empty() const noexcept
  {
    return items_.empty();
  }

  [[nodiscard]] std::size_t size() const noexcept
  {
    return items_.size();
  }

  template<class U>
  void push(U&& value)
  {
    items_.push_back(std::forward<U>(value));
  }

  T pop()
  {
    T value = std::move(items_.front());
    items_.pop_front();
    return value;
  }

  T pop_back()
  {
    T value = std::move(items_.back());
    items_.pop_back();
    return value;
  }

private:
  std::pmr::deque<T> items_;
};
} // namespace modern::detail

#endif
