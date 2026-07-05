#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace arbc {

// The durability protocol for the mmap workspace file (design doc 15,
// "Checkpointing rides the version model", lines 183-199). Turns the layout
// mmap_backing reserved (the two A/B header root slots) into an ordered-commit
// protocol so a crash costs at most the work since the last checkpoint, never a
// corrupt document. Four coupled mechanisms, all in arbc::pool:
//
//   1. Ordered commit (LMDB-style A/B root): msync data chunks, publish the new
//      root into the inactive header slot (flip), msync the header. A crash
//      lands on the old or the new root, both consistent.
//   2. Durability-epoch quarantine: a slot freed after the last checkpoint may
//      still back the on-disk root, so `SlotStore::release` is fenced -- freed
//      slots are held until a checkpoint makes their freeing durable.
//   3. Recovery = map, read the durable root, rebuild: `open` maps the file and
//      selects the highest-generation valid root; the caller walks the graph to
//      rebuild counts, then `finalize_open` rebuilds the free list.
//   4. Debug mprotect: published data chunks are made read-only so a stray write
//      through a stale pointer faults at the write site.
//
// `Checkpointer::commit` is a bare MECHANISM; deciding WHEN to checkpoint is
// runtime.housekeeping's cadence policy (doc 17: L5), not this layer's (L1).

// A root record packed into one header A/B root slot: {generation, root index},
// published by a single naturally-aligned 8-byte store (torn-write-free within a
// sector). `generation == 0` is the create-time zero: "never written".
struct WorkspaceRoot {
  std::uint32_t generation{0};
  std::uint32_t root_index{0};
};

inline std::uint64_t encode_root(WorkspaceRoot root) noexcept {
  return (static_cast<std::uint64_t>(root.generation) << 32) | root.root_index;
}

