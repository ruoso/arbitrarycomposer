#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace arbc {

// Stable, document-unique object identity (doc 14). Zero is never a valid
// id.
struct ObjectId {
  std::uint64_t value{0};

  constexpr bool valid() const { return value != 0; }

  friend constexpr bool operator==(const ObjectId&, const ObjectId&) = default;
};

} // namespace arbc

template <> struct std::hash<arbc::ObjectId> {
  std::size_t operator()(const arbc::ObjectId& id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value);
  }
};
