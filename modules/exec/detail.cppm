module;

#include "../detail/move_only_function_support.hpp"

module modern.exec:detail;

namespace modern::detail
{
class inline_executor
{
public:
  void execute(move_only_function task);

  [[nodiscard]] std::pmr::memory_resource* resource() const noexcept
  {
    return std::pmr::get_default_resource();
  }
};
} // namespace modern::detail
