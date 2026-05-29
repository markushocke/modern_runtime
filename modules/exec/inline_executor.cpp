module;

#include "../detail/move_only_function_support.hpp"
#include "../detail/shared_ptr.hpp"

#include <memory>

module modern.exec;

import :api;
import :detail;

namespace modern::detail
{
void inline_executor::execute(move_only_function task)
{
  task();
}
} // namespace modern::detail

namespace modern
{
scheduler inline_scheduler()
{
  static std::shared_ptr<detail::inline_executor> instance =
    detail::make_shared_object<detail::inline_executor>();
  return scheduler{instance};
}
} // namespace modern
