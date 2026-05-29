module;

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include <optional>
#include <thread>

module modern.platform;

namespace modern::platform
{
steady_time_point now() noexcept
{
  return steady_clock::now();
}

void sleep_for(steady_duration duration)
{
  if (duration > steady_duration::zero())
    std::this_thread::sleep_for(duration);
}

void yield() noexcept
{
  std::this_thread::yield();
}

std::size_t hardware_concurrency() noexcept
{
  auto count = std::thread::hardware_concurrency();
  return count == 0 ? 1u : count;
}

std::optional<std::size_t> current_cpu() noexcept
{
#if defined(__linux__)
  auto cpu = ::sched_getcpu();

  if (cpu < 0)
    return std::nullopt;

  return static_cast<std::size_t>(cpu);
#else
  return std::nullopt;
#endif
}

bool set_thread_affinity(thread_handle thread, std::size_t cpu_index) noexcept
{
#if defined(__linux__)
  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(cpu_index, &cpu_set);
  return ::pthread_setaffinity_np(thread, sizeof(cpu_set), &cpu_set) == 0;
#else
  (void)thread;
  (void)cpu_index;
  return false;
#endif
}
} // namespace modern::platform
