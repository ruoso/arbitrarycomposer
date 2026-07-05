#include <arbc/cache/keyed_store.hpp>

namespace arbc {
namespace detail {

// The single authoritative eviction walk order (doc 02): Speculative first,
// Visible last. KeyedStore's eviction loop reads this so the ordering lives in
// exactly one place; adding a class (e.g. doc 11's temporal ring, owned by
// `cache.prefetch`) is a one-line change here plus the enum.
const std::array<PriorityClass, k_priority_class_count>& cache_eviction_order() {
  static const std::array<PriorityClass, k_priority_class_count> order = {
      PriorityClass::Speculative,
      PriorityClass::Recent,
      PriorityClass::Adjacent,
      PriorityClass::Visible,
  };
  static_assert(k_priority_class_count == 4, "eviction order must list every PriorityClass");
  return order;
}

} // namespace detail
} // namespace arbc
