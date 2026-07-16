#include <arbc/runtime/tile_decode_dispatch.hpp>
#include <arbc/runtime/worker_pool.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

namespace arbc {

// Per-job staging, sized once per pass and captured BY INDEX into the submitted job (never
// resized mid-pass, so `&d_slots[j]` stays valid while a worker writes it). `in` is the
// job-owned frame the loading thread staged before submit; `done` is the generic lane's
// caller-owned settle handle (null for a declined or inline-run job); `out` is the worker's
// only write target; `verdict` is the loading-thread fetch disposition surfaced at reap.
struct TileDecodeDispatch::Slot {
  std::shared_ptr<WorkCompletion> done;
  TileDecodeInput in;
  TileDecodeOutput out;
  TileFetch verdict{TileFetch::Ready};
  bool submitted{false}; // a worker decode job is outstanding for this slot
};

TileDecodeDispatch::TileDecodeDispatch() = default;

TileDecodeDispatch::TileDecodeDispatch(WorkerPool& pool, const void* owner)
    : d_pool(&pool), d_owner(owner) {
  // The window is O(worker_count): at most this many decode jobs are outstanding at once,
  // and the fetch look-ahead runs at most this many indices ahead of the reap cursor, so a
  // load's transient scratch is O(worker_count * tile) independent of image size, and
  // opening a document cannot flood the shared render pool with an image's worth of queued
  // jobs (Constraint 4). A small multiple keeps workers fed while the loading thread reaps
  // and writes into the pool. Clamped to >= 1 so a worker-backed dispatch over a zero-worker
  // pool (which runs jobs inline through `submit_work`) still makes progress.
  d_window = std::max<std::size_t>(1, 2 * pool.worker_count());
}

TileDecodeDispatch::~TileDecodeDispatch() = default;

void TileDecodeDispatch::begin(std::size_t count, FetchFn fetch, DecodeFn decode) {
  d_slots.assign(count, Slot{});
  d_fetch = std::move(fetch);
  d_decode = std::move(decode);
  d_count = count;
  d_dispatched = 0;
  d_reaped = 0;
  d_in_flight = 0;
  d_cursor_gen = (d_pool != nullptr) ? d_pool->settle_generation() : 0;
  d_open = true;
}

void TileDecodeDispatch::pump_ahead() {
  // Fetch + submit fresh jobs until the window is full (or the input is exhausted). The
  // window bounds how far the fetch runs AHEAD of the reap cursor, so a run of declined
  // entries cannot fetch the whole array either. `fetch` runs on THIS (loading) thread and
  // decides each job's verdict; only a `Ready` verdict stages an input and submits a decode.
  while (d_dispatched < d_count && (d_dispatched - d_reaped) < d_window) {
    const std::size_t j = d_dispatched;
    Slot& slot = d_slots[j];
    slot.verdict = d_fetch(j, slot.in);
    if (slot.verdict != TileFetch::Ready) {
      ++d_dispatched; // a declined entry: no job, resolved on the loading thread at reap
      continue;
    }
    if (d_pool == nullptr) {
      // INLINE executor: decode now, on this thread -- byte-identical to the serial load
      // (Constraint 5). Peak in flight is 1.
      slot.out = d_decode(slot.in);
      d_peak = std::max<std::uint64_t>(d_peak, 1);
    } else {
      // WORKER-BACKED executor: the job's ONLY write is its own slot's output; `decode` is
      // pure and reentrant over the job-owned `in`, and outlives the job (a pass member). No
      // shared mutable state, so no lock on the hot path (Constraint 2).
      slot.done = std::make_shared<WorkCompletion>();
      slot.submitted = true;
      d_pool->submit_work(
          WorkTask{[this, j] { d_slots[j].out = d_decode(d_slots[j].in); }, slot.done, d_owner});
      ++d_in_flight;
      d_peak = std::max<std::uint64_t>(d_peak, d_in_flight);
    }
    ++d_dispatched;
  }
}

TileDecodeReap TileDecodeDispatch::reap(std::size_t index) {
  pump_ahead();
  Slot& slot = d_slots[index];
  if (slot.submitted) {
    // Park on the settle condition (never a fixed sleep) until precisely THIS slot has
    // settled. A sibling's settle may wake us spuriously; we re-test our own slot and re-park.
    while (!slot.done->settled()) {
      CompletionCursor cursor{d_cursor_gen};
      d_pool->wait_completions(cursor, std::nullopt);
      d_cursor_gen = cursor.drained_gen;
    }
    slot.done.reset(); // free the completion
    --d_in_flight;
  }
  TileDecodeReap out;
  out.verdict = slot.verdict;
  out.output = std::move(slot.out);
  slot.in = TileDecodeInput{}; // free the job-owned frame the moment its tile is reaped
  ++d_reaped;
  return out;
}

void TileDecodeDispatch::finish() {
  if (!d_open) {
    return;
  }
  if (d_pool != nullptr && d_in_flight > 0) {
    // A load that aborts mid-fan-out drains its outstanding jobs before returning, so no
    // worker outlives a reference to a `d_slots` frame we are about to free (Constraint 7).
    // `drain_owner` purges our not-yet-started jobs and waits out the started ones, touching
    // only this dispatch's owner tag -- a sibling renderer's work keeps running.
    d_pool->drain_owner(d_owner);
    d_in_flight = 0;
  }
  d_slots.clear();
  d_fetch = nullptr;
  d_decode = nullptr;
  d_open = false;
}

} // namespace arbc
