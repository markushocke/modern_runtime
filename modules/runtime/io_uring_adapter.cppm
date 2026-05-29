module;

#include <memory_resource>
#include <stdexcept>
#include <stop_token>
#include <utility>

export module modern.runtime:io_uring_adapter;

export import modern.memory;
export import modern.task;
export import :io_backend;
export import :modern_io_adapter;

export namespace modern
{
template<class Service>
class modern_io_uring_adapter
{
public:
  explicit modern_io_uring_adapter(Service& service)
    : service_(&service),
      adapter_(service)
  {
    auto info = backend_info();

    if (info.kind != io_backend_kind::io_uring)
      throw std::runtime_error("service is not io_uring-backed");
  }

  [[nodiscard]] io_backend_info backend_info() const
  {
    return modern::query_io_backend(*service_);
  }

  [[nodiscard]] scheduler completion_scheduler() const
  {
    return adapter_.completion_scheduler();
  }

  [[nodiscard]] memory::memory_resource* resource() const noexcept
  {
    return adapter_.resource();
  }

  template<class R, class Operation>
  auto submit(Operation&& operation)
  {
    return adapter_.template submit<R>(std::forward<Operation>(operation));
  }

  template<class R, class Operation>
  auto submit(memory::memory_resource* resource_override, Operation&& operation)
  {
    return adapter_.template submit<R>(resource_override, std::forward<Operation>(operation));
  }

  template<class R, class Operation>
  auto submit(std::stop_token token, Operation&& operation)
  {
    return adapter_.template submit<R>(token, std::forward<Operation>(operation));
  }

  template<class R, class Operation>
  auto submit(std::stop_token token, memory::memory_resource* resource_override, Operation&& operation)
  {
    return adapter_.template submit<R>(token, resource_override, std::forward<Operation>(operation));
  }

private:
  Service* service_;
  modern_io_service_adapter<Service> adapter_;
};

template<class Service>
auto adapt_modern_io_uring(Service& service)
{
  return modern_io_uring_adapter<Service>{service};
}
} // namespace modern