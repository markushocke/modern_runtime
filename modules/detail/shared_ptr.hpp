#ifndef MODERN_RUNTIME_SHARED_PTR_HPP
#define MODERN_RUNTIME_SHARED_PTR_HPP

#include <memory>
#include <memory_resource>
#include <utility>

namespace modern::detail
{
template<class T>
struct pmr_deleter
{
  std::pmr::memory_resource* resource;

  void operator()(T* object) const noexcept
  {
    std::pmr::polymorphic_allocator<T> deleter_allocator{resource};
    std::allocator_traits<std::pmr::polymorphic_allocator<T>>::destroy(deleter_allocator, object);
    deleter_allocator.deallocate(object, 1);
  }
};

template<class T, class... Args>
std::shared_ptr<T> make_shared_object(Args&&... args)
{
  return std::shared_ptr<T>{new T(std::forward<Args>(args)...)};
}

template<class T, class... Args>
std::shared_ptr<T> allocate_shared_object(std::pmr::memory_resource* resource, Args&&... args)
{
  auto* actual_resource = std::pmr::get_default_resource();

  if (resource)
    actual_resource = resource;

  std::pmr::polymorphic_allocator<T> allocator{actual_resource};
  auto* storage = allocator.allocate(1);

  try
  {
    std::allocator_traits<std::pmr::polymorphic_allocator<T>>::construct(
      allocator,
      storage,
      std::forward<Args>(args)...);
  }
  catch (...)
  {
    allocator.deallocate(storage, 1);
    throw;
  }

  return std::shared_ptr<T>{storage, pmr_deleter<T>{actual_resource}};
}
} // namespace modern::detail

#endif
