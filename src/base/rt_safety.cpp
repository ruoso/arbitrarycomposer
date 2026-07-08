#include <arbc/base/rt_safety.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace arbc {
namespace {

// Per-thread RT-guard state. Thread-local so arming the device callback thread
// never perturbs the pump / worker threads that legitimately allocate (D3,
// Constraint 5). Constant-initialized, so `armed()` is well-defined from the
// first instant of any thread's life (including before dynamic init).
thread_local unsigned tl_depth = 0;
thread_local std::uint64_t tl_allocations = 0;
thread_local std::uint64_t tl_locks = 0;
thread_local std::uint64_t tl_refcounts = 0;

#if defined(ARBC_RT_HARDENED)
[[noreturn]] void rt_abort() noexcept {
  // A forbidden operation ran under an armed `RtScope`: the callback chain is not
  // RT-clean. Abort build-failingly (doc 16:72-73) rather than glitch silently --
  // the whole point of the guarantee is that such a regression cannot merge.
  std::abort();
}
#endif

} // namespace

RtScope::RtScope() noexcept { ++tl_depth; }
RtScope::~RtScope() noexcept { --tl_depth; }

bool RtScope::armed() noexcept { return tl_depth > 0; }

std::uint64_t RtScope::allocations() noexcept { return tl_allocations; }
std::uint64_t RtScope::locks() noexcept { return tl_locks; }
std::uint64_t RtScope::refcounts() noexcept { return tl_refcounts; }

void RtScope::reset_counts() noexcept {
  tl_allocations = 0;
  tl_locks = 0;
  tl_refcounts = 0;
}

void RtScope::note_allocation() noexcept {
  ++tl_allocations;
#if defined(ARBC_RT_HARDENED)
  rt_abort();
#endif
}

void RtScope::note_lock() noexcept {
  ++tl_locks;
#if defined(ARBC_RT_HARDENED)
  rt_abort();
#endif
}

void RtScope::note_refcount() noexcept {
  ++tl_refcounts;
#if defined(ARBC_RT_HARDENED)
  rt_abort();
#endif
}

} // namespace arbc

// The hooked global `operator new` / `operator delete` (Layer B, doc 16:70-73).
// Compiled ONLY into the debug-hardened build (ARBC_RT_HARDENED): the sanitizer
// lanes (asan/tsan/rtsan) own the global allocator themselves and RealtimeSanitizer
// is the Clang-side catch, and a Release build carries no guard. When no `RtScope`
// is armed these forward straight to `malloc`/`free`, so the only observable effect
// off the RT thread is a single predictable branch. A SINGLE base TU owns the
// override so there is exactly one definition program-wide (Decision D3; a
// per-component override would risk ODR / duplicate-symbol trouble).
#if defined(ARBC_RT_HARDENED)

namespace {

void* rt_alloc(std::size_t size) {
  if (arbc::RtScope::armed()) {
    arbc::RtScope::note_allocation(); // aborts build-failingly under ARBC_RT_HARDENED
  }
  if (size == 0) {
    size = 1; // a distinct, freeable pointer for a zero-size request
  }
  void* p = std::malloc(size);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void rt_free(void* p) noexcept {
  if (arbc::RtScope::armed()) {
    arbc::RtScope::note_allocation(); // a free is a heap op, equally forbidden on RT
  }
  std::free(p);
}

} // namespace

void* operator new(std::size_t size) { return rt_alloc(size); }
void* operator new[](std::size_t size) { return rt_alloc(size); }
void operator delete(void* p) noexcept { rt_free(p); }
void operator delete[](void* p) noexcept { rt_free(p); }
void operator delete(void* p, std::size_t) noexcept { rt_free(p); }
void operator delete[](void* p, std::size_t) noexcept { rt_free(p); }

#endif // ARBC_RT_HARDENED
