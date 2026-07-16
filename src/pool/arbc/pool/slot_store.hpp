#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>
#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/slab_directory.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>

#ifndef NDEBUG
#include <thread>
#endif

namespace arbc {

// Storage address of a slot within a store. Distinct from ObjectId: an index
// is a storage location that recycles, an ObjectId is a document identity.
using SlotIndex = std::uint32_t;

// One thread's LIFO free pool within a store (pool.free_pools). Defined in the
// .cpp; the store owns these (so they outlive the threads that touch them) and a
// thread reaches its own via a thread-local {store-id -> pool} cache.
struct SlotFreePool;

// Slots-per-chunk exponent k targeting ~64 KiB data chunks, capped at 12
// (the max the 10+10 root/table split keeps addressable in a 32-bit index).
// Larger slots get fewer slots per chunk; slots larger than a chunk target
// get one slot per chunk.
constexpr std::uint32_t default_chunk_bits(std::size_t slot_stride) {
  constexpr std::size_t target_bytes = std::size_t{64} * 1024;
  const std::size_t slots = target_bytes / (slot_stride == 0 ? 1 : slot_stride);
  std::uint32_t bits = 0;
  while (bits < 12 && (std::size_t{1} << (bits + 1)) <= slots) {
    ++bits;
  }
  return bits;
}

// Round `n` up to a multiple of `align` (a power of two).
constexpr std::size_t align_up(std::size_t n, std::size_t align) {
  return (n + align - 1) & ~(align - 1);
}

// Nullable interposition point between `SlotStore::release` and the free list
// (doc 15's durability-epoch quarantine). Default is `nullptr` — release pushes
// straight to the free list, so every anonymous/arena-only usage is byte-for-
// byte unchanged. When a fence is installed (pool.checkpoints' durability
// fence), `release` diverts the freed slot to the fence instead of the free
// list; the fence returns it through `free_now` only once a checkpoint has made
// the freeing durable. This mirrors the nullable zero-count sink of pool.refs.
class SlotStore;
class ARBC_API ReleaseFence {
public:
  virtual ~ReleaseFence() = default;
  // The store has decided `index` is free but a fence is installed: quarantine
  // it (stamped with the current durability epoch). The slot's data bytes are
  // NOT mutated here and stay resolvable until `free_now` returns it.
  virtual void on_release(SlotStore& store, SlotIndex index) = 0;
};

// Diagnostic backing seam for the REFCOUNT column (doc 15). Production leaves
// this null and refcount chunks are plain anonymous `new[]`/`delete[]` heap --
// the portable default. A harness installs a page-aligned, mprotectable backing
// to machine-check the inside-out invariant that a `const&`/`peek` traversal
// touches no refcount page (the honest-baseline premise,
// 15-memory-model#const-ref-traversal-touches-no-refcount-page): with the
// refcount chunks frozen read-only, a `peek` traversal must not fault, whereas a
// by-value shared_ptr traversal -- which dirties a count per node per visit --
// would. Only the refcount column routes here (that is the column the invariant
// is about); the generation column keeps its `new[]` backing. Because the
// column is owned by the size-class store, the backing is a store property
// supplied at store creation: one backing per size class (the first typed view
// to mint the store wins; later views over the same class share it).
class ARBC_API RefcountTableBacking {
public:
  virtual ~RefcountTableBacking() = default;
  RefcountTableBacking() = default;
  RefcountTableBacking(const RefcountTableBacking&) = delete;
  RefcountTableBacking& operator=(const RefcountTableBacking&) = delete;

