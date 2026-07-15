#pragma once

// The executor seam for the PARALLEL tile-encode save (serialize.tile_store_parallel_save
// Decision 1/3). It fans the pure per-tile encode -- `peek` -> storage-convert -> hash and
// shuffle+compress, all a pure function of one immutable `peek()`ed tile over a reentrant
// compressor -- across the runtime `WorkerPool`'s generic work lane, while keeping EVERY
// mutation on the caller's (save) thread.
//
// It is injected DOWNWARD into `codec_raster.cpp`, mirroring how `RenderDispatch` is
// injected into the `PullService`: the pure encode functions stay in `arbc::serialize`
// (L4), which gains no pool edge, and the whole fan-out lives in `arbc::runtime` (L5) --
// the one component that already sees both the pool and the raster codec. The dispatch
// runs the caller's pure `encode` on its executor and hands each result back to the
// caller's `reap` ON THE CALLING THREAD, in ascending index order. All mutation -- the
// memo, the in-save/on-disk dedup, the positional `blobs[i]`, the `AssetSink` write --
// lives in `reap`; the executor touches only the caller-owned output buffer and the
// immutable pinned document version (Constraint 2), never the tile cache.
//
// One concrete class, two executors, ONE algorithm (Decision 5). Default-constructed it is
// the INLINE executor -- `encode` then `reap`, one job at a time, in row-major order,
// byte-identical to the single-threaded save `raster_tile_store` shipped and the offline
// export default. Constructed over a `WorkerPool` it fans `encode` across that pool's
// workers, BOUNDED to O(`worker_count`) jobs in flight (Constraint 4), reaping by index.
// The two produce byte-identical output; they differ only in throughput and in transient
// scratch (O(`worker_count` * tile) vs O(tile)).

#include <arbc/serialize/tile_blob.hpp> // TileBlobError (errors are values across the lane)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace arbc {

class WorkerPool; // runtime/worker_pool.hpp

// One tile's pure encode result -- the worker's ONLY output, written into a caller-owned
// buffer and reaped on the save thread. Errors are values (Constraint 7): a
// `frame_tile_blob` failure rides back as `error`, never a throw across the worker
// boundary. `frame` is the on-disk blob (shuffle+zstd); `hash` its content name.
struct TileEncodeOutput {
  std::string hash;
  std::vector<std::byte> frame;
  std::optional<TileBlobError> error;
};

class TileEncodeDispatch {
public:
  // `encode(j)` produces job j's pure result; it runs on a worker (or inline) and must be
  // reentrant and touch no shared mutable state.
  using EncodeFn = std::function<TileEncodeOutput(std::size_t)>;
  // `reap(j, out)` consumes job j's result on the CALLING (save) thread, in ascending j
  // order, and returns `true` to continue or `false` to abort. On abort the dispatch
  // drains every outstanding job before `run` returns, so no worker outlives a reference
  // to a freed buffer (Constraint 7).
  using ReapFn = std::function<bool(std::size_t, TileEncodeOutput&)>;

  // The INLINE executor (offline export default; byte-identical to the serial save).
  // Out-of-line (defined in the .cpp) so the `std::vector<Slot>` member is only ever
  // constructed where `Slot` is a complete type.
  TileEncodeDispatch();

  // The WORKER-BACKED executor: fans `encode` across `pool`'s generic work lane, bounded
  // to O(`pool.worker_count()`) jobs in flight. `owner` is the opaque submitter tag the
  // pool drains this dispatch's outstanding jobs by (`WorkerPool::drain_owner`), exactly
  // as a render driver passes its own `this`. `pool` must outlive the dispatch.
  TileEncodeDispatch(WorkerPool& pool, const void* owner);

  // Out-of-line so the `std::vector<Slot>` member's teardown is emitted where `Slot` is
  // complete (it is forward-declared here, defined in the .cpp).
  ~TileEncodeDispatch();

  TileEncodeDispatch(const TileEncodeDispatch&) = delete;
  TileEncodeDispatch& operator=(const TileEncodeDispatch&) = delete;

  // Encode jobs `[0, count)` on this dispatch's executor and reap each on the calling
  // thread in ascending index order (see `EncodeFn`/`ReapFn`).
  void run(std::size_t count, const EncodeFn& encode, const ReapFn& reap);

  // The bounded-in-flight behavioral counter (Constraint 4; claim
  // `08-serialization#raster-parallel-save-encode-is-bounded`): the peak number of
  // simultaneously-outstanding encode jobs observed across every `run` call. Never
  // exceeds `window()` -- which is O(`worker_count`), and exactly 1 for the inline
  // executor. Never a wall-clock, never a tile count.
  std::uint64_t peak_in_flight() const noexcept { return d_peak; }

  // The window bound: the most jobs this dispatch will hold outstanding at once. 1 inline;
  // a small multiple of `worker_count` worker-backed.
  std::size_t window() const noexcept { return d_window; }

private:
  struct Slot; // run-scoped per-job staging (completion + output), worker-backed only

  WorkerPool* d_pool{nullptr}; // null => the inline executor
  const void* d_owner{nullptr};
  std::size_t d_window{1};
  std::uint64_t d_peak{0};
  std::vector<Slot> d_slots; // sized once per `run`; captured by index, never resized mid-run
};

} // namespace arbc
