module;

#include <cstddef>
#include <memory_resource>

export module modern.memory;

export namespace modern::memory
{
using std::pmr::memory_resource;
using std::pmr::monotonic_buffer_resource;
using std::pmr::pool_options;
using std::pmr::unsynchronized_pool_resource;

template<class T>
using polymorphic_allocator = std::pmr::polymorphic_allocator<T>;

inline memory_resource* get_default_resource() noexcept
{
  return std::pmr::get_default_resource();
}

inline memory_resource* new_delete_resource() noexcept
{
  return std::pmr::new_delete_resource();
}

class arena
{
public:
  explicit arena(memory_resource* upstream = get_default_resource()) noexcept;
  explicit arena(std::byte* buffer,
                 std::size_t size,
                 memory_resource* upstream = get_default_resource()) noexcept;

  arena(arena&&) noexcept = delete;
  arena& operator=(arena&&) noexcept = delete;

  arena(const arena&) = delete;
  arena& operator=(const arena&) = delete;

  [[nodiscard]] memory_resource* resource() noexcept;
  void release() noexcept;

private:
  monotonic_buffer_resource resource_;
};

class pool
{
public:
  explicit pool(memory_resource* upstream = get_default_resource());
  explicit pool(pool_options options,
                memory_resource* upstream = get_default_resource());

  pool(pool&&) noexcept = delete;
  pool& operator=(pool&&) noexcept = delete;

  pool(const pool&) = delete;
  pool& operator=(const pool&) = delete;

  [[nodiscard]] memory_resource* resource() noexcept;
  void release();

private:
  unsynchronized_pool_resource resource_;
};
} // namespace modern::memory

