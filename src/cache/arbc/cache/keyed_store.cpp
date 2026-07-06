#include <arbc/cache/keyed_store.hpp>

namespace arbc {
namespace detail {

// The single authoritative eviction walk order (doc 02 + doc 11): Speculative
// first, Visible last, with doc 11's temporal ring between Recent and Adjacent.
// KeyedStore's eviction loop reads this so the ordering lives in exactly one
// place; adding a class (doc 11's temporal ring, owned by `cache.prefetch`) is
// a one-line change here plus the enum.
const std::array<PriorityClass, k_priority_class_count>& cache_eviction_order() {
  static const std::array<PriorityClass, k_priority_class_count> order = {
      PriorityClass::Speculative, PriorityClass::Recent,  PriorityClass::Temporal,
      PriorityClass::Adjacent,    PriorityClass::Visible,
  };
  static_assert(k_priority_class_count == 5, "eviction order must list every PriorityClass");
  return order;
}

} // namespace detail
} // namespace arbc
