#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/pool/workspace_file.hpp> // WorkspaceFileError (the checkpoint error value)

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

namespace arbc {

// Housekeeping policy for arbc::runtime (doc 17: L5). The pool layer (L1) ships
// the reclamation drain (`ReclamationQueue::drain`) and the checkpoint commit
// (`Checkpointer::commit`) as bare MECHANISMS and deliberately defers "when to
// run them" here (reclamation.hpp:47-48, checkpoint.hpp:32-33). This is that
// policy: a small, passive object that decides WHEN to drain and WHEN to
// checkpoint, and that aggregates the mechanisms' existing counters into one
// wall-clock-free "memory panel" snapshot (doc 15:149-151, doc 14:199-201).
//
// It owns no thread and reads no clock. The contract the CALLER must honor:
// exactly one thread calls a draining entry point at a time. RT threads only
// enqueue (they never touch a Housekeeper). `HousekeepingThread` is the owned
// background loop that enforces the single-drainer discipline by construction.

// WHAT a Housekeeper drives. `runtime.housekeeping` validated the policy against
// bare pool primitives -- a `(ReclamationQueue&, Checkpointer*, Arena*)` triple --
// and that shape does not survive contact with a live `Document`, for two reasons
// (`runtime.housekeeping_document_wiring` Decision 1):
//
//  1. Draining a `Model`'s queue DIRECTLY is a silent, total leak. `~HamtNode`
//     reaches its stores through a thread-local `ReclaimContext` and returns
//     WITHOUT releasing its child edges when none is published (hamt.hpp:103-109).
//     `Model::drain()` publishes it; a bare `queue.drain()` does not -- and there
//     is no crash and no failing assertion to say so. So "drain" must be a call
//     the TARGET implements, in the one place that already discharges that duty.
//  2. `Model::checkpoint()` resolves its own current root (model.cpp:730-739), so
//     a `SlotIndex` tip threaded through `after_commit` is duplicated work for the
//     only target that matters. The target owns the root question.
//
// Hence this seam. `Housekeeper` names no pool type and holds no arena: it decides
// WHEN, the target knows WHERE. Two implementations ship in
// `arbc/runtime/housekeeping_targets.hpp`: `ModelHousekeepingTarget` (a live
// `Document`'s `Model`) and `PoolHousekeepingTarget` (the bare pool triple).
class HousekeepingTarget {
public:
  virtual ~HousekeepingTarget() = default;

  HousekeepingTarget() = default;
  HousekeepingTarget(const HousekeepingTarget&) = delete;
  HousekeepingTarget& operator=(const HousekeepingTarget&) = delete;

  // Drain deferred reclamation to quiescence. The implementation publishes
  // whatever per-thread context its destructors need (a `Model` publishes the
  // HAMT `ReclaimContext`). Called on whichever thread holds the drain seat.
  virtual void drain() = 0;

  // Commit a durable checkpoint at the target's current published root.
  // WRITER-THREAD ONLY (doc 15 § Version reclamation: the drainer is not the
  // writer, and the checkpointer is). Never reached when `checkpointable()`.
  virtual expected<std::monostate, WorkspaceFileError> checkpoint() = 0;

  // False for an anonymous, live-only arena: every checkpoint trigger is inert.
  virtual bool checkpointable() const noexcept = 0;