  // Storage for `count` live (constructed) `std::atomic<std::uint32_t>`, each
  // holding 0 on return -- matching the value-initialized `new[]` default.
  virtual std::atomic<std::uint32_t>* allocate(std::size_t count) = 0;
  virtual void deallocate(std::atomic<std::uint32_t>* base, std::size_t count) noexcept = 0;
};

// Instance-owned fixed-slot storage for one size class (doc 15). Slots have
// stable addresses for the life of the store; growth appends chunks through
// the ChunkSource and never reallocates. Fragmentation is structurally
// impossible: a released slot is a perfect hole reused by the next same-class
// allocation.
//
// The store owns the inside-out parallel columns keyed by PHYSICAL slot index:
// one `std::atomic<std::uint32_t>` refcount column and (debug only) one
// generation column, grown in lock-step with the data chunks (writer-only, at
// chunk-mint time) so a column can never be short of the data directory. These
// columns are anonymous runtime state (never persisted, rebuilt on open) and
// are read/written by the `pool.refs` typed views over this store. Because they
// are keyed by physical slot -- not by the typed view -- several `RefStore<T>` /
// `RefStore<U>` views that share one size class share ONE count column and ONE
// generation column: a slot has exactly one logical reference count wherever it
// is viewed from, and a slot recycled from `T` to `U` reuses the same count
// entry and carries its generation bump across the views.
//
// Threading (doc 15, pool.free_pools): the guard relaxation is ASYMMETRIC.
// `allocate`/`reserve_restored`/`finalize_restore` stay WRITER-THREAD-ONLY (a
// debug-build assert enforces it) because arena growth -- chunk publish, column
// publish -- is single-threaded: the writer is the only structural allocator.
// The capacity ACCOUNTING it maintains along the way is a different matter: a
// host memory panel reads it from its own thread while the writer allocates
// (doc 15 `:186-198`), so `d_high_water`/`d_slots_capacity`/`d_bytes_reserved`
// are RELAXED ATOMICS -- lock-free on the writer, legal to load from any thread
// (pool.stats_counter_race). Relaxed, not release: they publish no data, they
// are diagnostics on no correctness path, and each reader only needs a value the
// counter actually held. `high_water()` stays correctness-load-bearing on the
// writer (the checkpointer reads it to write the store table), which is sound
// precisely because the writer is its only mutator: program order hands the
// writer its own latest value. `release`/`free_now` admit ANY thread
// (the low-priority housekeeping drain releases cross-thread): each thread pushes
// freed slots onto its own thread-local LIFO pool with no lock and no allocation,
// spilling a batch to the shared global pool only when its local pool fills, and
// the writer's `allocate` refills a batch from the global pool when its local
// pool runs dry -- so reuse is thread-affine (`15-memory-model.md:45-46,137-143`)
// and the writer's allocation never contends with a concurrent release. The
// deferred reclamation drain stays SINGLE-DRAINER (exactly one thread at a time,
// writer between transactions OR the low-priority thread -- runtime.housekeeping
// serializes the choice), which is what `RefStore::drain_pending`'s exchange
// detach relies on. `resolve` is safe from any thread concurrently with growth.
//
// `release` marks a slot reusable but DOES NOT run the object's destructor —
// running or deferring destruction is the caller's obligation (doc 15:
// release enqueues, never destroys inline). This is the seam
// pool.reclamation's deferred queue plugs into.
class ARBC_API SlotStore {
public:
  // `refcount_backing` is null in production (portable `new[]` refcount chunks);
  // a diagnostic harness passes a page-aligned, mprotectable backing to freeze
  // the count column read-only and witness the zero-refcount-traffic traversal.
  SlotStore(std::size_t slot_size, std::size_t slot_align, std::uint32_t chunk_bits,
            ChunkSource& source, RefcountTableBacking* refcount_backing = nullptr);
  ~SlotStore();
  SlotStore(const SlotStore&) = delete;
  SlotStore& operator=(const SlotStore&) = delete;

  // Batch size for the thread-local <-> global free-pool boundary (pool.free_
  // pools). A thread-local pool caches at most this many slots; on filling it
  // spills the whole batch to the shared global pool, and a dry `allocate`
  // refills up to a batch from the global pool. Sized for the two-thread (writer
  // + one drainer) reality; not tuned in this task.
  static constexpr std::size_t k_free_pool_batch = 32;

  // Reserve a slot and return its index. WRITER-ONLY (arena growth is
  // single-threaded). Pops from the writer's thread-local pool, refilling a batch
  // from the global pool when it is empty before falling back to high-water
  // growth. Returns an error (never throws, never aborts) when the ChunkSource
  // refuses to grow or the index space is exhausted.
  expected<SlotIndex, PoolError> allocate();

  // Mark a slot reusable. Does NOT destroy the slot's object (caller's
  // obligation). ANY THREAD (pool.free_pools): pushes onto the calling thread's
  // thread-local free pool -- no lock, no allocation. With a release fence
  // installed the slot is diverted to the fence instead of the free pool.
  void release(SlotIndex index);

  // Install (or clear, with nullptr) the durability-epoch release fence. Default
  // is no fence (direct free-pool push). Writer-only setup, but the pointer is
  // atomic because a cross-thread `release` reads it concurrently.
  void set_release_fence(ReleaseFence* fence) noexcept {
    d_release_fence.store(fence, std::memory_order_release);
  }

  // Un-fenced return of a slot to the free pool, called by a ReleaseFence once
  // the freeing is durable. Does NOT touch the live count (release already
  // decremented it) and never runs a destructor. Runs on the releasing thread
  // and pushes onto THAT thread's local pool (any thread).
  void free_now(SlotIndex index);

