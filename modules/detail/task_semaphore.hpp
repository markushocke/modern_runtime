#ifndef MODERN_RUNTIME_TASK_SEMAPHORE_HPP
#define MODERN_RUNTIME_TASK_SEMAPHORE_HPP

#include <cstddef>
#if defined(__linux__)
#include <cerrno>
#include <climits>
#include <exception>
#include <semaphore.h>
#include <system_error>
#else
#include <semaphore>
#endif

namespace modern::detail {
// Linux libstdc++ 13's atomic-wait semaphore can sleep on a positive counter
// after a contended CAS/ABA, leaving a shutdown permit unconsumed indefinitely.
// POSIX sem_wait atomically checks/decrements the count before sleeping and
// sem_post wakes one waiter. Keep the standard implementation elsewhere.
class task_semaphore {
public:
  explicit task_semaphore(std::ptrdiff_t count)
#if !defined(__linux__)
    : semaphore_(count)
#endif
  {
#if defined(__linux__)
    if (sem_init(&semaphore_, 0, static_cast<unsigned>(count)) != 0)
      throw std::system_error(errno, std::generic_category(), "sem_init");

#endif
  }
  ~task_semaphore() {
#if defined(__linux__)
    sem_destroy(&semaphore_);
#endif
  }
  task_semaphore(const task_semaphore&) = delete;
  task_semaphore& operator=(const task_semaphore&) = delete;

  static constexpr std::ptrdiff_t max() noexcept {
#if defined(__linux__)
    return SEM_VALUE_MAX;
#else
    return std::counting_semaphore<>::max();
#endif
  }
  void acquire() noexcept {
#if defined(__linux__)
    while (sem_wait(&semaphore_) != 0)
      if (errno != EINTR) std::terminate();
#else
    semaphore_.acquire();
#endif
  }
  void release(std::ptrdiff_t count = 1) noexcept {
#if defined(__linux__)
    for (std::ptrdiff_t i = 0; i < count; ++i)
      if (sem_post(&semaphore_) != 0) std::terminate();
#else
    semaphore_.release(count);
#endif
  }
private:
#if defined(__linux__)
  sem_t semaphore_;
#else
  std::counting_semaphore<> semaphore_{0};
#endif
};
}
#endif