  // The memory-panel numbers (doc 15:164-169), read for `HousekeepingStats`.
  virtual std::size_t live_slots() const noexcept = 0;
  virtual std::size_t bytes_reserved() const noexcept = 0;
  virtual std::uint64_t slots_freed_to_list() const noexcept = 0;
  virtual std::uint32_t durable_epoch() const noexcept = 0;
};

// The cadence knobs, all defaulted. An absent optional disables that checkpoint
// trigger; a non-checkpointable target ignores both. These realize doc 15:213-214's
// three triggers -- transaction count, timer, explicit host call -- as declarative
// policy.
struct HousekeepingConfig {
  // Drain the reclamation queue to quiescence in `after_commit` (doc 15:129-136,
  // the writer-between-transactions drain site). When false, released slots stay
  // on the queue until an explicit `drain_and_quiesce()` (or a `tick`).
  bool drain_between_transactions = true;
  // Commit a checkpoint every Nth transaction (trigger (a)). Absent disables it.
  // Fires from `after_commit`, i.e. ON THE WRITER THREAD -- which is the only
  // thread a commit may ever run on (doc 15; `runtime.housekeeping_document_wiring`
  // Decision 2).
  std::optional<std::uint64_t> checkpoint_every_n_transactions;
  // Commit a checkpoint when a handed tick has advanced this far past the last
  // checkpoint AND at least one transaction has occurred since (trigger (b)).
  // Absent disables it.
  //
  // DANGEROUS ON A LIVE DOCUMENT: this trigger fires from `tick()`, which a
  // `HousekeepingThread` drives on its BACKGROUND thread, and `Checkpointer::commit`
  // racing a writer transaction is memory-unsafe in four distinct ways (see
  // `runtime.housekeeping_document_wiring` Constraint 3). `Document` therefore never
  // sets it -- `DocumentHousekeepingConfig` does not even expose it -- and enabling
  // it off the writer thread waits on `runtime.background_checkpoint_quiesce`. It
  // stays here because a single-threaded host driving `tick()` itself is safe.
  std::optional<std::uint64_t> checkpoint_tick_interval;
};

// The wall-clock-free observability snapshot a host memory panel reads
// (doc 15:149-151, :164-169). Aggregates the Housekeeper's own event counters with
// the target's live-slot / reserved-byte accounting and its pass-through
// Checkpointer counters -- no new counting mechanism, just a composed view of
// numbers the mechanisms publish.
struct HousekeepingStats {
  std::uint64_t transactions_seen{0};         // total after_commit calls
  std::uint64_t drains_run{0};                // reclamation drains issued
  std::uint64_t checkpoints_committed{0};     // commits issued (auto + explicit)
  std::uint64_t checkpoints_skipped_clean{0}; // auto triggers gated by dirtiness
  std::size_t live_slots{0};                  // target's live slot count
  std::size_t bytes_reserved{0};              // target's reserved arena bytes
  std::uint64_t slots_freed_to_list{0};       // Checkpointer pass-through, 0 if unbound
  std::uint32_t durable_epoch{0};             // Checkpointer pass-through, 0 if unbound
};

class Housekeeper {
public:
  // `target` must outlive the Housekeeper.
  Housekeeper(HousekeepingTarget& target, HousekeepingConfig config) noexcept;

  Housekeeper(const Housekeeper&) = delete;
  Housekeeper& operator=(const Housekeeper&) = delete;

  // The between-transactions drain site (doc 15:129-136). Call after each
  // published transaction: drains the target to quiescence (when configured),
  // bumps the transaction counters, and -- if the transaction-count trigger has
  // reached its threshold -- commits a checkpoint at the target's current root.
  // A commit I/O failure surfaces as a value. WRITER-THREAD ONLY (it can commit).
  expected<std::monostate, WorkspaceFileError> after_commit();

  // The timer / low-priority-thread entry. Drains the target, then, if the
  // tick-interval trigger has elapsed AND at least one transaction has occurred
  // since the last checkpoint, commits. `monotonic_tick` is a HANDED value, never
  // a wall-clock read (doc 16; the caller owns the clock). A caller that drives
  // this from a thread other than the writer MUST leave `checkpoint_tick_interval`
  // empty -- see the knob's comment.
  expected<std::monostate, WorkspaceFileError> tick(std::uint64_t monotonic_tick);

  // The explicit host-call trigger (autosave / export / quit). Commits
  // unconditionally -- `Checkpointer::commit` still skips the data msync on a
  // genuinely clean scene, so a redundant request is cheap. WRITER-THREAD ONLY.
  expected<std::monostate, WorkspaceFileError> request_checkpoint();

  // Teardown / bulk-release path (doc 15:144-147): drain to quiescence so
  // externally-resourced types run their destructors before arena drop.
  void drain_and_quiesce();

  // The observability snapshot (see HousekeepingStats).
  HousekeepingStats stats() const noexcept;

private:
  // Commit at the target's root and account it; shared by every trigger. Resets
  // the since-checkpoint transaction counter on success.
  expected<std::monostate, WorkspaceFileError> commit_now();

  HousekeepingTarget* d_target;
  HousekeepingConfig d_config;

  std::uint64_t d_transactions_seen{0};
  std::uint64_t d_transactions_since_checkpoint{0};
  std::uint64_t d_drains_run{0};
  std::uint64_t d_checkpoints_committed{0};
  std::uint64_t d_checkpoints_skipped_clean{0};
  std::uint64_t d_last_checkpoint_tick{0};
};

} // namespace arbc