  // Resolve a slot index to its stable address. Any thread.
  void* resolve(SlotIndex index) const noexcept;

  // Recovery (doc 15, "map → validate → select → rebuild"). Bind existing
  // file-backed chunks covering `[0, high_water)` from the (reopened) source and
  // set the high-water mark WITHOUT constructing anything: the records already
  // live in the file. Counts and free list are rebuilt by the caller
  // (RefStore::set_count on the reachability walk, then `finalize_restore`).
  // Writer-only. The reopened source returns the already-mapped file chunks in
  // order, so this never grows the file.
  expected<std::monostate, PoolError> reserve_restored(std::uint32_t high_water);

  // Recovery finalize: repopulate the free list with the below-high-water
  // complement of `live` (the reachable slots the walk found) and set the live
  // count. Writer-only.
  void finalize_restore(const std::vector<SlotIndex>& live);

  std::uint32_t high_water() const noexcept { return d_high_water.load(std::memory_order_relaxed); }

  // Debug checkpoint seal (doc 15): visit every FULLY-published data chunk base
  // (all chunks below the frontier chunk still being filled), so the caller can
  // mprotect published records read-only while continued allocation into the
  // frontier chunk stays writable. `fn(base, chunk_bytes)`.
  template <class Fn> void for_each_sealable_chunk(Fn&& fn) const {
    const std::uint32_t frontier = high_water() >> d_chunk_bits;
    for (std::uint32_t chunk_number = 0; chunk_number < frontier; ++chunk_number) {
      std::byte* base = d_directory.chunk(chunk_number);
      if (base != nullptr) {
        fn(static_cast<void*>(base), d_chunk_bytes);
      }
    }
  }

  // Every slot a later `allocate` could hand out: the shared global free pool plus
  // every thread's local pool. The companion of `for_each_sealable_chunk` -- a slot
  // on a free list is NOT published data (the next allocation placement-news into
  // it), so the checkpoint seal has to reopen its page, and the seal protects whole
  // CHUNKS, which is coarser than the slots it means to freeze. Snapshot, not a
  // view: taken under the pool lock.
  std::vector<SlotIndex> reusable_slots() const;

  std::size_t slot_size() const noexcept { return d_slot_size; }
  std::size_t slot_stride() const noexcept { return d_slot_stride; }
  std::size_t slot_align() const noexcept { return d_slot_align; }
  std::uint32_t chunk_bits() const noexcept { return d_chunk_bits; }

  // The store-owned refcount cell for physical slot `index` (the inside-out
  // parallel column shared by every typed view over this size class). Backed by
  // the chunk minted in lock-step with the slot's data chunk, so it is always
  // published for any live index; never touches a data page. Any thread (a
  // per-slot atomic). This is the hot-path accessor `pool.refs` retain/release
  // read and write.
  std::atomic<std::uint32_t>& count_ref(SlotIndex index) const noexcept {
    const std::uint32_t chunk_number = index >> d_chunk_bits;
    const std::uint32_t slot_in_chunk = index & d_slot_mask;
    return d_refcounts.chunk(chunk_number)[slot_in_chunk];
  }

#ifndef NDEBUG
  // The store-owned generation tag for physical slot `index` (debug only). Bumped
  // when the slot is reclaimed so a stale typed reference faults on resolution --
  // and because the column is store-owned, a bump made through one typed view is
  // visible to a stale reference held through another view of the same slot.
  std::atomic<std::uint32_t>& generation_ref(SlotIndex index) const noexcept {
    const std::uint32_t chunk_number = index >> d_chunk_bits;
    const std::uint32_t slot_in_chunk = index & d_slot_mask;
    return d_generations.chunk(chunk_number)[slot_in_chunk];
  }
#endif

  // Per-store accounting (doc 15 debug discipline; hosts want a memory panel).
  // ANY THREAD: every one of these reads a relaxed atomic (or takes a lock), so a
  // host panel may poll them while the writer allocates (pool.stats_counter_race).
  // Each reads a value the counter actually held; the SET of them is not a
  // coherent snapshot, and no correctness decision may be made from one.
  // `slots_live` is exact (atomic: the writer's `allocate` and a cross-thread
  // `release` both mutate it).
  std::size_t slots_live() const noexcept { return d_slots_live.load(std::memory_order_relaxed); }
  std::size_t slots_capacity() const noexcept {
    return d_slots_capacity.load(std::memory_order_relaxed);
  }
  // BEST-EFFORT (pool.free_pools): the size of the SHARED global pool only, under
  // the pool lock. Slots cached in per-thread local pools are not counted -- they
  // are not globally visible. A diagnostic counter, never an invariant source.
  std::size_t free_slots() const noexcept {
    std::lock_guard<std::mutex> guard(d_pool_mutex);
    return d_free.size();
  }
  std::size_t bytes_reserved() const noexcept {
    return d_bytes_reserved.load(std::memory_order_relaxed);
  }

