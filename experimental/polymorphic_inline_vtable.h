/* Copyright (c) 2016 The Value Types Authors. All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
==============================================================================*/

#ifndef XYZ_POLYMORPHIC_H_
#define XYZ_POLYMORPHIC_H_

#include <cassert>
#include <concepts>
#include <memory>
#include <utility>

namespace xyz {

struct NoPolymorphicSBO {};

namespace detail {
template <class T, class A>
struct control_block {
  T* p_;
  typedef void(destroy_fn)(control_block*, A& alloc);
  typedef control_block<T, A>*(clone_fn)(const control_block*, A& alloc);

  struct vtable {
    destroy_fn* const destroy;
    clone_fn* const clone;
  } local_vtable_;

  constexpr control_block(destroy_fn* const destroy, clone_fn* const clone)
      : local_vtable_{.destroy = destroy, .clone = clone} {}

  constexpr void destroy(A& alloc) { local_vtable_.destroy(this, alloc); }

  constexpr control_block* clone(A& alloc) const {
    return local_vtable_.clone(this, alloc);
  }
};

template <class T, class U, class A>
class direct_control_block : public control_block<T, A> {
  U u_;

  using cb_allocator = typename std::allocator_traits<A>::template rebind_alloc<
      direct_control_block<T, U, A>>;
  using cb_alloc_traits = std::allocator_traits<cb_allocator>;

 public:
  template <class... Ts>
  constexpr direct_control_block(Ts&&... ts)
      : control_block<T, A>{
            // destroy
            +[](control_block<T, A>* p, A& alloc) {
              auto cb = static_cast<direct_control_block*>(p);
              cb_allocator cb_alloc(alloc);
              cb_alloc_traits::destroy(cb_alloc, cb);
              cb_alloc_traits::deallocate(cb_alloc, cb, 1);
            },
            // clone
            +[](const control_block<T, A>* p,
                A& alloc) -> control_block<T, A>* {
              auto cb = static_cast<const direct_control_block*>(p);
              cb_allocator cb_alloc(alloc);
              auto* mem = cb_alloc_traits::allocate(cb_alloc, 1);
              try {
                cb_alloc_traits::construct(cb_alloc, mem, cb->u_);
                return mem;
              } catch (...) {
                cb_alloc_traits::deallocate(cb_alloc, mem, 1);
                throw;
              }
            }},
        u_(std::forward<Ts>(ts)...) {
    this->p_ = &u_;
  }
};

}  // namespace detail

template <class T, class A = std::allocator<T>>
class polymorphic {
  detail::control_block<T, A>* cb_;

#if defined(_MSC_VER)
  [[msvc::no_unique_address]] A alloc_;
#else
  [[no_unique_address]] A alloc_;
#endif

  using allocator_traits = std::allocator_traits<A>;

 public:
  using value_type = T;
  using allocator_type = A;

  constexpr polymorphic()
    requires std::default_initializable<T>
  {
    using cb_allocator = typename std::allocator_traits<
        A>::template rebind_alloc<detail::direct_control_block<T, T, A>>;
    using cb_traits = std::allocator_traits<cb_allocator>;
    cb_allocator cb_alloc(alloc_);
    auto* mem = cb_traits::allocate(cb_alloc, 1);
    try {
      cb_traits::construct(cb_alloc, mem);
      cb_ = mem;
    } catch (...) {
      cb_traits::deallocate(cb_alloc, mem, 1);
      throw;
    }
  }

  template <class U, class... Ts>
  constexpr explicit polymorphic(std::in_place_type_t<U>, Ts&&... ts)
    requires std::constructible_from<U, Ts&&...> &&
             std::copy_constructible<U> &&
             (std::derived_from<U, T> || std::same_as<U, T>)
  {
    using cb_allocator = typename std::allocator_traits<
        A>::template rebind_alloc<detail::direct_control_block<T, U, A>>;
    using cb_traits = std::allocator_traits<cb_allocator>;
    cb_allocator cb_alloc(alloc_);
    auto* mem = cb_traits::allocate(cb_alloc, 1);
    try {
      cb_traits::construct(cb_alloc, mem, std::forward<Ts>(ts)...);
      cb_ = mem;
    } catch (...) {
      cb_traits::deallocate(cb_alloc, mem, 1);
      throw;
    }
  }

  template <class U, class... Ts>
  constexpr polymorphic(std::allocator_arg_t, const A& alloc,
                        std::in_place_type_t<U>, Ts&&... ts)
    requires std::constructible_from<U, Ts&&...> &&
             std::copy_constructible<U> &&
             (std::derived_from<U, T> || std::same_as<U, T>)
      : alloc_(alloc) {
    using cb_allocator = typename std::allocator_traits<
        A>::template rebind_alloc<detail::direct_control_block<T, U, A>>;
    using cb_traits = std::allocator_traits<cb_allocator>;
    cb_allocator cb_alloc(alloc_);
    auto* mem = cb_traits::allocate(cb_alloc, 1);
    try {
      cb_traits::construct(cb_alloc, mem, std::forward<Ts>(ts)...);
      cb_ = mem;
    } catch (...) {
      cb_traits::deallocate(cb_alloc, mem, 1);
      throw;
    }
  }

