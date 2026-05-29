module;

module modern.memory;

namespace modern::memory
{
pool::pool(memory_resource* upstream)
  : resource_(upstream)
{
}

pool::pool(pool_options options, memory_resource* upstream)
  : resource_(options, upstream)
{
}

memory_resource* pool::resource() noexcept
{
  return &resource_;
}

void pool::release()
{
  resource_.release();
}
} // namespace modern::memory
