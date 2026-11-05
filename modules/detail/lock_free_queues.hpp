#ifndef MODERN_RUNTIME_LOCK_FREE_QUEUES_HPP
#define MODERN_RUNTIME_LOCK_FREE_QUEUES_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>

namespace modern::detail {

// Fixed-size power-of-two buffers avoid reclamation of slot storage while
// thieves/readers are active. Payloads are pointers: ownership transfers only
// after the winning CAS. A scheduler must release the pointed-to task itself.
inline std::size_t queue_power_of_two(std::size_t requested) {
  std::size_t value = 2;
  while (value < requested) {
    if (value > std::numeric_limits<std::size_t>::max() / 2)
      throw std::length_error("lock-free queue capacity overflow");
    value *= 2;
  }
  return value;
}

template<class T>
class bounded_chase_lev_deque {
  static_assert(std::is_pointer_v<T>);
 public:
  explicit bounded_chase_lev_deque(
      std::size_t requested,
      std::pmr::memory_resource* resource = std::pmr::get_default_resource())
      : capacity_(queue_power_of_two(requested)),
        mask_(capacity_ - 1),
        allocator_(resource ? resource : std::pmr::get_default_resource()) {
    slots_ = allocator_.allocate(capacity_);
    for (std::size_t i = 0; i < capacity_; ++i)
      std::construct_at(slots_ + i, nullptr);
  }

  bounded_chase_lev_deque(const bounded_chase_lev_deque&) = delete;
  bounded_chase_lev_deque& operator=(const bounded_chase_lev_deque&) = delete;
  ~bounded_chase_lev_deque() {
    for (std::size_t i = 0; i < capacity_; ++i)
      std::destroy_at(slots_ + i);
    allocator_.deallocate(slots_, capacity_);
  }

  // Only the owning worker may push/pop at the bottom. Foreign workers steal
  // from the top. An overflowing push is routed to the injection queue.
  [[nodiscard]] bool try_push_owner(T value) noexcept {
    const auto bottom = bottom_.load(std::memory_order_relaxed);
    const auto top = top_.load(std::memory_order_acquire);
    if (bottom - top >= capacity_)
      return false;
    slots_[bottom & mask_].store(value, std::memory_order_relaxed);
    bottom_.store(bottom + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] T pop_owner() noexcept {
    auto bottom = bottom_.load(std::memory_order_relaxed);
    if (bottom == 0)
      return nullptr;
    --bottom;
    bottom_.store(bottom, std::memory_order_seq_cst);
    auto top = top_.load(std::memory_order_seq_cst);
    if (top <= bottom) {
      T value = slots_[bottom & mask_].load(std::memory_order_relaxed);
      if (top == bottom) {
        if (!top_.compare_exchange_strong(top, top + 1,
                std::memory_order_seq_cst, std::memory_order_relaxed))
          value = nullptr;
        bottom_.store(bottom + 1, std::memory_order_relaxed);
      }
      return value;
    }
    bottom_.store(bottom + 1, std::memory_order_relaxed);
    return nullptr;
  }

  [[nodiscard]] T steal() noexcept {
    auto top = top_.load(std::memory_order_seq_cst);
    const auto bottom = bottom_.load(std::memory_order_seq_cst);
    if (top >= bottom)
      return nullptr;
    T value = slots_[top & mask_].load(std::memory_order_relaxed);
    if (!top_.compare_exchange_strong(top, top + 1,
            std::memory_order_seq_cst, std::memory_order_relaxed))
      return nullptr;
    return value;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  const std::size_t capacity_;
  const std::size_t mask_;
  std::pmr::polymorphic_allocator<std::atomic<T>> allocator_;
  std::atomic<T>* slots_ = nullptr;
  alignas(64) std::atomic<std::size_t> top_{0};
  alignas(64) std::atomic<std::size_t> bottom_{0};
};

// Dmitry Vyukov's bounded MPMC ring algorithm: multiple external producers
// and any worker may concurrently drain injection. Unlike a strict MPSC ring,
// this can safely support the pool-wide, non-targeted semaphore permits.
template<class T>
class bounded_mpmc_queue {
  static_assert(std::is_pointer_v<T>);
  struct cell {
    std::atomic<std::size_t> sequence{0};
    T value = nullptr;
  };
 public:
  explicit bounded_mpmc_queue(
      std::size_t requested,
      std::pmr::memory_resource* resource = std::pmr::get_default_resource())
      : capacity_(queue_power_of_two(requested)),
        mask_(capacity_ - 1),
        allocator_(resource ? resource : std::pmr::get_default_resource()) {
    cells_ = allocator_.allocate(capacity_);
    for (std::size_t i = 0; i < capacity_; ++i) {
      std::construct_at(cells_ + i);
      cells_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }
  bounded_mpmc_queue(const bounded_mpmc_queue&) = delete;
  bounded_mpmc_queue& operator=(const bounded_mpmc_queue&) = delete;
  ~bounded_mpmc_queue() {
    for (std::size_t i = 0; i < capacity_; ++i)
      std::destroy_at(cells_ + i);
    allocator_.deallocate(cells_, capacity_);
  }

  [[nodiscard]] bool try_push(T value) noexcept {
    auto pos = enqueue_.load(std::memory_order_relaxed);
    for (;;) {
      auto& slot = cells_[pos & mask_];
      const auto seq = slot.sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<std::ptrdiff_t>(seq - pos);
      if (diff == 0) {
        if (enqueue_.compare_exchange_weak(pos, pos + 1,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
          slot.value = value;
          slot.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        return false;
      } else {
        pos = enqueue_.load(std::memory_order_relaxed);
      }
    }
  }

  [[nodiscard]] T try_pop() noexcept {
    auto pos = dequeue_.load(std::memory_order_relaxed);
    for (;;) {
      auto& slot = cells_[pos & mask_];
      const auto seq = slot.sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<std::ptrdiff_t>(seq - (pos + 1));
      if (diff == 0) {
        if (dequeue_.compare_exchange_weak(pos, pos + 1,
                std::memory_order_relaxed, std::memory_order_relaxed)) {
          T value = slot.value;
          slot.sequence.store(pos + capacity_, std::memory_order_release);
          return value;
        }
      } else if (diff < 0) {
        return nullptr;
      } else {
        pos = dequeue_.load(std::memory_order_relaxed);
      }
    }
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  const std::size_t capacity_;
  const std::size_t mask_;
  std::pmr::polymorphic_allocator<cell> allocator_;
  cell* cells_ = nullptr;
  alignas(64) std::atomic<std::size_t> enqueue_{0};
  alignas(64) std::atomic<std::size_t> dequeue_{0};
};

} // namespace modern::detail
#endif
