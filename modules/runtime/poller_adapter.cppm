module;

#include <memory_resource>
#include <stdexcept>
#include <stop_token>
#include <utility>

export module modern.runtime:poller_adapter;

export import modern.memory;
export import modern.task;
export import :io_backend;
export import :modern_io_adapter;

export namespace modern
{
template<class Service>
class modern_io_poller_adapter
{
public:
  explicit modern_io_poller_adapter(Service& service)
    : service_(&service),
      adapter_(service),
      kind_(modern::query_poller_backend(service))
  {
  }

  [[nodiscard]] poller_backend_kind poller_kind() const noexcept
  {
    return kind_;
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
  poller_backend_kind kind_;
};

template<class Service>
auto adapt_modern_io_poller(Service& service)
{
  return modern_io_poller_adapter<Service>{service};
}
} // namespace modern