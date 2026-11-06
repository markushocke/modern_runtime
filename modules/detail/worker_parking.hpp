#ifndef MODERN_RUNTIME_WORKER_PARKING_HPP
#define MODERN_RUNTIME_WORKER_PARKING_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <stdexcept>
#if defined(__linux__)
#include <cerrno>
#include <exception>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace modern::detail {

// Idle-only parking: one atomic wait channel per worker. A producer reserves
// exactly one parked waiter with exchange(false), then advances its epoch and
// notifies it. Multiple producers cannot repeatedly target the same waiter.
//
// Protocol:
//   worker: epoch = prepare(); recheck published queues + shutdown;
//           if work exists cancel(); else wait(index, epoch).
//   producer: publish task, then notify_one().
// Rechecking after registration closes the publish-before-park race. The
// caller must NOT park while accepted, unclaimed tasks exist: a queue scan
// can transiently miss a task being published or concurrently stolen.
class worker_parking {
  struct alignas(64) slot {
    std::atomic<std::uint64_t> epoch{0};
    // A 32-bit futex word lets Linux sleep without libstdc++'s spin phase.
    // It is used ONLY by the separately selectable futex mode.
    std::atomic<std::uint32_t> futex_epoch{0};
    std::atomic<bool> parked{false};
  };

public:
  explicit worker_parking(std::size_t workers,
                          std::pmr::memory_resource* resource,
                          bool linux_futex = false)
    : count_(workers),
      linux_futex_(linux_futex),
      allocator_(resource ? resource : std::pmr::get_default_resource()) {
#if !defined(__linux__)
    if (linux_futex_) throw std::invalid_argument("Linux futex parking is unavailable");
#else
    // The futex ABI requires an aligned, 32-bit word. This project supports
    // normal Linux std::atomic<uint32_t> implementations satisfying this.
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
    static_assert(alignof(std::atomic<std::uint32_t>) >= alignof(std::uint32_t));
#endif
    slots_ = allocator_.allocate(count_);
    for (std::size_t i = 0; i < count_; ++i)
      std::construct_at(slots_ + i);
  }

  worker_parking(const worker_parking&) = delete;
  worker_parking& operator=(const worker_parking&) = delete;

  ~worker_parking() {
    // The thread pool must join its workers and producers before destruction.
    for (std::size_t i = 0; i < count_; ++i)
      std::destroy_at(slots_ + i);
    allocator_.deallocate(slots_, count_);
  }

  [[nodiscard]] std::uint64_t prepare(std::size_t worker) noexcept {
    auto& entry = slots_[worker];
    const auto observed = linux_futex_
      ? static_cast<std::uint64_t>(entry.futex_epoch.load(std::memory_order_acquire))
      : entry.epoch.load(std::memory_order_acquire);
    // Count first, then publish the parked flag. A notifier that sees a
    // transient count without a flag can safely skip: the worker rechecks.
    // Seq-cst pairing with the publisher's acceptance CAS and idle
    // worker's post-registration state load prevents a missed notification:
    // either the worker sees pending work, or the publisher sees this waiter.
    parked_count_.fetch_add(1, std::memory_order_seq_cst);
    entry.parked.store(true, std::memory_order_seq_cst);
    return observed;
  }

  void cancel(std::size_t worker) noexcept {
    // Only the party that switches true -> false decreases parked_count_.
    if (slots_[worker].parked.exchange(false, std::memory_order_acq_rel))
      parked_count_.fetch_sub(1, std::memory_order_acq_rel);
  }

  void wait(std::size_t worker, std::uint64_t observed) noexcept {
    // A prior notification increments epoch so this cannot miss a wakeup.
    if (linux_futex_) {
#if defined(__linux__)
      auto& epoch = slots_[worker].futex_epoch;
      const auto expected = static_cast<std::uint32_t>(observed);
      // FUTEX_WAIT_PRIVATE compares before sleeping: wake-before-wait
      // returns EAGAIN rather than losing the notification. EINTR retries.
      while (epoch.load(std::memory_order_acquire) == expected) {
        const auto rc = ::syscall(SYS_futex,
          reinterpret_cast<std::uint32_t*>(&epoch), FUTEX_WAIT_PRIVATE,
          expected, nullptr, nullptr, 0);
        if (rc == -1 && errno != EINTR && errno != EAGAIN)
          std::terminate();
      }
#endif
    } else {
      slots_[worker].epoch.wait(observed, std::memory_order_acquire);
    }
    cancel(worker);
  }

  void notify_one() noexcept {
    if (parked_count_.load(std::memory_order_seq_cst) == 0)
      return;

    const auto first = next_wake_.fetch_add(1, std::memory_order_relaxed) % count_;
    for (std::size_t step = 0; step < count_; ++step) {
      auto& entry = slots_[(first + step) % count_];
      if (!entry.parked.exchange(false, std::memory_order_seq_cst))
        continue;
      parked_count_.fetch_sub(1, std::memory_order_acq_rel);
      wake_slot(entry);
      return;
    }
  }

  void notify_all() noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      auto& entry = slots_[i];
      if (entry.parked.exchange(false, std::memory_order_seq_cst))
        parked_count_.fetch_sub(1, std::memory_order_acq_rel);
      wake_slot(entry);
    }
  }

  [[nodiscard]] std::size_t parked_count() const noexcept {
    return parked_count_.load(std::memory_order_relaxed);
  }

private:
  void wake_slot(slot& entry) noexcept {
    if (linux_futex_) {
#if defined(__linux__)
      entry.futex_epoch.fetch_add(1, std::memory_order_release);
      if (::syscall(SYS_futex,
            reinterpret_cast<std::uint32_t*>(&entry.futex_epoch),
            FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0) == -1)
        std::terminate();
#endif
    } else {
      entry.epoch.fetch_add(1, std::memory_order_release);
      entry.epoch.notify_one();
    }
  }

  const std::size_t count_;
  const bool linux_futex_;
  std::pmr::polymorphic_allocator<slot> allocator_;
  slot* slots_ = nullptr;
  alignas(64) std::atomic<std::size_t> parked_count_{0};
  alignas(64) std::atomic<std::size_t> next_wake_{0};
};

} // namespace modern::detail
#endif
