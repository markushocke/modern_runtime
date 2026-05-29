module;

#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <type_traits>

export module modern.runtime:io_backend;

namespace modern::detail
{
template<class T>
inline constexpr bool io_backend_false = false;
}

export namespace modern
{
enum class io_backend_kind
{
  unknown,
  io_uring,
  epoll,
  kqueue,
};

enum class poller_backend_kind
{
  epoll,
  kqueue,
};

struct io_backend_info
{
  io_backend_kind kind = io_backend_kind::unknown;
  std::size_t submission_queue_depth = 0;
  std::size_t completion_queue_depth = 0;
};

template<class Service>
io_backend_info query_io_backend(Service& service)
{
  if constexpr (requires(Service& candidate)
  {
    { candidate.backend_info() } -> std::convertible_to<io_backend_info>;
  })
  {
    return service.backend_info();
  }
  else
  {
    io_backend_info info;

    if constexpr (requires(Service& candidate)
    {
      { candidate.backend_kind() } -> std::convertible_to<io_backend_kind>;
    })
    {
      info.kind = service.backend_kind();
    }
    else if constexpr (requires(Service& candidate)
    {
      { candidate.backend() } -> std::convertible_to<io_backend_kind>;
    })
    {
      info.kind = service.backend();
    }
    else if constexpr (requires(Service& candidate)
    {
      { candidate.kind() } -> std::convertible_to<io_backend_kind>;
    })
    {
      info.kind = service.kind();
    }
    else
    {
      static_assert(detail::io_backend_false<Service>,
        "service must expose backend_info(), backend_kind(), backend(), or kind() for backend inspection");
    }

    if constexpr (requires(Service& candidate)
    {
      { candidate.submission_queue_depth() } -> std::convertible_to<std::size_t>;
    })
    {
      info.submission_queue_depth = service.submission_queue_depth();
    }
    else if constexpr (requires(Service& candidate)
    {
      { candidate.queue_depth() } -> std::convertible_to<std::size_t>;
    })
    {
      info.submission_queue_depth = service.queue_depth();
    }

    if constexpr (requires(Service& candidate)
    {
      { candidate.completion_queue_depth() } -> std::convertible_to<std::size_t>;
    })
    {
      info.completion_queue_depth = service.completion_queue_depth();
    }
    else
    {
      info.completion_queue_depth = info.submission_queue_depth;
    }

    return info;
  }
}

template<class Service>
poller_backend_kind query_poller_backend(Service& service)
{
  auto info = modern::query_io_backend(service);

  switch (info.kind)
  {
    case io_backend_kind::epoll:
      return poller_backend_kind::epoll;
    case io_backend_kind::kqueue:
      return poller_backend_kind::kqueue;
    default:
      throw std::runtime_error("service is not poller-backed");
  }
}
} // namespace modern