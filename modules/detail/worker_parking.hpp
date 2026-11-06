#ifndef MODERN_RUNTIME_WORKER_PARKING_HPP
#define MODERN_RUNTIME_WORKER_PARKING_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>

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
    std::atomic<bool> parked{false};
  };

public:
  explicit worker_parking(std::size_t workers,
                          std::pmr::memory_resource* resource)
    : count_(workers),
      allocator_(resource ? resource : std::pmr::get_default_resource()) {
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
    const auto observed = entry.epoch.load(std::memory_order_acquire);
    // Count first, then publish the parked flag. A notifier that sees a
    // transient count without a flag can safely skip: the worker rechecks.
    parked_count_.fetch_add(1, std::memory_order_acq_rel);
    entry.parked.store(true, std::memory_order_release);
    return observed;
  }

  void cancel(std::size_t worker) noexcept {
    // Only the party that switches true -> false decreases parked_count_.
    if (slots_[worker].parked.exchange(false, std::memory_order_acq_rel))
      parked_count_.fetch_sub(1, std::memory_order_acq_rel);
  }

  void wait(std::size_t worker, std::uint64_t observed) noexcept {
    // A prior notification increments epoch so this cannot miss a wakeup.
    slots_[worker].epoch.wait(observed, std::memory_order_acquire);
    cancel(worker);
  }

  void notify_one() noexcept {
    if (parked_count_.load(std::memory_order_acquire) == 0)
      return;

    const auto first = next_wake_.fetch_add(1, std::memory_order_relaxed) % count_;
    for (std::size_t step = 0; step < count_; ++step) {
      auto& entry = slots_[(first + step) % count_];
      if (!entry.parked.exchange(false, std::memory_order_acq_rel))
        continue;
      parked_count_.fetch_sub(1, std::memory_order_acq_rel);
      entry.epoch.fetch_add(1, std::memory_order_release);
      entry.epoch.notify_one();
      return;
    }
  }

  void notify_all() noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      auto& entry = slots_[i];
      if (entry.parked.exchange(false, std::memory_order_acq_rel))
        parked_count_.fetch_sub(1, std::memory_order_acq_rel);
      entry.epoch.fetch_add(1, std::memory_order_release);
      entry.epoch.notify_one();
    }
  }

  [[nodiscard]] std::size_t parked_count() const noexcept {
    return parked_count_.load(std::memory_order_relaxed);
  }

private:
  const std::size_t count_;
  std::pmr::polymorphic_allocator<slot> allocator_;
  slot* slots_ = nullptr;
  alignas(64) std::atomic<std::size_t> parked_count_{0};
  alignas(64) std::atomic<std::size_t> next_wake_{0};
};

} // namespace modern::detail
#endif
