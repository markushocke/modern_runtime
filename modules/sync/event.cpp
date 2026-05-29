module;

#include <mutex>

module modern.sync;

namespace modern::sync
{
manual_reset_event::manual_reset_event(bool signaled) noexcept
  : signaled_(signaled)
{
}

void manual_reset_event::set()
{
  {
    std::lock_guard lock(mutex_);
    signaled_ = true;
  }

  cv_.notify_all();
}

void manual_reset_event::reset()
{
  std::lock_guard lock(mutex_);
  signaled_ = false;
}

void manual_reset_event::wait()
{
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this]
  {
    return signaled_;
  });
}

bool manual_reset_event::wait_until(platform::steady_time_point deadline)
{
  std::unique_lock lock(mutex_);
  return cv_.wait_until(lock, deadline, [this]
  {
    return signaled_;
  });
}

bool manual_reset_event::is_set() const
{
  std::lock_guard lock(mutex_);
  return signaled_;
}
} // namespace modern::sync
