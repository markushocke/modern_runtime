module;

#include <concepts>
#include <stop_token>
#include <type_traits>
#include <utility>

export module modern.runtime:thread_adapter;

export import modern.memory;
export import modern.task;
export import modern.thread;
export import modern.exec;

export namespace modern
{
template<class F, class... Args>
auto submit(thread_pool& pool, memory::memory_resource* resource, F&& f, Args&&... args)
{
  return modern::submit(pool.get_scheduler(), resource, std::forward<F>(f), std::forward<Args>(args)...);
}

template<class F, class... Args>
auto submit(thread_pool& pool, std::stop_token token, memory::memory_resource* resource, F&& f, Args&&... args)
{
  return modern::submit(pool.get_scheduler(), token, resource, std::forward<F>(f), std::forward<Args>(args)...);
}

template<class F, class... Args>
  requires (!std::convertible_to<std::remove_cvref_t<F>, memory::memory_resource*>)
auto submit(thread_pool& pool, F&& f, Args&&... args)
{
  return modern::submit(pool.get_scheduler(), pool.resource(), std::forward<F>(f), std::forward<Args>(args)...);
}

template<class F, class... Args>
auto submit(thread_pool& pool, std::stop_token token, F&& f, Args&&... args)
{
  return modern::submit(pool.get_scheduler(), token, pool.resource(), std::forward<F>(f), std::forward<Args>(args)...);
}
} // namespace modern