  // Behavioral counters (doc 16 `:54-62`): how many times a local pool spilled a
  // batch to, or refilled a batch from, the shared global pool -- i.e. how many
  // times the global lock was taken for a slot round-trip. Both stay put across a
  // sub-batch churn burst (proving the hot path takes no global lock) and advance
  // only when a local pool crosses the batch threshold. Any thread.
  std::size_t spill_count() const noexcept { return d_spill_count.load(std::memory_order_relaxed); }
  std::size_t refill_count() const noexcept {
    return d_refill_count.load(std::memory_order_relaxed);
  }

private:
  void assert_writer_thread() noexcept;

  // The calling thread's local free pool for this store, created (and registered
  // in `d_local_pools` under the lock) on first touch. The hot path after warm-up
  // is a lock-free thread-local lookup.
  SlotFreePool& local_pool();

  // Push a freed slot onto the calling thread's local pool (no lock, no
  // allocation); when the local pool fills, spill the whole batch to the global
  // pool under the lock. The shared release/free_now tail.
  void push_free(SlotIndex index);

  // Move up to a batch of slots from the shared global pool into `local` (LIFO
  // order preserved) under the lock; a no-op when the global pool is empty.
  void refill_from_global(SlotFreePool& local);

  // Publish the refcount (and, in debug, generation) chunk covering
  // `chunk_number` if it is not yet backed. Idempotent. Called in lock-step with
  // data-chunk growth (allocate / reserve_restored) so the columns can never be
  // short of the data directory. Writer-only. The refcount chunk routes through
  // the optional `d_refcount_backing`; the generation chunk is always `new[]`.
  void publish_parallel_columns(std::uint32_t chunk_number);

  std::size_t d_slot_size;
  std::size_t d_slot_align;
  std::size_t d_slot_stride;
  std::uint32_t d_chunk_bits;
  std::uint32_t d_slot_mask;
  std::size_t d_chunk_slots;
  std::size_t d_chunk_bytes;
  ChunkSource* d_source;

