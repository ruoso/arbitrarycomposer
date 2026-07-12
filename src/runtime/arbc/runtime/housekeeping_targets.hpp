#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/model/model.hpp>
#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/reclamation.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>
#include <arbc/runtime/housekeeping.hpp>

#include <cstddef>
#include <cstdint>
#include <variant>

namespace arbc {

// The two `HousekeepingTarget` implementations (`runtime.housekeeping_document_wiring`
// Decision 1). Both live in `runtime` (L5), which is the only level that may see a
// `Model` (L2) and the pool primitives (L1) at once -- and, crucially, the only place
// the reclaim-context duty can be discharged without teaching `model` or `pool`
// anything about housekeeping.

// A live document's `Model`. `drain()` goes through `Model::drain()`, which publishes
// the HAMT `ReclaimContext` -- so no drainer, on any thread, can reach the queue
// without it and silently leak every interior edge (hamt.hpp:103-109). `checkpoint()`
// goes through `Model::checkpoint()`, which resolves its own current root, so the
// Housekeeper never has to track a tip.
class ModelHousekeepingTarget final : public HousekeepingTarget {
public:
  explicit ModelHousekeepingTarget(Model& model) noexcept : d_model(&model) {}

  void drain() override { d_model->drain(); }

  expected<std::monostate, WorkspaceFileError> checkpoint() override {
    return d_model->checkpoint();
  }

  bool checkpointable() const noexcept override { return d_model->workspace_backed(); }

  std::size_t live_slots() const noexcept override { return d_model->live_slots(); }
  std::size_t bytes_reserved() const noexcept override { return d_model->bytes_reserved(); }

  std::uint64_t slots_freed_to_list() const noexcept override {
    const Checkpointer* const ckpt = d_model->checkpointer();
    return ckpt != nullptr ? ckpt->slots_freed_to_list() : 0;
  }
  std::uint32_t durable_epoch() const noexcept override {
    const Checkpointer* const ckpt = d_model->checkpointer();
    return ckpt != nullptr ? ckpt->durable_epoch() : 0;
  }

private:
  Model* d_model;
};

// The bare pool triple `runtime.housekeeping` was validated against: a reclamation
// queue, an optional checkpointer, an optional arena. It reproduces the pre-seam
// semantics exactly, including the caller-handed commit root -- `set_root()` is the
// `after_commit(SlotIndex)` argument that used to ride the policy object. A pool
// substrate has no HAMT and so needs no reclaim context; `queue.drain()` is the whole
// duty here.
class PoolHousekeepingTarget final : public HousekeepingTarget {
public:
  // `checkpointer` is null for anonymous, live-only arenas (doc 15:160-162): its
  // checkpoint triggers become inert. `arena` is the observability source, null when
  // no arena is bound (the memory-panel numbers then read 0).
  PoolHousekeepingTarget(ReclamationQueue& queue, Checkpointer* checkpointer,
                         Arena* arena) noexcept
      : d_queue(&queue), d_checkpointer(checkpointer), d_arena(arena) {}

  // The root a checkpoint commits at -- the caller's published tip.
  void set_root(SlotIndex root) noexcept { d_root = root; }

  void drain() override { d_queue->drain(); }

  expected<std::monostate, WorkspaceFileError> checkpoint() override {
    return d_checkpointer->commit(d_root);
  }

  bool checkpointable() const noexcept override { return d_checkpointer != nullptr; }

  std::size_t live_slots() const noexcept override {
    return d_arena != nullptr ? d_arena->total_slots_live() : 0;
  }
  std::size_t bytes_reserved() const noexcept override {
    return d_arena != nullptr ? d_arena->total_bytes_reserved() : 0;
  }
  std::uint64_t slots_freed_to_list() const noexcept override {
    return d_checkpointer != nullptr ? d_checkpointer->slots_freed_to_list() : 0;
  }
  std::uint32_t durable_epoch() const noexcept override {
    return d_checkpointer != nullptr ? d_checkpointer->durable_epoch() : 0;
  }

private:
  ReclamationQueue* d_queue;
  Checkpointer* d_checkpointer; // nullable
  Arena* d_arena;               // nullable
  SlotIndex d_root{0};
};

} // namespace arbc
