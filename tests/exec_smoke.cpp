import modern.exec;

#include "../modules/detail/move_only_function_support.hpp"

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

  modern::detail::move_only_function direct{&resource, [&]
  {
    direct_called = true;
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

  scheduler.execute([&]
  {
    value = 42;
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