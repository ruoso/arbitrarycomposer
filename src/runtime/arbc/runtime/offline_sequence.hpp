#pragma once

#include <arbc/base/expected.hpp>          // expected
#include <arbc/base/rational_time.hpp>      // Rational
#include <arbc/base/time.hpp>               // Time, TimeRange
#include <arbc/compositor/compositor.hpp>   // Viewport, Backend, SurfacePool, Surface, ContentResolver
#include <arbc/compositor/counters.hpp>     // CompositorCounters, TileCache
#include <arbc/runtime/document.hpp>        // Document, DocStatePtr, DocRoot
#include <arbc/runtime/worker_pool.hpp>     // WorkerPool, WorkerPoolConfig
#include <arbc/surface/surface_error.hpp>   // SurfaceError

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

// The offline/export render driver for `arbc::runtime` (L5, doc 17:60,78-86):
// doc 02:73-85 ("The frame, offline") + doc 11:209-213 (the sequence loop) made
// concrete. It is the second render driver over the same compositor (doc
// 02:40-41) -- the offline counterpart of `InteractiveRenderer` -- but where the
// interactive driver owns deadlines, damage, and progressive refinement, this one
// inverts every one of those: NO deadline, NO degradation, NO transport, exact
// only (doc 02:73-85). It is a CLASS (not a free function like the one-shot
// `render_offline`) for the same reason `InteractiveRenderer` is: it carries state
// ACROSS frames -- here the single pinned revision, the cross-frame `SurfacePool`
// + `TileCache`, the persistent counters, and (opt-in) the worker pool.
//
// The one genuinely new guarantee over a trivial loop of `render_offline` is
// SEQUENCE-WIDE revision consistency (doc 02:77-80, doc 11:211-213): the instance
// pins ONE `DocStatePtr` at construction and renders EVERY frame against that one
// `DocRoot::revision()`, so an export is internally consistent even while the host
// keeps editing on the writer thread -- "frame N must not see frame N+1's edits".
// The one-shot `render_offline` (still-image exact render of the CURRENT version)
// is unchanged and stays beside this.

namespace arbc {

// The default cross-frame tile-cache byte budget for an export. A sequence reuses
// one `TileCache` so a mostly-still export does not re-rasterize every layer every
// frame (doc 02:82-85); a generous default keeps a fixed-resolution export's
// working set resident. A caller sizes it explicitly for a large export.
inline constexpr std::size_t k_default_sequence_cache_budget = 256u * 1024 * 1024;

// The exact integer-flick output frame instants over the half-open `range` at a
// fixed `output_rate` (frames per second, an exact rational -- doc 11:38-43: "frame
// rate is a property of the render, not the model"). Frame k lands at
// `range.start + k * (flicks_per_second / output_rate)`, computed in exact rational
// arithmetic with one ties-to-even leaf rounding (reusing `TimeMap::evaluate`, the
// same stepping the transport uses), so the series is deterministic and
// byte-reproducible. Half-open: the first instant is `range.start`, and instants
// are emitted while strictly inside `range` (the endpoint `range.end` is excluded).
// An empty range, a non-positive rate, or a rate whose period overflows the flick
// width yields an empty series (faults-as-values, never an abort). The host may
// compute the series any other way and feed `render_frame_at`/`render_sequence`
// raw instants directly -- this is only the common fixed-rate convenience.
std::vector<Time> frame_times_over(const TimeRange& range, const Rational& output_rate);

// The stateful offline sequence renderer (Decision 1). Non-copyable and
// non-movable: it pins a document revision and owns a `SurfacePool`, a `TileCache`,
// and a `WorkerPool` (which owns threads when parallel).
class SequenceRenderer {
public:
  // What the caller's per-frame sink receives: the frame instant and the owned
  // frame surface (or the capability-honest `SurfaceError` if the pinned working
  // space is unstorable). The host keeps, encodes, or muxes the surface -- the
  // engine writes no files and links no codec (doc 12:157, doc 02:6-14).
  using FrameSink =
      std::function<void(Time, expected<std::unique_ptr<Surface>, SurfaceError>)>;

