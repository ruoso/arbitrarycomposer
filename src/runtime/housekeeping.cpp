#include <arbc/runtime/housekeeping.hpp>

namespace arbc {

Housekeeper::Housekeeper(ReclamationQueue& queue, Checkpointer* checkpointer, Arena* arena,
                         HousekeepingConfig config) noexcept
    : d_queue(&queue), d_checkpointer(checkpointer), d_arena(arena), d_config(config) {}

expected<std::monostate, WorkspaceFileError> Housekeeper::commit_tip() {
  // Callers guard on a non-null checkpointer before reaching here.
  expected<std::monostate, WorkspaceFileError> committed = d_checkpointer->commit(d_tip);
  if (!committed) {
    return unexpected(committed.error());
  }
  ++d_checkpoints_committed;
  d_transactions_since_checkpoint = 0;
  return std::monostate{};
}

expected<std::monostate, WorkspaceFileError> Housekeeper::after_commit(SlotIndex root) {
  if (d_config.drain_between_transactions) {
    d_queue->drain();
    ++d_drains_run;
  }
  d_tip = root;
  ++d_transactions_seen;
  ++d_transactions_since_checkpoint;

  // Trigger (a): commit every Nth transaction. The transaction proxy is never
  // clean at this point (the count just advanced), so this never skips-clean.
  if (d_checkpointer != nullptr && d_config.checkpoint_every_n_transactions &&
      d_transactions_since_checkpoint >= *d_config.checkpoint_every_n_transactions) {
    return commit_tip();
  }
  return std::monostate{};
}

expected<std::monostate, WorkspaceFileError> Housekeeper::tick(std::uint64_t monotonic_tick) {
  d_queue->drain();
  ++d_drains_run;

  // Trigger (b): a handed tick past the interval, gated on dirtiness. The tick
  // is a handed monotonic value -- this reads no clock (doc 16:54-62).
  if (d_checkpointer != nullptr && d_config.checkpoint_tick_interval &&
      monotonic_tick - d_last_checkpoint_tick >= *d_config.checkpoint_tick_interval) {
    if (d_transactions_since_checkpoint >= 1) {
      expected<std::monostate, WorkspaceFileError> committed = commit_tip();
      if (!committed) {
        return unexpected(committed.error());
      }
    } else {
      // The interval elapsed but the scene is still: a still scene issues no
      // checkpoint. Reset the window so a long idle stretch counts one skip per
      // interval, not one per tick.
      ++d_checkpoints_skipped_clean;
    }
    d_last_checkpoint_tick = monotonic_tick;
  }
  return std::monostate{};
}

expected<std::monostate, WorkspaceFileError> Housekeeper::request_checkpoint() {
  if (d_checkpointer == nullptr) {
    return std::monostate{};
  }
  // The explicit host call is unconditional (autosave / export / quit). The
  // underlying commit still skips the data msync on a genuinely clean scene.
  return commit_tip();
}

void Housekeeper::drain_and_quiesce() {
  d_queue->drain();
  ++d_drains_run;
}

HousekeepingStats Housekeeper::stats() const noexcept {
  HousekeepingStats snapshot;
  snapshot.transactions_seen = d_transactions_seen;
  snapshot.drains_run = d_drains_run;
  snapshot.checkpoints_committed = d_checkpoints_committed;
  snapshot.checkpoints_skipped_clean = d_checkpoints_skipped_clean;
  snapshot.live_slots = d_arena != nullptr ? d_arena->total_slots_live() : 0;
  snapshot.slots_freed_to_list =
      d_checkpointer != nullptr ? d_checkpointer->slots_freed_to_list() : 0;
  snapshot.durable_epoch = d_checkpointer != nullptr ? d_checkpointer->durable_epoch() : 0;
  return snapshot;
}

} // namespace arbc
