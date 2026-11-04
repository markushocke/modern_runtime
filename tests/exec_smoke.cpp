import modern.exec;

#include "../modules/detail/move_only_function_support.hpp"

#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <memory_resource>

namespace
{
class counting_resource final : public std::pmr::memory_resource
{
public:
  [[nodiscard]] std::size_t allocations() const noexcept
  {
    return allocations_;
  }

private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override
  {
    ++allocations_;
    return std::pmr::get_default_resource()->allocate(bytes, alignment);
  }

  void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override
  {
    std::pmr::get_default_resource()->deallocate(ptr, bytes, alignment);
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
  {
    return this == &other;
  }

  std::size_t allocations_ = 0;
};

class resource_executor
{
public:
  explicit resource_executor(std::pmr::memory_resource* resource) noexcept
    : resource_(resource)
  {
  }

  void execute(modern::detail::move_only_function task)
  {
    task();
  }

  [[nodiscard]] std::pmr::memory_resource* resource() const noexcept
  {
    return resource_;
  }

private:
  std::pmr::memory_resource* resource_;
};
} // namespace

int main()
{
  counting_resource resource;
  bool direct_called = false;

  // Small callables are intentionally stored inline, even when a PMR
  // resource is supplied. Supplying an allocator does not require allocation.
  const auto inline_before = resource.allocations();
  modern::detail::move_only_function inline_call{&resource, [&]
  {
    direct_called = true;
  }};

  if (resource.allocations() != inline_before)
  {
    std::cerr << "small callable unexpectedly allocated\n";
    return 1;
  }

  inline_call();
  if (!direct_called)
  {
    std::cerr << "small inline callable failed\n";
    return 1;
  }

  direct_called = false;

  // A large capture forces the PMR-backed fallback, verifying that the
  // explicitly supplied resource is honored when allocation is necessary.
  modern::detail::move_only_function direct{&resource,
    [&, payload = std::array<std::byte, 128>{}]
  {
    direct_called = payload.size() == 128;
  }};

  if (resource.allocations() == 0)
  {
    std::cerr << "move_only_function resource ignored\n";
    return 1;
  }

  direct();

  if (!direct_called)
  {
    std::cerr << "direct move_only_function failed\n";
    return 1;
  }

  auto scheduler = modern::scheduler{std::shared_ptr<resource_executor>{new resource_executor(&resource)}};
  int value = 0;
  auto before = resource.allocations();

  // Verify the scheduler forwards the executor's PMR resource for a
  // callable that cannot be placed into the small inline buffer.
  scheduler.execute([&, payload = std::array<std::byte, 128>{}]
  {
    value = payload.size() == 128 ? 42 : -1;
  });

  if (value != 42)
  {
    std::cerr << "inline scheduler failed\n";
    return 1;
  }

  if (resource.allocations() <= before)
  {
    std::cerr << "scheduler resource propagation failed\n";
    return 1;
  }

  return 0;
}