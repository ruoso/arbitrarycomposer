#pragma once

// RT-safety enforcement primitives (audio.rt_safety, docs 12/16).
//
// Two portable pieces sit at level 0 (doc 17:50-61, Decision D3) so every
// consumer on the device RT callback chain -- the L4 `LookaheadRing::drain`, the
// L5 `DeviceMonitor::fill_rt` / `LookaheadPump::drain`, the `media`
// `StreamingResampler` feed/produce, and the out-of-lib miniaudio trampoline --
// can name them without a new levelization edge:
//
//   * ARBC_RT_NONBLOCKING -- the portable spelling of Clang's
//     `[[clang::nonblocking]]` attribute (doc 16:70-73). On Clang it expands to
//     the attribute, so RealtimeSanitizer (`-fsanitize=realtime`, the `rtsan`
//     preset) flags any lock / allocation / blocking syscall in the annotated
//     call graph at runtime (Layer A, D1). On GCC/MSVC it expands to nothing, so
//     the `-Wall -Wextra -Wpedantic -Werror` matrix still builds; those lanes are
//     guarded by Layer B (`RtScope`) and the grep-lint (D1/D5).
//
//   * RtScope -- a compiler-independent debug guard (Layer B, doc 16:70-73,103).
//     Arming it on the RT thread for the duration of a callback makes the hooked
//     global `operator new` / `operator delete` (rt_safety.cpp) treat any
//     allocation as a forbidden RT operation: it is counted, and in the
//     debug-hardened build (ARBC_RT_HARDENED) it aborts build-failingly. The
//     counters back the structural upgrade of device_monitor's callback-purity
//     counter (device_monitor.md:311-312) -- "not a convention but a
//     build-failing check".

#if defined(__clang__)
#define ARBC_RT_NONBLOCKING [[clang::nonblocking]]
#else
#define ARBC_RT_NONBLOCKING
#endif

#include <cstdint>

namespace arbc {

// A scoped "RT active" flag for the calling thread. It is thread-local, so
// arming the device callback thread never perturbs the pump / worker threads
// that legitimately allocate (Constraint 5). Nestable: the flag stays armed
// until the outermost scope leaves.
class RtScope {
public:
  RtScope() noexcept;
  ~RtScope() noexcept;

  RtScope(const RtScope&) = delete;
  RtScope& operator=(const RtScope&) = delete;

  // True while any `RtScope` is live on the calling thread.
  static bool armed() noexcept;

  // Forbidden-operation counters observed while armed on the calling thread
  // (thread-local, monotonic until `reset_counts`). `allocations` is driven by
  // the hooked global `operator new` / `operator delete`; `locks` / `refcounts`
  // are driven by explicit notes at any instrumented blocking / refcount site --
  // there are none on the shipped, now-lock-free chain (Decision D2), so they
  // read zero, with RealtimeSanitizer the primary structural catch (D1).
  static std::uint64_t allocations() noexcept;
  static std::uint64_t locks() noexcept;
  static std::uint64_t refcounts() noexcept;
  static void reset_counts() noexcept;

  // Record a forbidden operation seen while armed. `note_allocation` is called by
  // the global `operator new` / `operator delete` override; the others are
  // available for a pool refcount / lock debug-assert. Each aborts build-failingly
  // under ARBC_RT_HARDENED (doc 16:72-73).
  static void note_allocation() noexcept;
  static void note_lock() noexcept;
  static void note_refcount() noexcept;
};

} // namespace arbc
