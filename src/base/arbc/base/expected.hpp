#pragma once

#include <cassert>
#include <type_traits>
#include <utility>

namespace arbc {

// Error wrapper disambiguating the error alternative of `expected` from its
// value alternative (needed when T and E are convertible, and to make the
// error construction explicit at the call site: `unexpected<E>(e)`).
template <class E> class unexpected {
public:
  explicit unexpected(E error) : d_error(std::move(error)) {}

  const E& error() const& noexcept { return d_error; }
  E& error() & noexcept { return d_error; }
  E&& error() && noexcept { return std::move(d_error); }

private:
  E d_error;
};

// Minimal value-or-error result type (doc 10: errors as values, never
// thrown, never aborting across the public API). Deliberately small: value
// or error, no monadic `and_then` until a consumer needs it. `arbc::pool`
// is the first consumer and fixes the shape.
template <class T, class E> class expected {
public:
  expected(const T& value) : d_has_value(true) { ::new (&d_value) T(value); }
  expected(T&& value) : d_has_value(true) { ::new (&d_value) T(std::move(value)); }

  expected(const unexpected<E>& error) : d_has_value(false) { ::new (&d_error) E(error.error()); }
  expected(unexpected<E>&& error) : d_has_value(false) {
    ::new (&d_error) E(std::move(error).error());
  }

  expected(const expected& other) : d_has_value(other.d_has_value) {
    if (d_has_value) {
      ::new (&d_value) T(other.d_value);
    } else {
      ::new (&d_error) E(other.d_error);
    }
  }
  expected(expected&& other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                      std::is_nothrow_move_constructible_v<E>)
      : d_has_value(other.d_has_value) {
    if (d_has_value) {
      ::new (&d_value) T(std::move(other.d_value));
    } else {
      ::new (&d_error) E(std::move(other.d_error));
    }
  }

  expected& operator=(const expected& other) {
    if (this != &other) {
      destroy();
      d_has_value = other.d_has_value;
      if (d_has_value) {
        ::new (&d_value) T(other.d_value);
      } else {
        ::new (&d_error) E(other.d_error);
      }
    }
    return *this;
  }
  expected& operator=(expected&& other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                                 std::is_nothrow_move_constructible_v<E>) {
    if (this != &other) {
      destroy();
      d_has_value = other.d_has_value;
      if (d_has_value) {
        ::new (&d_value) T(std::move(other.d_value));
      } else {
        ::new (&d_error) E(std::move(other.d_error));
      }
    }
    return *this;
  }

  ~expected() { destroy(); }

  bool has_value() const noexcept { return d_has_value; }
  explicit operator bool() const noexcept { return d_has_value; }

  T& operator*() & noexcept {
    assert(d_has_value);
    return d_value;
  }
  const T& operator*() const& noexcept {
    assert(d_has_value);
    return d_value;
  }
  T* operator->() noexcept {
    assert(d_has_value);
    return &d_value;
  }
  const T* operator->() const noexcept {
    assert(d_has_value);
    return &d_value;
  }

  T& value() & noexcept {
    assert(d_has_value);
    return d_value;
  }
  const T& value() const& noexcept {
    assert(d_has_value);
    return d_value;
  }

  E& error() & noexcept {
    assert(!d_has_value);
    return d_error;
  }
  const E& error() const& noexcept {
    assert(!d_has_value);
    return d_error;
  }

private:
  void destroy() noexcept {
    if (d_has_value) {
      d_value.~T();
    } else {
      d_error.~E();
    }
  }

  bool d_has_value;
  union {
    T d_value;
    E d_error;
  };
};

} // namespace arbc
