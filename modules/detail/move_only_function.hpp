#ifndef MODERN_RUNTIME_MOVE_ONLY_FUNCTION_HPP
#define MODERN_RUNTIME_MOVE_ONLY_FUNCTION_HPP

namespace modern::detail
{
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
    : self_(std::exchange(other.self_, nullptr)),
      resource_(std::exchange(other.resource_, std::pmr::get_default_resource()))
  {
  }

  move_only_function& operator=(move_only_function&& other) noexcept
  {
    if (this == &other)
      return *this;

    reset();
    self_ = std::exchange(other.self_, nullptr);
    resource_ = std::exchange(other.resource_, std::pmr::get_default_resource());
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
    return static_cast<bool>(self_);
  }

private:
  struct concept_t
  {
    virtual ~concept_t() = default;
    virtual void call() = 0;
    virtual void destroy(std::pmr::memory_resource* resource) noexcept = 0;
  };

  template<class F>
  struct model final : concept_t
  {
    explicit model(F&& f)
      : f_(std::move(f))
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

    F f_;
  };

  template<class F>
  void emplace(std::pmr::memory_resource* resource, F&& f)
  {
    using model_type = model<std::remove_cvref_t<F>>;
    using allocator_type = std::pmr::polymorphic_allocator<model_type>;

    resource_ = resource ? resource : std::pmr::get_default_resource();

    allocator_type allocator{resource_};
    auto* storage = allocator.allocate(1);

    try
    {
      std::allocator_traits<allocator_type>::construct(allocator, storage, std::forward<F>(f));
      self_ = storage;
    }
    catch (...)
    {
      allocator.deallocate(storage, 1);
      throw;
    }
  }

  void reset() noexcept
  {
    if (!self_)
      return;

    self_->destroy(resource_);
    self_ = nullptr;
    resource_ = std::pmr::get_default_resource();
  }

  concept_t* self_ = nullptr;
  std::pmr::memory_resource* resource_ = std::pmr::get_default_resource();
};
} // namespace modern::detail

#endif
