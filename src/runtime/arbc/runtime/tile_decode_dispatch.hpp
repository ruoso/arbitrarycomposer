#pragma once

// The executor seam for the PARALLEL tile-decode load (serialize.tile_store_parallel_load
// Decision 1/2). It fans the pure per-tile decode -- decompress -> unshuffle -> verify-hash,
// all a pure function of one fetched immutable frame over a reentrant decompressor -- across
// the runtime `WorkerPool`'s generic work lane, while keeping EVERY fetch and EVERY pool
// write on the loading thread.
//
// It is the structural twin of `TileEncodeDispatch`, injected DOWNWARD into
// `codec_raster.cpp`: the pure `decode_tile_blob` stays in `arbc::serialize` (L4), which
// gains no pool edge, and the whole fan-out lives in `arbc::runtime` (L5). One concrete
// class, two executors, ONE algorithm (Decision 1). Default-constructed it is the INLINE
// executor -- fetch, decode, reap, one tile at a time in row-major order, byte-identical to
// the single-threaded load `kinds.raster_tilewise_load` shipped (Constraint 5) and the
// offline default. Constructed over a `WorkerPool` it fans `decode` across that pool's
// workers, BOUNDED to O(`worker_count`) jobs in flight (Constraint 4), reaping by index.
// The two produce byte-identical output; they differ only in throughput and in transient
// scratch (O(`worker_count` * tile) vs O(tile)).
//
// WHERE THE LOAD DIFFERS FROM THE SAVE. On the save side `TileEncodeDispatch::run` owns the
// whole loop, because `peek` reads pool slots that already exist. On the load side the pool
// slots do NOT exist yet: `RasterStore::build_from_tiles` allocates one blob at a time and
// hands its memory to the codec's `fill(t, dst)` in ascending row-major order. That fill IS
// the outer loop, and it cannot be replaced (kind_raster owns the allocation, Constraint 3).
// So this dispatch is a STEPPED pump the fill drives: `begin` opens a pass, each `reap(t)`
// first tops up the look-ahead window -- `fetch`ing and submitting jobs up to the window
// ahead on the CALLING (loading) thread -- then blocks until job `t` has decoded and hands
// its result back; `finish` drains. The fetch (`LoadContext::resolve` + `AssetSource` read)
// stays on the loading thread because both are single-writer/non-atomic (Decision 2); only
// the pure decode fans out. Every mutation -- the fetch, the `std::copy_n` into the pool
// blob, the `build_from_tiles` allocation, the memo seed -- lives on the loading thread; a
// worker touches only its own job-owned frame input and its own output buffer (Constraint 2).

#include <arbc/serialize/tile_blob.hpp> // TileBlobError (errors are values across the lane)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace arbc {

class WorkerPool; // runtime/worker_pool.hpp

// One tile's job-owned decode INPUT, staged on the loading thread by `fetch` and moved into
// the job. The worker reads only this (plus the pass-constant storage format / sample count
// the `DecodeFn` closes over), so a decode job shares no mutable state (Constraint 2).
// `frame` is the on-disk blob bytes; `hash` the content name they were fetched under, which
// `decode_tile_blob` re-derives and compares (self-verifying, doc 08 Principle 8).
struct TileDecodeInput {
  std::string hash;
  std::vector<std::byte> frame;
};

// One tile's pure decode RESULT -- the worker's ONLY output, reaped on the loading thread.
// Errors are values (Constraint 6): a `decode_tile_blob` hash/frame failure rides back as
// `error`, never a throw across the worker boundary. `pixels` is the tile's working RGBA32F.
struct TileDecodeOutput {
  std::vector<float> pixels;
  std::optional<TileBlobError> error;
};

// The disposition a `fetch` reports on the loading thread, BEFORE any decode job is
// submitted (Constraint 6). `Ready` stages `in` and dispatches the pure decode; the two
// failure verdicts are decided entirely on the loading thread -- a malformed `blobs` entry
// (non-string / bad hash) and an unresolvable / empty frame -- and submit NO job, so no
// worker is ever handed an unchecked input. `reap` returns the same verdict in index order,
// so the codec maps it to exactly the `ReaderError` the serial path returned.
enum class TileFetch { Ready, Malformed, Unresolvable };

// The result of reaping one job: its fetch verdict and (when `Ready`) its decode output.
struct TileDecodeReap {
  TileFetch verdict{TileFetch::Ready};
  TileDecodeOutput output;
};

