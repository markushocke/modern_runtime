module;

#include "../detail/move_only_function_support.hpp"

#include <stdexcept>
#include <utility>

module modern.exec;

import :api;

namespace modern
{
void scheduler::execute_impl(detail::move_only_function task, scheduler_priority priority) const
{
  if (!impl_ || !execute_)
    throw std::runtime_error("empty scheduler");

  execute_(impl_.get(), std::move(task), priority);
}
} // namespace modern