  SlabDirectory<std::byte> d_directory;
  // Inside-out parallel columns, physical-slot indexed and grown in lock-step
  // with the data directory (see class comment). Anonymous, never persisted.
  SlabDirectory<std::atomic<std::uint32_t>> d_refcounts;
#ifndef NDEBUG
  SlabDirectory<std::atomic<std::uint32_t>> d_generations;
#endif
  // Null in production (portable `new[]` refcount chunks); non-null only for the
  // page-aligned, mprotectable count-column diagnostic harness.
  RefcountTableBacking* d_refcount_backing{nullptr};
  // The SHARED global free pool (LIFO), kept OUTSIDE data pages. Touched only on
  // the cold spill/refill boundary and at recovery, always under `d_pool_mutex`.
  std::vector<SlotIndex> d_free;
  // Per-thread local pools, store-owned so they outlive the threads that touch
  // them. `local_pool()` registers a thread's pool here on first touch; the hot
  // path then reaches it lock-free through a thread-local {store-id -> pool}
  // cache. Guarded by `d_pool_mutex` (registration is cold).
  std::vector<std::unique_ptr<SlotFreePool>> d_local_pools;
  mutable std::mutex d_pool_mutex; // guards d_free + d_local_pools registration
  std::uint64_t d_store_id;        // identifies this store in the thread-local cache
  // Atomic because a cross-thread `release` reads/writes it concurrently with the
  // writer-only `set_release_fence` install.
  std::atomic<ReleaseFence*> d_release_fence{nullptr};
  // The accounting trio. All relaxed atomics (pool.stats_counter_race): the writer
  // read-modify-writes them unlocked on its growth path while a host memory panel
  // loads them from its own thread. Relaxed inc/store on the writer costs the same
  // instruction the plain `++`/`+=` emitted before.
  std::atomic<SlotIndex> d_high_water{0};
  // Atomic: mutated by the writer's `allocate` and a cross-thread `release`.
  std::atomic<std::size_t> d_slots_live{0};
  std::atomic<std::size_t> d_slots_capacity{0};
  std::atomic<std::size_t> d_bytes_reserved{0};
  std::atomic<std::size_t> d_spill_count{0};  // global-pool spills (lock taken)
  std::atomic<std::size_t> d_refill_count{0}; // global-pool refills (lock taken)

#ifndef NDEBUG
  std::thread::id d_writer{};
  bool d_writer_bound{false};
#endif
};

// Instance arena owning the per-size-class stores (doc 15). Documents own
// arenas: this gives per-arena accounting, O(live buffers) teardown, and
// multi-document / plugin-lifetime ownership that cpioo's `inline static`
// storage cannot. Same-sized record types share a store (free memory
// efficiency).
class ARBC_API Arena {
public:
  Arena();                             // owns a default AnonymousChunkSource
  explicit Arena(ChunkSource& source); // borrows an external source
  // Per-store-routed arena (doc 15's arena directory): every size-class store is
  // handed its OWN ChunkSource by the router rather than sharing one source. This
  // is how a workspace-backed arena gives each store a facade serving only the
  // chunks the file records as that store's own -- so a reopen re-binds each store
  // to its own chunks instead of guessing. A store the router refuses is backed by
  // a RefusingChunkSource (it allocates nothing) rather than by a fallback that
  // could mis-route; the router carries the reason.
  explicit Arena(ChunkSourceRouter& router);
  ~Arena();
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // The store for a size class, created on first use. `chunk_bits == 0`
  // derives the chunk size from the slot size; pass nonzero to override.
  // `refcount_backing` is consulted ONLY when the store is first minted (it
  // becomes the size-class store's count-column allocator); a later call for an
  // already-existing size class ignores it -- one backing per size class.
  //
  // WRITER-ONLY, and it takes `d_stores_mutex` for the whole lookup-or-insert:
  // the aggregate walks below run on a HOST thread (a memory panel polls them),
  // and a red-black insert rewrites the link and colour words such a walk is
  // reading. That hazard is a tree rebalance, not a torn word, so no per-counter
  // atomic can close it -- only this lock can (pool.stats_counter_race).
  //
  // The returned `SlotStore&` is used after the lock drops. That is legal ONLY
  // because `d_stores` is a `std::map`: its nodes -- and so the `SlotStore`s the
  // node values own -- are address-stable across later inserts. The safety of
  // this whole scheme rests on that container choice; an `unordered_map` here
  // would rehash and invalidate the reference, and the lock would not save it.
  SlotStore& store_for(std::size_t slot_size, std::size_t slot_align, std::uint32_t chunk_bits = 0,
                       RefcountTableBacking* refcount_backing = nullptr);

  // Arena aggregates -- the host memory panel's read surface (doc 15 `:186-198`).
  // ANY THREAD: each takes `d_stores_mutex` (so the walk cannot race a `store_for`
  // insert) and sums relaxed atomic per-store counters. Deliberately weak: each
  // counter contributes a value it actually held, but the SUM is not a coherent
  // snapshot across stores -- a store may grow after it is visited. Diagnostics,
  // never an invariant source.
  std::size_t store_count() const noexcept {
    std::lock_guard<std::mutex> guard(d_stores_mutex);
    return d_stores.size();
  }
  std::size_t total_slots_live() const noexcept;
  std::size_t total_high_water() const noexcept;
  std::size_t total_bytes_reserved() const noexcept;

  // Visit every store in the arena (the checkpoint seal walks these). Writer-only,
  // but it holds `d_stores_mutex` across the walk for the same reason the
  // aggregates do -- `fn` must not call back into `store_for` on this arena.
  template <class Fn> void for_each_store(Fn&& fn) {
    std::lock_guard<std::mutex> guard(d_stores_mutex);
    for (auto& entry : d_stores) {
      fn(*entry.second);
    }
  }

private:
  std::unique_ptr<ChunkSource> d_owned_source; // non-null only for the default
  RefusingChunkSource d_refusing;              // backs a store the router refused to bind
  ChunkSource* d_source;
  ChunkSourceRouter* d_router{nullptr}; // non-null only for the routed constructor
  // A `std::map`, NOT an `unordered_map`: node stability is what lets `store_for`
  // hand out a `SlotStore&` that outlives the lock (see `store_for`).
  std::map<std::pair<std::size_t, std::size_t>, std::unique_ptr<SlotStore>> d_stores;
  // Guards `d_stores` against the writer's cold `store_for` insert racing a host
  // thread's aggregate walk. Cold on both sides: an insert happens once per
  // size class, a walk at a memory panel's frame cadence -- so it is uncontended
  // in practice, and it is NOT on the writer's per-slot `allocate` path.
  mutable std::mutex d_stores_mutex;
};

} // namespace arbc
