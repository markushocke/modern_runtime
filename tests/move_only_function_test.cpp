#include "../modules/detail/move_only_function.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace
{
void require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

class counting_resource final : public std::pmr::memory_resource
{
public:
  std::size_t allocations() const noexcept { return allocated_.load(); }
  std::size_t deallocations() const noexcept { return deallocated_.load(); }

private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override
  {
    void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
    allocated_.fetch_add(1);
    return result;
  }

  void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override
  {
    std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    deallocated_.fetch_add(1);
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
  {
    return this == &other;
  }

  std::atomic<std::size_t> allocated_{0};
  std::atomic<std::size_t> deallocated_{0};
};

struct small_move_only
{
  std::unique_ptr<int> value;
  int* output;

  void operator()() { *output += *value; }
};

struct large_move_only
{
  std::array<std::byte, 256> payload{};
  std::unique_ptr<int> value;
  int* output;

  void operator()() { *output += *value; }
};

// This capture is designed to fit in a 48-byte erased model, but not 32.
struct medium_callable
{
  std::array<std::uintptr_t, 3> payload{{1, 2, 3}};
  int* output;

  void operator()() { *output = static_cast<int>(payload[0] + payload[1] + payload[2]); }
};

struct alignas(64) aligned_callable
{
  bool* aligned;
  void operator()()
  {
    *aligned = reinterpret_cast<std::uintptr_t>(this) % 64 == 0;
  }
};

void test_inline_move_only()
{
  counting_resource resource;
  int observed = 0;
  {
    modern::detail::move_only_function first{
      &resource, small_move_only{std::make_unique<int>(21), &observed}};
    require(resource.allocations() == 0, "small callable allocated");

    modern::detail::move_only_function second{std::move(first)};
    require(!first && second, "move constructor invalidated callable");
    second();
    require(observed == 21, "inline callable produced wrong result");

    modern::detail::move_only_function third;
    third = std::move(second);
    require(!second && third, "move assignment invalidated callable");
    // Exercise self-move via an alias without triggering compiler self-move warnings.
    auto& same = third;
    third = std::move(same);
    third();
    require(observed == 42, "self-move assignment broke callable");
  }
  require(resource.allocations() == 0, "inline move allocated");
  require(resource.deallocations() == 0, "inline move deallocated heap");
}

void test_heap_fallback()
{
  counting_resource resource;
  int observed = 0;
  {
    modern::detail::move_only_function first{
      &resource, large_move_only{{}, std::make_unique<int>(7), &observed}};
    require(resource.allocations() == 1, "large callable missed PMR allocation");
    modern::detail::move_only_function second{std::move(first)};
    require(!first && second, "heap callable move failed");
    second();
    require(observed == 7, "large callable produced wrong result");

    second = modern::detail::move_only_function{
      &resource, small_move_only{std::make_unique<int>(3), &observed}};
    require(resource.deallocations() == 1, "reassignment leaked large callable");
    second();
    require(observed == 10, "heap-to-inline move assignment failed");
  }
  require(resource.allocations() == resource.deallocations(), "heap callable leaked");
}

void test_inline_boundary()
{
  counting_resource resource;
  int output = 0;
  {
    modern::detail::move_only_function callable{&resource, medium_callable{{1, 2, 3}, &output}};
#if MODERN_RUNTIME_INLINE_CALLABLE_BYTES == 32
    require(resource.allocations() == 1, "32-byte inline capacity unexpectedly held medium callable");
#elif MODERN_RUNTIME_INLINE_CALLABLE_BYTES == 48
    require(resource.allocations() == 0, "48-byte inline capacity missed medium callable");
#else
#error "Unexpected MODERN_RUNTIME_INLINE_CALLABLE_BYTES"
#endif
    callable();
    require(output == 6, "medium callable executed incorrectly");
  }
  require(resource.allocations() == resource.deallocations(),
          "medium callable lifetime leak");
}

void test_alignment_and_exceptions()
{
  counting_resource resource;
  bool aligned = false;
  {
    modern::detail::move_only_function function{&resource, aligned_callable{&aligned}};
    require(resource.allocations() == 1, "over-aligned callable was inlined");
    function();
  }
  require(aligned, "over-aligned callable stored at incorrect alignment");
  require(resource.allocations() == resource.deallocations(), "aligned callable leaked");

  modern::detail::move_only_function empty;
  bool empty_threw = false;
  try { empty(); }
  catch (const std::bad_function_call&) { empty_threw = true; }
  require(empty_threw, "empty invocation failed to throw bad_function_call");

  modern::detail::move_only_function throwing{[]
  {
    throw std::runtime_error("expected");
  }};
  bool propagated = false;
  try { throwing(); }
  catch (const std::runtime_error&) { propagated = true; }
  require(propagated, "callable exception was lost");
}
}

int main()
{
  static_assert(!std::is_copy_constructible_v<modern::detail::move_only_function>);
  static_assert(std::is_nothrow_move_constructible_v<modern::detail::move_only_function>);
  try
  {
    test_inline_move_only();
    test_heap_fallback();
    test_inline_boundary();
    test_alignment_and_exceptions();
    std::cout << "PASS: move_only_function inline, PMR fallback and lifetime\n";
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "move_only_function_test: " << error.what() << '\n';
    return 1;
  }
}
