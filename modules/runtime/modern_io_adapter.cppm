module;

#include <concepts>
#include <memory_resource>
#include <stop_token>
#include <type_traits>
#include <utility>

export module modern.runtime:modern_io_adapter;

export import modern.exec;
export import modern.memory;
export import modern.task;
export import :io_bridge;

namespace modern::detail
{
template<class Service, class Operation, class Completion>
void start_modern_io(Service& service, Operation&& operation, Completion&& completion)
{
  if constexpr (requires(Service& candidate, Operation&& op, Completion&& done)
  {
    candidate.submit(std::forward<Operation>(op), std::forward<Completion>(done));
  })
  {
    service.submit(std::forward<Operation>(operation), std::forward<Completion>(completion));
  }
  else if constexpr (requires(Service& candidate, Operation&& op, Completion&& done)
  {
    candidate.start(std::forward<Operation>(op), std::forward<Completion>(done));
  })
  {
    service.start(std::forward<Operation>(operation), std::forward<Completion>(completion));
  }
  else
  {
    static_assert(always_false<Service>,
      "modern_io adapter service must expose submit(operation, completion) or start(operation, completion)");
  }
}

template<class Service, class Operation, class Completion>
void start_modern_io(Service& service, Operation&& operation, std::stop_token token, Completion&& completion)
{
  if constexpr (requires(Service& candidate, Operation&& op, std::stop_token stop, Completion&& done)
  {
    candidate.submit(std::forward<Operation>(op), stop, std::forward<Completion>(done));
  })
  {
    service.submit(std::forward<Operation>(operation), token, std::forward<Completion>(completion));
  }
  else if constexpr (requires(Service& candidate, Operation&& op, std::stop_token stop, Completion&& done)
  {
    candidate.start(std::forward<Operation>(op), stop, std::forward<Completion>(done));
  })
  {
    service.start(std::forward<Operation>(operation), token, std::forward<Completion>(completion));
  }
  else
  {
    modern::detail::start_modern_io(service, std::forward<Operation>(operation), std::forward<Completion>(completion));
  }
}
} // namespace modern::detail

export namespace modern
{
template<class Service>
class modern_io_service_adapter
{
public:
  explicit modern_io_service_adapter(Service& service) noexcept
    : service_(&service)
  {
  }

  [[nodiscard]] scheduler completion_scheduler() const
  {
    return detail::io_bridge_scheduler(*service_);
  }

  [[nodiscard]] memory::memory_resource* resource() const noexcept
  {
    return detail::io_bridge_resource(*service_);
  }

  template<class R, class Operation>
  auto submit(Operation&& operation)
  {
    return submit<R>(resource(), std::forward<Operation>(operation));
  }

  template<class R, class Operation>
  auto submit(memory::memory_resource* resource_override, Operation&& operation)
  {
    using operation_type = std::remove_cvref_t<Operation>;
    auto* actual_resource = resource_override ? resource_override : resource();

    return modern::bind_io<R>(
      completion_scheduler(),
      actual_resource,
      [service = service_, operation = operation_type(std::forward<Operation>(operation))](io_task_completion<R> completion) mutable
      {
        detail::start_modern_io(*service, std::move(operation), std::move(completion));
      });
  }

  template<class R, class Operation>
  auto submit(std::stop_token token, Operation&& operation)
  {
    return submit<R>(token, resource(), std::forward<Operation>(operation));
  }

  template<class R, class Operation>
  auto submit(std::stop_token token, memory::memory_resource* resource_override, Operation&& operation)
  {
    using operation_type = std::remove_cvref_t<Operation>;
    auto* actual_resource = resource_override ? resource_override : resource();

    return modern::bind_io<R>(
      completion_scheduler(),
      token,
      actual_resource,
      [service = service_, operation = operation_type(std::forward<Operation>(operation))](
        io_task_completion<R> completion,
        std::stop_token inner_token) mutable
      {
        detail::start_modern_io(*service, std::move(operation), inner_token, std::move(completion));
      });
  }

private:
  Service* service_;
};

template<class Service>
auto adapt_modern_io(Service& service) noexcept
{
  return modern_io_service_adapter<Service>{service};
}
} // namespace modern