  // Construction PINS THE EXPORT (Decision 2): one `DocStatePtr` for the whole
  // sequence, so every frame reads one frozen `DocRoot::revision()`. `viewport` is
  // taken by value (per-frame value state, like the interactive camera). A
  // `worker_count == 0` pool config (the default) selects inline exact rendering --
  // the byte-deterministic path; a non-zero count opts into parallel exact renders
  // (Decision 4), a correctness-neutral speed knob.
  SequenceRenderer(const Document& document, Viewport viewport, Backend& backend,
                   WorkerPoolConfig pool_config = {},
                   std::size_t cache_budget_bytes = k_default_sequence_cache_budget);

  SequenceRenderer(const SequenceRenderer&) = delete;
  SequenceRenderer& operator=(const SequenceRenderer&) = delete;
  SequenceRenderer(SequenceRenderer&&) = delete;
  SequenceRenderer& operator=(SequenceRenderer&&) = delete;

  // Render one exact frame at `composition_time` against the pinned revision
  // (Decision 3). Allocates a fresh frame target at the pinned version's
  // `working_space()` (capability-honest via the `SurfaceError` value path exactly
  // as `render_offline`), then drives the compositor in EXACT, NO-DEGRADE mode:
  // every visible cache miss is rendered to completion with NO deadline (inline, or
  // fanned out to the worker pool and reaped via `wait_completions(nullopt)`), and
  // only fresh, exact-scale, current-revision tiles are composited -- never a
  // stale, coarser, or placeholder tile, no matter how long a render takes (doc
  // 02:73-85). The per-layer span-cull + time-map + `quantize_time` walk the
  // compositor already runs is driven at `composition_time`. Returns one owned
  // `Surface`.
  expected<std::unique_ptr<Surface>, SurfaceError> render_frame_at(Time composition_time);

  // Render each supplied instant and hand each frame to `sink` (doc 11:209-213). A
  // thin convenience loop over `render_frame_at`; the HOST owns the frame-time
  // series (`frame_times_over` computes the common fixed-rate case) and owns
  // encoding/muxing. No `Transport` is required -- each `Time` feeds
  // `composition_time` directly (the seam that already accepts a raw instant).
  void render_sequence(std::span<const Time> frame_times, const FrameSink& sink);

  // The pinned version -- the single frozen `DocRoot` every frame renders against.
  const DocRoot& pinned_state() const noexcept { return *d_pinned; }
  // The one revision the whole sequence is consistent at (the pin held).
  std::uint64_t revision() const noexcept { return d_pinned->revision(); }

  // The persistent behavioral counters accumulated across the sequence (doc
  // 16:54-62). `degraded_composites()` reads zero for a well-formed export -- the
  // no-degrade proof.
  const CompositorCounters& counters() const noexcept { return d_counters; }
  // The cross-frame tile cache (read-only, for hit/miss/eviction observability).
  const TileCache& cache() const noexcept { return d_cache; }
  // The parallel-exact worker pool (inline by default). Exposed for observability
  // and for a settler to `poke()` a reaped render thread.
  WorkerPool& worker_pool() noexcept { return d_pool; }

private:
  const Document& d_document;
  Viewport d_viewport;
  Backend& d_backend;
  // The single frozen revision pinned for the WHOLE export (Decision 2): every
  // frame reads `*d_pinned`, so a writer thread committing new revisions during the
  // export changes no exported frame.
  DocStatePtr d_pinned;
  // Cross-frame state (Decision 1): the pool reuses temps across frames, the cache
  // reuses tiles so a mostly-still export does not re-rasterize every frame (doc
  // 02:82-85), and the counters accumulate the whole sequence.
  SurfacePool d_surfaces;
  TileCache d_cache;
  CompositorCounters d_counters;
  // Cached `worker_count != 0`: selects the inline single-pass path vs the
  // block-and-fan-out reap loop in `render_frame_at`. Declared before `d_pool` so
  // it reads `pool_config` before the pool moves it (init runs in declaration
  // order).
  bool d_parallel;
  // Inline (`worker_count == 0`, default) or the parallel-exact fan-out substrate.
  WorkerPool d_pool;
};

} // namespace arbc
