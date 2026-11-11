#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace san4 {

template <typename Signature>
class FunctionRef;

// Non-owning, non-allocating reference to a callable. The referenced callable
// must outlive every invocation through the FunctionRef (typically the
// callable is a lambda living in the caller's stack frame).
template <typename R, typename... Args>
class FunctionRef<R(Args...)> {
 public:
  template <typename F>
    requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> &&
             std::is_invocable_r_v<R, F&, Args...>)
  FunctionRef(F&& fn) noexcept  // Implicit so lambdas bind at call sites.
      : obj_(const_cast<void*>(
            static_cast<const void*>(std::addressof(fn)))),
        call_(&invoke_fn<std::remove_reference_t<F>>) {}

  R operator()(Args... args) const {
    return call_(obj_, std::forward<Args>(args)...);
  }

 private:
  template <typename F>
  static R invoke_fn(void* obj, Args... args) {
    return std::invoke(*static_cast<F*>(obj), std::forward<Args>(args)...);
  }

  void* obj_;
  R (*call_)(void*, Args...);
};

}  // namespace san4
