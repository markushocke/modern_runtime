module;

#include <cstddef>

module modern.memory;

namespace modern::memory
{
arena::arena(memory_resource* upstream) noexcept
  : resource_(upstream)
{
}

arena::arena(std::byte* buffer, std::size_t size, memory_resource* upstream) noexcept
  : resource_(buffer, size, upstream)
{
}

memory_resource* arena::resource() noexcept
{
  return &resource_;
}

void arena::release() noexcept
{
  resource_.release();
}
} // namespace modern::memory
