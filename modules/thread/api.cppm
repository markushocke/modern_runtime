module;

#include <cstddef>
#include <memory>
#include <span>
#include <thread>
#include <utility>

export module modern.thread:api;

export import modern.exec;
export import modern.memory;

namespace modern::detail
{
class thread_pool_state;
} // namespace modern::detail

export namespace modern
{
class thread_pool
{
public:
  explicit thread_pool(
    std::size_t thread_count = std::thread::hardware_concurrency(),
    memory::memory_resource* resource = memory::get_default_resource(),
    std::size_t max_pending_tasks = 1024,
    std::span<const std::size_t> worker_affinity = {});

  thread_pool(const thread_pool&) = delete;
  thread_pool& operator=(const thread_pool&) = delete;

  thread_pool(thread_pool&&) noexcept = default;
  thread_pool& operator=(thread_pool&&) noexcept = default;

  ~thread_pool();

  [[nodiscard]] scheduler get_scheduler() const;

  [[nodiscard]] memory::memory_resource* resource() const noexcept;

  template<class F>
  void execute(F&& f)
  {
    get_scheduler().execute(std::forward<F>(f));
  }

  template<class F>
  void execute(scheduler_priority priority, F&& f)
  {
    get_scheduler().execute(priority, std::forward<F>(f));
  }

  void shutdown() noexcept;

  void join();

private:
  std::shared_ptr<detail::thread_pool_state> state_;
};
} // namespace modern
