#include <arbc/runtime/tile_encode_dispatch.hpp>
#include <arbc/runtime/worker_pool.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace arbc {

// Per-job staging, sized once per `run` and captured BY INDEX into the submitted job
// (never resized mid-run, so `&d_slots[j]` stays valid while a worker writes it). The
// completion is the generic lane's caller-owned settle handle; the output is the worker's
// only write target.
struct TileEncodeDispatch::Slot {
  std::shared_ptr<WorkCompletion> done;
  TileEncodeOutput out;
};

TileEncodeDispatch::TileEncodeDispatch() = default;

TileEncodeDispatch::TileEncodeDispatch(WorkerPool& pool, const void* owner)
    : d_pool(&pool), d_owner(owner) {
  // The window is O(worker_count): at most this many encode jobs are outstanding at once,
  // so a save's transient scratch is O(worker_count * tile), independent of image size,
  // and an autosave cannot flood the shared render pool with an image's worth of queued
  // jobs (Constraint 4). A small multiple keeps workers fed while the save thread reaps.
  // Clamped to >= 1 so a worker-backed dispatch over a zero-worker pool (which runs jobs
  // inline through `submit_work`) still makes progress.
  d_window = std::max<std::size_t>(1, 2 * pool.worker_count());
}

TileEncodeDispatch::~TileEncodeDispatch() = default;

void TileEncodeDispatch::run(std::size_t count, const EncodeFn& encode, const ReapFn& reap) {
  if (d_pool == nullptr) {
    // INLINE executor: encode then reap, one job at a time, in row-major order --
    // byte-identical to the single-threaded save (Constraint 5). Peak in flight is 1.
    for (std::size_t j = 0; j < count; ++j) {
      TileEncodeOutput out = encode(j);
      d_peak = std::max<std::uint64_t>(d_peak, 1);
      if (!reap(j, out)) {
        return;
      }
    }
    return;
  }

  // WORKER-BACKED executor: a windowed submit/reap over the pool's generic work lane. At
  // most `d_window` jobs are outstanding (dispatched-but-not-reaped) at once; results land
  // in completion order, but we reap strictly BY INDEX so the save thread's dedup/store
  // runs in the same row-major order the serial path does (Decision 2/3).
  d_slots.assign(count, Slot{});
  CompletionCursor cursor{d_pool->settle_generation()};
  std::size_t dispatched = 0;
  std::size_t reaped = 0;
  bool aborted = false;

  while (reaped < count) {
    // Fill the window with fresh jobs.
    while (dispatched < count && (dispatched - reaped) < d_window) {
      const std::size_t j = dispatched;
      d_slots[j].done = std::make_shared<WorkCompletion>();
      // The job's ONLY write is its own slot's output buffer; `encode` is pure and
      // reentrant, and outlives the job (it is a `run` parameter). No shared mutable
      // state, so no lock on the hot path (Constraint 2).
      d_pool->submit_work(WorkTask{[this, j, &encode] { d_slots[j].out = encode(j); },
                                   d_slots[j].done, d_owner});
      ++dispatched;
      d_peak = std::max<std::uint64_t>(d_peak, dispatched - reaped);
    }

    // Reap the next job in index order, parking on the settle condition (never a fixed
    // sleep) until precisely THAT slot has settled. A sibling's settle may wake us
    // spuriously; we re-test our own slot and re-park.
    while (!d_slots[reaped].done->settled()) {
      d_pool->wait_completions(cursor, std::nullopt);
    }
    TileEncodeOutput out = std::move(d_slots[reaped].out);
    d_slots[reaped].done.reset(); // free the completion (and let the frame buffer die)
    const bool cont = reap(reaped, out);
    ++reaped;
    if (!cont) {
      aborted = true;
      break;
    }
  }

  if (aborted) {
    // A save that fails mid-fan-out drains its outstanding jobs before returning, so no
    // worker outlives a reference to a `d_slots` buffer we are about to free (Constraint
    // 7). `drain_owner` purges our not-yet-started jobs and waits out the started ones,
    // touching only this dispatch's owner tag -- a sibling renderer's work keeps running.
    d_pool->drain_owner(d_owner);
  }
  d_slots.clear();
}

} // namespace arbc