class TileDecodeDispatch {
public:
  // `fetch(j, in)` stages job j's frame on the CALLING (loading) thread and returns its
  // verdict; on `Ready` it has filled `in` (hash + frame bytes). It runs strictly within
  // the look-ahead window, never on a worker.
  using FetchFn = std::function<TileFetch(std::size_t, TileDecodeInput&)>;
  // `decode(in)` produces the pure decode of one staged frame; it runs on a worker (or
  // inline) and must be reentrant and touch no shared mutable state.
  using DecodeFn = std::function<TileDecodeOutput(const TileDecodeInput&)>;

  // The INLINE executor (offline default; byte-identical to the serial load). Out-of-line
  // (defined in the .cpp) so the `std::vector<Slot>` member is only ever constructed where
  // `Slot` is a complete type.
  TileDecodeDispatch();

  // The WORKER-BACKED executor: fans `decode` across `pool`'s generic work lane, bounded to
  // O(`pool.worker_count()`) jobs in flight. `owner` is the opaque submitter tag the pool
  // drains this dispatch's outstanding jobs by (`WorkerPool::drain_owner`), exactly as a
  // render driver passes its own `this`. `pool` must outlive the dispatch.
  TileDecodeDispatch(WorkerPool& pool, const void* owner);

  // Out-of-line so the `std::vector<Slot>` member's teardown is emitted where `Slot` is
  // complete (it is forward-declared here, defined in the .cpp).
  ~TileDecodeDispatch();

  TileDecodeDispatch(const TileDecodeDispatch&) = delete;
  TileDecodeDispatch& operator=(const TileDecodeDispatch&) = delete;

  // Open a pass over `count` jobs fed by `fetch` (loading thread) and computed by `decode`
  // (worker/inline). Exactly one open pass per object; `begin` resets the per-pass state.
  void begin(std::size_t count, FetchFn fetch, DecodeFn decode);

  // Reap job `index` in ASCENDING order: first top up the look-ahead window (fetch + submit
  // jobs up to `window()` ahead on the CALLING thread), then block until job `index` has
  // settled and hand back its verdict and output (moved out). Parks on the pool's settle
  // condition, never a fixed sleep; a sibling's settle may wake it spuriously and it
  // re-tests its own slot.
  TileDecodeReap reap(std::size_t index);

  // Drain every outstanding decode job and close the pass. On an abort (the caller stopped
  // reaping because a `fill` declined), `drain_owner` purges this dispatch's not-yet-started
  // jobs and waits out the started ones before the job-owned frames are freed (Constraint
  // 7), touching only this dispatch's owner tag. Idempotent.
  void finish();

  // The bounded-in-flight behavioral counter (Constraint 4; claim
  // `08-serialization#raster-parallel-load-decode-is-bounded`): the peak number of
  // simultaneously-outstanding decode jobs observed across every pass. Never exceeds
  // `window()` -- O(`worker_count`), exactly 1 for the inline executor. Never a wall-clock,
  // never a tile count.
  std::uint64_t peak_in_flight() const noexcept { return d_peak; }

  // The window bound: the most decode jobs this dispatch holds outstanding at once, and the
  // most indices it fetches ahead of the reap cursor. 1 inline; a small multiple of
  // `worker_count` worker-backed.
  std::size_t window() const noexcept { return d_window; }

private:
  struct Slot; // per-job staging (input + completion + output + verdict)

  // Advance the fetch/submit cursor so jobs are outstanding up to the window ahead of the
  // reap cursor. Runs on the calling thread.
  void pump_ahead();

  WorkerPool* d_pool{nullptr}; // null => the inline executor
  const void* d_owner{nullptr};
  std::size_t d_window{1};
  std::uint64_t d_peak{0};

  // --- per-pass state (reset by `begin`) ---
  std::vector<Slot> d_slots; // sized once per pass; captured by index, never resized mid-pass
  FetchFn d_fetch;
  DecodeFn d_decode;
  std::size_t d_count{0};
  std::size_t d_dispatched{0}; // next index to fetch (both `Ready` and declined advance it)
  std::size_t d_reaped{0};     // next index to reap
  std::size_t d_in_flight{0};  // submitted-but-not-reaped worker decode jobs
  std::uint64_t d_cursor_gen{0}; // this pass's `CompletionCursor` state (worker-backed park)
  bool d_open{false};
};

} // namespace arbc
