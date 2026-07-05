#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>

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
// It owns no thread and reads no clock. Doc 15:129-136 sanctions EITHER drain
// site -- the writer between transactions OR a low-priority thread -- and the
// mechanism is single-drainer-any-thread (reclamation.hpp:57-62); keeping this
// object passive and thread-agnostic keeps it correct under either regime. The
// contract the CALLER must honor: exactly one thread calls a draining entry
// point at a time. RT threads only enqueue (they never touch a Housekeeper).
// The owned background low-priority thread is the deferred follow-up
// `runtime.housekeeping_thread`.

// The cadence knobs, all defaulted. An absent optional disables that checkpoint
// trigger; an anonymous (no-checkpointer) arena ignores both. These realize
// doc 15:213-214's three triggers -- transaction count, timer, explicit host
// call -- as declarative policy.
struct HousekeepingConfig {
  // Drain the reclamation queue to quiescence in `after_commit` (doc 15:129-136,
  // the writer-between-transactions drain site). When false, released slots stay
  // on the queue until an explicit `drain_and_quiesce()` (or a `tick`).
  bool drain_between_transactions = true;
  // Commit a checkpoint every Nth transaction (trigger (a)). Absent disables it.
  std::optional<std::uint64_t> checkpoint_every_n_transactions;
  // Commit a checkpoint when a handed tick has advanced this far past the last
  // checkpoint AND at least one transaction has occurred since (trigger (b)).
  // Absent disables it.
  std::optional<std::uint64_t> checkpoint_tick_interval;
};

// The wall-clock-free observability snapshot a host memory panel reads
// (doc 15:149-151). Aggregates the Housekeeper's own event counters with the
// arena's live-slot count and pass-through Checkpointer counters -- no new
// counting mechanism, just a composed view of numbers the mechanisms publish.
struct HousekeepingStats {
  std::uint64_t transactions_seen{0};         // total after_commit calls
  std::uint64_t drains_run{0};                // reclamation drains issued
  std::uint64_t checkpoints_committed{0};     // commits issued (auto + explicit)
  std::uint64_t checkpoints_skipped_clean{0}; // auto triggers gated by dirtiness
  std::size_t live_slots{0};                  // Arena::total_slots_live(), 0 if unbound
  std::uint64_t slots_freed_to_list{0};       // Checkpointer pass-through, 0 if unbound
  std::uint32_t durable_epoch{0};             // Checkpointer pass-through, 0 if unbound
};

class Housekeeper {
public:
  // `queue` is always required (reclamation cadence applies to every arena).
  // `checkpointer` is null for anonymous, live-only arenas (doc 15:160-162):
  // its checkpoint triggers become inert. `arena` is the live-count source for
  // observability, null when no arena is bound (stats().live_slots reads 0).
  Housekeeper(ReclamationQueue& queue, Checkpointer* checkpointer, Arena* arena,
              HousekeepingConfig config) noexcept;

  Housekeeper(const Housekeeper&) = delete;
  Housekeeper& operator=(const Housekeeper&) = delete;

  // The between-transactions drain site (doc 15:129-136). Call after each
  // published transaction: drains the reclamation queue to quiescence (when
  // configured), records `root` as the checkpointable tip, bumps the transaction
  // counters, and -- if the transaction-count trigger has reached its threshold
  // -- commits a checkpoint at `root`. A commit I/O failure surfaces as a value.
  expected<std::monostate, WorkspaceFileError> after_commit(SlotIndex root);

  // The timer / low-priority-thread entry. Drains the queue, then, if the
  // tick-interval trigger has elapsed AND at least one transaction has occurred
  // since the last checkpoint, commits at the recorded tip. `monotonic_tick` is
  // a HANDED value, never a wall-clock read (doc 16; the caller owns the clock).
  expected<std::monostate, WorkspaceFileError> tick(std::uint64_t monotonic_tick);

  // The explicit host-call trigger (autosave / export / quit). Commits at the
  // recorded tip unconditionally -- `Checkpointer::commit` still skips the data
  // msync on a genuinely clean scene, so a redundant request is cheap.
  expected<std::monostate, WorkspaceFileError> request_checkpoint();

  // Teardown / bulk-release path (doc 15:144-147): drain to quiescence so
  // externally-resourced types run their destructors before arena drop.
  void drain_and_quiesce();

  // The observability snapshot (see HousekeepingStats).
  HousekeepingStats stats() const noexcept;

private:
  // Commit at the recorded tip and account it; shared by every trigger. Resets
  // the since-checkpoint transaction counter on success.
  expected<std::monostate, WorkspaceFileError> commit_tip();

  ReclamationQueue* d_queue;
  Checkpointer* d_checkpointer; // nullable
  Arena* d_arena;               // nullable
  HousekeepingConfig d_config;

  SlotIndex d_tip{0}; // the last root handed to after_commit -- the commit target

  std::uint64_t d_transactions_seen{0};
  std::uint64_t d_transactions_since_checkpoint{0};
  std::uint64_t d_drains_run{0};
  std::uint64_t d_checkpoints_committed{0};
  std::uint64_t d_checkpoints_skipped_clean{0};
  std::uint64_t d_last_checkpoint_tick{0};
};

} // namespace arbc