inline WorkspaceRoot decode_root(std::uint64_t value) noexcept {
  return WorkspaceRoot{static_cast<std::uint32_t>(value >> 32),
                       static_cast<std::uint32_t>(value & 0xFFFF'FFFFu)};
}

// The durability-epoch quarantine buffer, installed on a workspace-backed
// Arena's SlotStores via SlotStore::set_release_fence (the clean interposition
// point pool.reclamation left below reclaim). A freed slot is stamped with the
// current epoch and held; the owning Checkpointer releases it once a commit has
// made its freeing durable (durable_epoch >= stamp). Off by default, so every
// anonymous/arena-only path is byte-for-byte unchanged.
class DurabilityEpochFence final : public ReleaseFence {
public:
  explicit DurabilityEpochFence(const std::uint32_t& epoch) noexcept : d_epoch(&epoch) {}

  DurabilityEpochFence(const DurabilityEpochFence&) = delete;
  DurabilityEpochFence& operator=(const DurabilityEpochFence&) = delete;

  void on_release(SlotStore& store, SlotIndex index) override {
    d_quarantine.push_back(Entry{&store, index, *d_epoch});
    ++d_quarantined_total;
  }

  // Cumulative count of slots that ever entered quarantine (advances only on the
  // free path, never at commit) -- the dirty-scene signal the Checkpointer uses.
  std::uint64_t quarantined_total() const noexcept { return d_quarantined_total; }
  std::size_t pending() const noexcept { return d_quarantine.size(); }

private:
  friend class Checkpointer;
  struct Entry {
    SlotStore* store;
    SlotIndex index;
    std::uint32_t epoch;
  };
  const std::uint32_t* d_epoch;
  std::vector<Entry> d_quarantine;
  std::uint64_t d_quarantined_total{0};
};

class Checkpointer {
public:
  // Bind a checkpointer to a workspace file source and its arena. Initializes
  // the epoch/next-slot state from the source's durable root (a fresh file's
  // zeroed slots start at generation 0). Installs the chunk hole-punch fence on
  // the source; the caller installs the slot fence on each workspace-backed
  // store via `slot_fence()`.
  Checkpointer(WorkspaceFileChunkSource& source, Arena& arena)
      : d_source(&source), d_arena(&arena), d_slot_fence(d_epoch) {
    const WorkspaceRoot a = decode_root(source.root_slot(0));
    const WorkspaceRoot b = decode_root(source.root_slot(1));
    if (a.generation == 0 && b.generation == 0) {
      d_generation = 0;
      d_next_slot = 0; // fresh file: publish into slot A first
    } else if (a.generation >= b.generation) {
      d_generation = a.generation;
      d_next_slot = 1; // A is current: publish into B next
    } else {
      d_generation = b.generation;
      d_next_slot = 0;
    }
    d_durable_epoch = d_generation;
    d_epoch = d_generation + 1;
    d_chunk_fence.self = this;
    source.set_release_fence(&d_chunk_fence);
    snapshot();
  }

  ~Checkpointer() {
    // The source outlives us; drop the fence so a later chunk release (e.g. at
    // store teardown) never calls back into a destroyed checkpointer.
    d_source->set_release_fence(nullptr);
  }

  Checkpointer(const Checkpointer&) = delete;
  Checkpointer& operator=(const Checkpointer&) = delete;

  DurabilityEpochFence& slot_fence() noexcept { return d_slot_fence; }

  // Ordered commit (doc 15:183-194): (1) msync every live data chunk, (2)
  // publish the new root into the inactive header slot with a bumped generation
  // and flip, (3) msync the header. The data a root points to is durable before
  // the root that publishes it. On success the durability fences drain: slots
  // and emptied chunks freed this epoch become reusable/punched. An unchanged
  // scene skips the data msync (nothing to flush) but still flips + syncs the
  // header. Errors surface as values.
  expected<std::monostate, WorkspaceFileError> commit(SlotIndex root_index) {
    if (is_dirty()) {
      expected<std::size_t, WorkspaceFileError> synced = d_source->sync_data();
      if (!synced) {
        return unexpected(synced.error());
      }
      d_data_msyncs += *synced;
    }

    const std::uint32_t new_generation = d_epoch; // == d_generation + 1
    d_source->publish_root_slot(d_next_slot,
                                encode_root(WorkspaceRoot{new_generation, root_index}));
    expected<std::monostate, WorkspaceFileError> header = d_source->sync_header();
    if (!header) {
      return unexpected(header.error());
    }
    ++d_header_msyncs;

    // The header is durable: the root -- and everything freed before it -- is now
    // recoverable. Advance the published/durable state and flip the target slot.
    d_generation = new_generation;
    d_durable_epoch = new_generation;
    d_next_slot ^= 1;
    ++d_commit_count;

#ifndef NDEBUG
    // Seal the just-published data chunks read-only (a stray write faults). Only
    // FULLY-published chunks are sealed; each store's frontier chunk stays
    // writable so the writer keeps allocating the next version after a commit.
    d_arena->for_each_store([this](SlotStore& store) {
      store.for_each_sealable_chunk(
          [this](void* base, std::size_t size) { d_source->protect_range(base, size, true); });
    });
#endif
    drain_fences();
    d_epoch = d_generation + 1;
    snapshot();
    return std::monostate{};
  }

  // Recovery entry point (doc 15:142-145). Maps an existing workspace file,
  // validates its header, remaps the live data chunks, and selects the
  // highest-generation valid root. Returns the reopened source plus the durable
  // root; the caller builds an Arena over the source, calls `RefStore<T>::restore`
  // then walks the graph rebuilding counts, and finishes with `finalize_open`.
  struct OpenState {
    std::unique_ptr<WorkspaceFileChunkSource> source;
    SlotIndex root_index{0};
    std::uint32_t generation{0};
    bool valid{false}; // false when the file has no durable root (generation 0)
  };

  static expected<OpenState, WorkspaceFileError> open(const std::string& path) {
    expected<std::unique_ptr<WorkspaceFileChunkSource>, WorkspaceFileError> source =
        WorkspaceFileChunkSource::open(path);
    if (!source) {
      return unexpected(source.error());
    }
    const WorkspaceRoot a = decode_root((*source)->root_slot(0));
    const WorkspaceRoot b = decode_root((*source)->root_slot(1));
    const WorkspaceRoot chosen = a.generation >= b.generation ? a : b;
    OpenState state;
    state.source = std::move(*source);
    state.root_index = chosen.root_index;
    state.generation = chosen.generation;
    state.valid = chosen.generation != 0;
    return state;
  }

  // Recovery finalize: repopulate `store`'s free list with the below-high-water
  // complement of `live_set` (the reachable slots the walk found) and set the
  // live count. One call per workspace-backed store.
  void finalize_open(SlotStore& store, const std::vector<SlotIndex>& live_set) {
    store.finalize_restore(live_set);
  }

  // --- behavioral counters (doc 16, never wall-clock) -----------------------
  std::uint64_t commit_count() const noexcept { return d_commit_count; }
  std::uint64_t data_msyncs() const noexcept { return d_data_msyncs; }
  std::uint64_t header_msyncs() const noexcept { return d_header_msyncs; }
  std::uint64_t slots_freed_to_list() const noexcept { return d_slots_freed_to_list; }
  std::uint32_t epoch() const noexcept { return d_epoch; }
  std::uint32_t durable_epoch() const noexcept { return d_durable_epoch; }
  std::uint32_t generation() const noexcept { return d_generation; }

private:
  // ChunkReleaseFence adapter feeding the Checkpointer's chunk quarantine.
  struct ChunkFence final : ChunkReleaseFence {
    Checkpointer* self{nullptr};
    void on_release(WorkspaceFileChunkSource&, std::uint64_t offset, std::uint64_t size,
                    std::uint32_t index) override {
      self->d_chunk_quarantine.push_back(ChunkEntry{offset, size, index, self->d_epoch});
    }
  };
  struct ChunkEntry {
    std::uint64_t offset;
    std::uint64_t size;
    std::uint32_t index;
    std::uint32_t epoch;
  };

  // Return every quarantined slot / emptied chunk whose freeing this checkpoint
  // just made durable. Runs after the header is synced (writer-only).
  void drain_fences() {
    std::vector<DurabilityEpochFence::Entry>& slots = d_slot_fence.d_quarantine;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < slots.size(); ++i) {
      if (slots[i].epoch <= d_durable_epoch) {
#ifndef NDEBUG
        // The slot is about to be reused: its chunk was just sealed read-only, so
        // reopen its page for the next write (debug hardening, best-effort).
        d_source->protect_range(slots[i].store->resolve(slots[i].index),
                                slots[i].store->slot_stride(), false);
#endif
        slots[i].store->free_now(slots[i].index);
        ++d_slots_freed_to_list;
      } else {
        slots[kept++] = slots[i];
      }
    }
    slots.resize(kept);

    std::size_t chunk_kept = 0;
    for (std::size_t i = 0; i < d_chunk_quarantine.size(); ++i) {
      if (d_chunk_quarantine[i].epoch <= d_durable_epoch) {
        d_source->punch_now(d_chunk_quarantine[i].offset, d_chunk_quarantine[i].size,
                            d_chunk_quarantine[i].index);
      } else {
        d_chunk_quarantine[chunk_kept++] = d_chunk_quarantine[i];
      }
    }
    d_chunk_quarantine.resize(chunk_kept);
  }

  // Has the arena mutated since the last commit? Growth advances high-water, a
  // net live change moves the live count, and any free bumps the quarantine
  // total -- so an all-three-unchanged scene issues no data msync.
  bool is_dirty() const {
    return d_arena->total_slots_live() != d_snap_live ||
           d_arena->total_high_water() != d_snap_hw ||
           d_slot_fence.quarantined_total() != d_snap_quarantined;
  }

  void snapshot() {
    d_snap_live = d_arena->total_slots_live();
    d_snap_hw = d_arena->total_high_water();
    d_snap_quarantined = d_slot_fence.quarantined_total();
  }

  WorkspaceFileChunkSource* d_source;
  Arena* d_arena;

  std::uint32_t d_epoch{1};          // current in-progress epoch (== generation + 1)
  std::uint32_t d_generation{0};     // last published root generation
  std::uint32_t d_durable_epoch{0};  // highest epoch made durable by a commit
  int d_next_slot{0};                // header slot the next commit publishes into (0=A, 1=B)

  DurabilityEpochFence d_slot_fence;
  ChunkFence d_chunk_fence{};
  std::vector<ChunkEntry> d_chunk_quarantine;

  std::uint64_t d_commit_count{0};
  std::uint64_t d_data_msyncs{0};
  std::uint64_t d_header_msyncs{0};
  std::uint64_t d_slots_freed_to_list{0};

  std::size_t d_snap_live{0};
  std::size_t d_snap_hw{0};
  std::uint64_t d_snap_quarantined{0};
};

} // namespace arbc