  constexpr polymorphic(const polymorphic& other)
      : alloc_(allocator_traits::select_on_container_copy_construction(
            other.alloc_)) {
    assert(other.cb_ != nullptr);  // LCOV_EXCL_LINE
    cb_ = other.cb_->clone(alloc_);
  }

  constexpr polymorphic(std::allocator_arg_t, const A& alloc,
                        const polymorphic& other)
      : alloc_(alloc) {
    assert(other.cb_ != nullptr);  // LCOV_EXCL_LINE
    cb_ = other.cb_->clone(alloc_);
  }

  constexpr polymorphic(polymorphic&& other) noexcept
      : alloc_(std::move(other.alloc_)) {
    assert(other.cb_ != nullptr);  // LCOV_EXCL_LINE
    cb_ = std::exchange(other.cb_, nullptr);
  }

  constexpr polymorphic(std::allocator_arg_t, const A& alloc,
                        polymorphic&& other) noexcept
      : alloc_(alloc) {
    assert(other.cb_ != nullptr);  // LCOV_EXCL_LINE
    cb_ = std::exchange(other.cb_, nullptr);
  }

  constexpr ~polymorphic() { reset(); }

  constexpr polymorphic& operator=(const polymorphic& other) {
    assert(other.cb_ != nullptr);  // LCOV_EXCL_LINE
    if (this != &other) {
      if constexpr (allocator_traits::propagate_on_container_copy_assignment::
                        value) {
        alloc_ = other.alloc_;
        polymorphic tmp(std::allocator_arg, alloc_, other);
        swap(tmp);
      } else {
        polymorphic tmp(other);
        this->swap(tmp);
      }
    }
    return *this;
  }

  constexpr polymorphic& operator=(polymorphic&& other) noexcept(
      allocator_traits::propagate_on_container_move_assignment::value) {
    assert(other.cb_ != nullptr);  // LCOV_EXCL_LINE
    reset();
    if constexpr (allocator_traits::propagate_on_container_move_assignment::
                      value) {
      alloc_ = other.alloc_;
      cb_ = std::exchange(other.cb_, nullptr);
    } else {
      if (alloc_ == other.alloc_) {
        cb_ = std::exchange(other.cb_, nullptr);
      } else {
        cb_ = other.cb_->clone(alloc_);
        other.reset();
      }
    }
    return *this;
  }

  constexpr T* operator->() noexcept {
    assert(cb_ != nullptr);  // LCOV_EXCL_LINE
    return cb_->p_;
  }

  constexpr const T* operator->() const noexcept {
    assert(cb_ != nullptr);  // LCOV_EXCL_LINE
    return cb_->p_;
  }

  constexpr T& operator*() noexcept {
    assert(cb_ != nullptr);  // LCOV_EXCL_LINE
    return *cb_->p_;
  }

  constexpr const T& operator*() const noexcept {
    assert(cb_ != nullptr);  // LCOV_EXCL_LINE
    return *cb_->p_;
  }

  constexpr bool valueless_after_move() const noexcept {
    return cb_ == nullptr;
  }

  constexpr allocator_type get_allocator() const noexcept { return alloc_; }

  constexpr void swap(polymorphic& other) noexcept(
      allocator_traits::propagate_on_container_swap::value ||
      allocator_traits::is_always_equal::value) {
    assert(other.cb_ != nullptr);  // LCOV_EXCL_LINE
    std::swap(cb_, other.cb_);
    if constexpr (allocator_traits::propagate_on_container_swap::value) {
      std::swap(alloc_, other.alloc_);
    }
  }

  friend constexpr void swap(polymorphic& lhs, polymorphic& rhs) noexcept(
      allocator_traits::propagate_on_container_swap::value ||
      allocator_traits::is_always_equal::value) {
    assert(lhs.cb_ != nullptr);  // LCOV_EXCL_LINE
    assert(rhs.cb_ != nullptr);  // LCOV_EXCL_LINE
    std::swap(lhs.cb_, rhs.cb_);
    if constexpr (allocator_traits::propagate_on_container_swap::value) {
      std::swap(lhs.alloc_, rhs.alloc_);
    }
  }

 private:
  constexpr void reset() noexcept {
    if (cb_ != nullptr) {
      cb_->destroy(alloc_);
      cb_ = nullptr;
    }
  }
};

}  // namespace xyz

template <class T, class Alloc>
struct std::uses_allocator<xyz::polymorphic<T, Alloc>, Alloc> : true_type {};

#endif  // XYZ_POLYMORPHIC_H_
