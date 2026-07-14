#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <bit>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace arbc {

// The SlotStore chunk-bits exponent for a store-table row's slots-per-chunk
// (validated as a nonzero power of two by `adopt_snapshot`, so this never sees a
// value `countr_zero` cannot invert).
inline std::uint32_t chunk_bits_for(std::uint32_t chunk_slots) noexcept {
  return static_cast<std::uint32_t>(std::countr_zero(chunk_slots));
}

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

// The outcome of the A/B root selection: which header slot a reopen resumes from.
struct RootSelection {
  int slot{0};          // header root slot (0 = A, 1 = B)
  WorkspaceRoot root{}; // the root that slot holds
  bool eligible{false}; // its store-table snapshot's stamp matches its generation
};

// THE ONE root-selection rule, used by both `Checkpointer::open` and the
// `Checkpointer` constructor (pool.header_writeback_ordering). Two copies of it would
// let the constructor pick a `d_next_slot` that overwrites the very snapshot the open
// path just recovered from.
//
// Newest generation first (ties to A), but a root is only ELIGIBLE when the snapshot
// it owns is stamped with its generation. The root slots and the store-table snapshots
// live on different pages of the header mapping, and the kernel orders their writeback
// not at all: a crash during the one header msync can persist the new root paired with
// the STALE snapshot, whose too-low high-water would make the recovery hole-punch the
// very chunks that root's records live in. The stamp makes the pairing checkable, so
// such a root is skipped and the older, still-coherent root is resumed instead. A fresh
// file's zeroed slots and zeroed stamps pair trivially (generation 0 == stamp 0).
inline RootSelection select_root(const WorkspaceFileChunkSource& source) noexcept {
  const WorkspaceRoot a = decode_root(source.root_slot(0));
  const WorkspaceRoot b = decode_root(source.root_slot(1));
  const int newest = a.generation >= b.generation ? 0 : 1;
  for (const int slot : {newest, newest ^ 1}) {
    const WorkspaceRoot root = slot == 0 ? a : b;
    if (source.snapshot_generation(slot) == root.generation) {
      return RootSelection{slot, root, true};
    }
  }
  // Neither root owns the snapshot beside it: the header is corrupt beyond what A/B
  // redundancy can repair. Refusing beats a silent under-restore.
  return RootSelection{};
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
    // The SAME eligibility rule `Checkpointer::open` resumed from: the next commit
    // must publish into the OTHER slot, or it would overwrite the snapshot the open
    // path just recovered from -- the only coherent one left after a torn header.
    // `open` refuses a file with no eligible root, and a fresh file's zeroed slots are
    // trivially eligible, so every path that reaches here has one.
    const RootSelection selected = select_root(source);
    assert(selected.eligible && "a Checkpointer over a file with no root/snapshot pair");
    d_generation = selected.root.generation;
    // Generation 0 is "never written": there is no durable root to preserve, so a fresh
    // file publishes into slot A first (and its snapshot A with it).
    d_next_slot = d_generation == 0 ? 0 : (selected.slot ^ 1);
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

  // Register a workspace-backed store whose slot high-water every commit publishes
  // into the store-table snapshot the root it flips owns. Writer-only setup,
  // mirroring `set_release_fence`. Idempotent. Registering a store with no
  // store-table row (an untagged / anonymous store) is a no-op: it has no row to
  // publish into. A debug assert in `commit` catches the converse mistake -- a
  // workspace-bound store left unregistered, whose high-water would never become
  // durable.
  void register_store(SlotStore& store) {
    const std::uint32_t id = d_source->store_id(static_cast<std::uint32_t>(store.slot_stride()),
                                                static_cast<std::uint32_t>(store.slot_align()));
    if (id == k_workspace_no_owner || is_registered(store)) {
      return;
    }
    d_registered.push_back(RegisteredStore{&store, id});
  }

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

    // (2a) Publish each registered store's high-water into the INACTIVE store-table
    // snapshot -- the one owned by the root slot about to be flipped. Step 1's data
    // msync has already made every chunk those high-waters cover durable, and step
    // 3's header msync makes the root AND its snapshot durable together, so this
    // costs ZERO additional syscalls (Decision 4). A crash before step 3 leaves the
    // old root durable, paired with the snapshot this commit never touched: old root
    // with old high-water, always -- never a mismatched pair.
#ifndef NDEBUG
    d_arena->for_each_store([this](SlotStore& store) {
      const std::uint32_t id = d_source->store_id(static_cast<std::uint32_t>(store.slot_stride()),
                                                  static_cast<std::uint32_t>(store.slot_align()));
      assert((id == k_workspace_no_owner || is_registered(store)) &&
             "a workspace-bound store must be register_store'd, or its high-water never "
             "becomes durable and a recovery under-restores it");
    });
#endif
    for (const RegisteredStore& entry : d_registered) {
      d_source->publish_store_high_water(d_next_slot, entry.id,
                                         static_cast<std::uint32_t>(entry.store->high_water()));
    }

    // (2b) Stamp that snapshot with the generation this commit is about to put in the
    // root slot. The rows above and this stamp share one page, so the kernel cannot
    // write back the stamp without them; the root slot lives on a DIFFERENT page, whose
    // writeback nothing orders against this one. A crash that persists the new root but
    // not its snapshot therefore leaves a root whose stamp does not match -- detectable,
    // and `select_root` falls back to the older root that still pairs. Plain stores, on
    // the writer thread, in the same critical section as the flip below: no syscall, no
    // new cross-thread state.
    d_source->publish_snapshot_generation(d_next_slot, new_generation);

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
#ifndef NDEBUG
    // ... then reopen the page of every slot a later `allocate` could hand out. The
    // seal above protects whole CHUNKS, but a slot on a free list is not published
    // data: the next allocation placement-news into it. Sealing the chunk it lives
    // in would fault that write -- and NOT just for the quarantine this commit
    // released (`drain_fences`, which ran first, so those are on the free list now),
    // but for every slot freed by an EARLIER commit that is still waiting to be
    // reused, which this seal would otherwise re-protect on every commit. Page
    // granularity makes it best-effort in the other direction: a live slot sharing a
    // page with a free one is reopened along with it.
    d_arena->for_each_store([this](SlotStore& store) {
      for (const SlotIndex index : store.reusable_slots()) {
        d_source->protect_range(store.resolve(index), store.slot_stride(), false);
      }
    });
#endif
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
    // The newest root whose store-table snapshot is stamped with its own generation --
    // VERIFIED, not assumed (see `select_root`). A root the check rejects is not a
    // corrupt file, merely one whose snapshot a torn header left behind, so the older
    // root is tried; only when NEITHER pairs is the file refused. That is a
    // root-selection predicate, upstream of `adopt_snapshot`: a snapshot that fails
    // adoption's CONTENT validation stays a hard failure, never a retry on the older
    // root (Decision 1).
    const RootSelection chosen = select_root(**source);
    if (!chosen.eligible) {
      return unexpected(WorkspaceFileError{WorkspaceFileErrc::StoreDirectoryInconsistent, 0});
    }

    // Adopt the store-table snapshot the SELECTED root published (doc 15's A/B
    // discipline extended to the store table). This validates that every store's
    // chunk set covers its recorded high-water and hole-punches the chunks a
    // crashed commit appended above it -- so what the caller restores below is a
    // lookup, never a guess.
    expected<std::monostate, WorkspaceFileError> adopted = (*source)->adopt_snapshot(chosen.slot);
    if (!adopted) {
      return unexpected(adopted.error());
    }

    OpenState state;
    state.source = std::move(*source);
    state.root_index = chosen.root.root_index;
    state.generation = chosen.root.generation;
    state.valid = chosen.root.generation != 0;
    return state;
  }

  // Recovery: re-bind every store the selected root's store-table snapshot records
  // and reserve exactly the high-water it published. `Arena::store_for` routes each
  // store to its own per-store facade, so a store re-binds exactly the chunks the
  // arena directory tags as its own, in slot order -- and because the chunks are
  // already mapped, the file never grows. The caller then runs its typed
  // reachability walk and `finalize_open` per store.
  //
  // Requires an Arena constructed over the source's `router()`.
  expected<std::monostate, WorkspaceFileError> reserve_restored_all(Arena& arena) {
    // A store the caller already bound with the wrong geometry poisoned the bind;
    // surface that before restoring anything on top of it.
    if (d_source->bind_error().code != WorkspaceFileErrc::Ok) {
      return unexpected(d_source->bind_error());
    }
    const std::vector<WorkspaceStoreEntry> rows = d_source->restored_stores();
    for (const WorkspaceStoreEntry& row : rows) {
      if (row.slot_stride == 0) {
        continue; // unused row
      }
      SlotStore& store =
          arena.store_for(row.slot_stride, row.slot_align, chunk_bits_for(row.chunk_slots));
      if (d_source->bind_error().code != WorkspaceFileErrc::Ok) {
        return unexpected(d_source->bind_error());
      }
      register_store(store);
      if (!store.reserve_restored(row.high_water)) {
        // `adopt_snapshot` already proved the chunk set covers the high-water, so
        // the only way to land here is a high-water past what the store's directory
        // can address -- a corrupt row, refused as a value.
        return unexpected(WorkspaceFileError{WorkspaceFileErrc::StoreDirectoryInconsistent, 0});
      }
    }
    return std::monostate{};
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
  // A store bound to a store-table row, whose high-water each commit publishes into
  // the flipped root's snapshot. GEOMETRY AND HIGH-WATER ONLY reach disk -- no
  // refcount, free list, or generation tag is ever written (doc 15:199-205).
  struct RegisteredStore {
    SlotStore* store;
    std::uint32_t id; // store-table row index
  };

  bool is_registered(const SlotStore& store) const {
    for (const RegisteredStore& entry : d_registered) {
      if (entry.store == &store) {
        return true;
      }
    }
    return false;
  }

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
        // The slot's chunk was just sealed read-only; `commit` reopens the pages of
        // every free-list slot right after this drain (it must cover slots freed by
        // EARLIER epochs too, which this loop never sees again).
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
    return d_arena->total_slots_live() != d_snap_live || d_arena->total_high_water() != d_snap_hw ||
           d_slot_fence.quarantined_total() != d_snap_quarantined;
  }

  void snapshot() {
    d_snap_live = d_arena->total_slots_live();
    d_snap_hw = d_arena->total_high_water();
    d_snap_quarantined = d_slot_fence.quarantined_total();
  }

  WorkspaceFileChunkSource* d_source;
  Arena* d_arena;

  std::uint32_t d_epoch{1};         // current in-progress epoch (== generation + 1)
  std::uint32_t d_generation{0};    // last published root generation
  std::uint32_t d_durable_epoch{0}; // highest epoch made durable by a commit
  int d_next_slot{0};               // header slot the next commit publishes into (0=A, 1=B)

  DurabilityEpochFence d_slot_fence;
  ChunkFence d_chunk_fence{};
  std::vector<ChunkEntry> d_chunk_quarantine;
  std::vector<RegisteredStore> d_registered;

  std::uint64_t d_commit_count{0};
  std::uint64_t d_data_msyncs{0};
  std::uint64_t d_header_msyncs{0};
  std::uint64_t d_slots_freed_to_list{0};

  std::size_t d_snap_live{0};
  std::size_t d_snap_hw{0};
  std::uint64_t d_snap_quarantined{0};
};

} // namespace arbc
