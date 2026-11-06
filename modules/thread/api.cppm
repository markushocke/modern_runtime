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
// Backend strategy is independent of the public scheduler/task interface.
// Further backends, including Chase-Lev, can implement the internal protocol.
enum class queue_backend
{
  direct_mutex,           // Original mutex-protected per-worker queues.
  injection_mutex_local,  // Per-worker producer inbox + mutex-local queue.
  injection_chase_lev,    // Bounded lock-free worker deques + MPMC ingress.
};

// Wake policy is independent of the queue backend. The original per-task
// permit protocol remains the default until idle parking passes full
// correctness and performance acceptance.
enum class worker_wakeup
{
  per_task_semaphore,
  idle_parking,
  idle_parking_futex, // Linux-only alternative to std::atomic::wait.
};

// Prefer named options for new users; positional constructor remains compatible.
struct thread_pool_options
{
  std::size_t workers = std::thread::hardware_concurrency();
  memory::memory_resource* resource = memory::get_default_resource();
  std::size_t max_pending_tasks = 1024;
  std::span<const std::size_t> worker_affinity = {};
  queue_backend backend = queue_backend::direct_mutex;
  worker_wakeup wakeup = worker_wakeup::per_task_semaphore;
};

class thread_pool
{
public:
  explicit thread_pool(thread_pool_options options);

  explicit thread_pool(
    std::size_t thread_count = std::thread::hardware_concurrency(),
    memory::memory_resource* resource = memory::get_default_resource(),
    std::size_t max_pending_tasks = 1024,
    std::span<const std::size_t> worker_affinity = {},
    queue_backend backend = queue_backend::direct_mutex,
    worker_wakeup wakeup = worker_wakeup::per_task_semaphore);

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
