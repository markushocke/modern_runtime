#ifndef MODERN_RUNTIME_MOVE_ONLY_FUNCTION_HPP
#define MODERN_RUNTIME_MOVE_ONLY_FUNCTION_HPP

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>

namespace modern::detail
{
// Move-only type erasure with a small, allocation-free representation for
// nothrow-movable callables. Large / over-aligned / potentially-throwing
// move types retain the PMR-backed heap representation.
class move_only_function
{
public:
  move_only_function() = default;

  template<class F>
    requires (!std::same_as<std::remove_cvref_t<F>, move_only_function>) &&
             std::invocable<F&>
  move_only_function(F&& f)
    : move_only_function(std::pmr::get_default_resource(), std::forward<F>(f))
  {
  }

  template<class F>
    requires (!std::same_as<std::remove_cvref_t<F>, move_only_function>) &&
             std::invocable<F&>
  move_only_function(std::pmr::memory_resource* resource, F&& f)
  {
    emplace(resource, std::forward<F>(f));
  }

  move_only_function(move_only_function&& other) noexcept
  {
    take_from(other);
  }

  move_only_function& operator=(move_only_function&& other) noexcept
  {
    if (this != &other)
    {
      reset();
      take_from(other);
    }
    return *this;
  }

  move_only_function(const move_only_function&) = delete;
  move_only_function& operator=(const move_only_function&) = delete;

  ~move_only_function()
  {
    reset();
  }

  void operator()()
  {
    if (!self_)
      throw std::bad_function_call{};

    self_->call();
  }

  explicit operator bool() const noexcept
  {
    return self_ != nullptr;
  }

private:
  static constexpr std::size_t inline_bytes = 48;

  struct concept_t
  {
    virtual ~concept_t() = default;
    virtual void call() = 0;
    virtual void destroy(std::pmr::memory_resource* resource) noexcept = 0;
    virtual void destroy_inline() noexcept = 0;
    virtual concept_t* move_inline_to(void* target) noexcept = 0;
  };

  template<class F>
  struct model final : concept_t
  {
    template<class U>
    explicit model(U&& f)
      : f_(std::forward<U>(f))
    {
    }

    void call() override
    {
      std::invoke(f_);
    }

    void destroy(std::pmr::memory_resource* resource) noexcept override
    {
      using allocator_type = std::pmr::polymorphic_allocator<model>;
      allocator_type allocator{resource};
      std::allocator_traits<allocator_type>::destroy(allocator, this);
      allocator.deallocate(this, 1);
    }

    void destroy_inline() noexcept override
    {
      std::destroy_at(this);
    }

    concept_t* move_inline_to(void* target) noexcept override
    {
      // Invoked only for types that are statically nothrow-move-constructible.
      return ::new (target) model{std::move(f_)};
    }

    F f_;
  };

  template<class F>
  void emplace(std::pmr::memory_resource* resource, F&& f)
  {
    using model_type = model<std::remove_cvref_t<F>>;
    resource_ = resource ? resource : std::pmr::get_default_resource();

    if constexpr (sizeof(model_type) <= inline_bytes &&
                  alignof(model_type) <= alignof(std::max_align_t) &&
                  std::is_nothrow_move_constructible_v<model_type>)
    {
      self_ = ::new (static_cast<void*>(inline_storage_))
        model_type(std::forward<F>(f));
      inline_ = true;
    }
    else
    {
      using allocator_type = std::pmr::polymorphic_allocator<model_type>;
      allocator_type allocator{resource_};
      auto* storage = allocator.allocate(1);
      try
      {
        std::allocator_traits<allocator_type>::construct(
          allocator, storage, std::forward<F>(f));
        self_ = storage;
      }
      catch (...)
      {
        allocator.deallocate(storage, 1);
        throw;
      }
    }
  }

  void take_from(move_only_function& other) noexcept
  {
    resource_ = std::exchange(other.resource_, std::pmr::get_default_resource());
    inline_ = std::exchange(other.inline_, false);
    if (inline_)
    {
      self_ = other.self_->move_inline_to(inline_storage_);
      other.self_->destroy_inline();
      other.self_ = nullptr;
    }
    else
    {
      self_ = std::exchange(other.self_, nullptr);
    }
  }

  void reset() noexcept
  {
    if (self_)
    {
      if (inline_)
        self_->destroy_inline();
      else
        self_->destroy(resource_);
    }
    self_ = nullptr;
    inline_ = false;
    resource_ = std::pmr::get_default_resource();
  }

  alignas(std::max_align_t) std::byte inline_storage_[inline_bytes]{};
  concept_t* self_ = nullptr;
  std::pmr::memory_resource* resource_ = std::pmr::get_default_resource();
  bool inline_ = false;
};
} // namespace modern::detail

#endif
