#pragma once

// The incremental-save hash memo for `org.arbc.raster` (serialize.raster_tile_store
// Decision 5). Lives in `runtime` (L5) because it is the one layer that may name both a
// `BigBlockPool` slot and the serialize format API; bound into the codec by CLOSURE, the
// pattern `nested_codec(loader)` / `image_codec(registry, loader)` already established.
//
// WHY A MEMO AT ALL. Without one, every save hashes every tile -- O(document) per save,
// ~1.4 GB of SHA-256 for doc 08's reference composition -- and "incremental save" would
// be a claim about blob WRITES while the CPU cost stayed linear in document size. That
// is not the gesture-cadence autosave doc 08 describes. With one, a re-save after a
// single dab hashes only the touched tiles, because `RasterStore::paint` leaves every
// untouched tile's `BlockSlotRef` bit-identical (kinds.raster_pool_backing's
// structural-sharing property).
//
// WHY THE REFCOUNT IS THE VALIDITY TOKEN, AND A GENERATION TAG IS NOT.
// A `BlockSlotRef`-keyed memo is UNSOUND on its own, and the reason is a release-build
// hazard a debug build cannot show you: the ref is `{uint32 index, uint32 size}` in
// release -- the generation tag is `#ifndef NDEBUG`-gated (`big_block_pool.hpp:49-77`) --
// and `SlotStore::allocate` reuses the MOST-RECENTLY-RELEASED slot first
// (`slot_store.cpp:190-206`). So a freed slot re-allocated at the same size class yields
// a ref that is BIT-IDENTICAL to the stale one, and neither `peek()` nor `count()` can
// tell them apart. A memo keyed on the ref alone would hand back a stale hash for
// entirely different pixels -- silent corruption of the user's painting, in release
// builds only.
//
// So the memo HOLDS AN OWNING `BlockRef` for every entry. A memoized slot therefore
// cannot be recycled while the entry lives: the pin IS the proof that the bits behind
// the key are still the bits we hashed. Each save rebuilds the memo against the version
// it just saved -- hit => carry the hash and the pin forward; miss => hash and take a
// pin -- then swaps and drops the old memo, releasing the pins of tiles no longer in the
// saved version. The memo thus pins exactly the tiles of the LAST-SAVED document
// version: bounded by one document, self-trimming, and arguably the working set you want
// resident anyway.
//
// CONCURRENCY (Constraint 10). The save runs off the editing thread on a pinned snapshot
// (doc 08: autosave never pauses editing). Every tile the save touches is already kept
// alive by the pinned `TileTablePtr`, which holds a retain on every slot it names -- so
// the memo's own `retain` is always "add a count to something already >= 1" and can never
// resurrect a dead slot. `peek()` is any-thread, `sha256`/`compress_blob` are stateless,
// and the save never allocates from the pool.

#include <arbc/base/sha256.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/pool/big_block_pool.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace arbc {

class RasterTileStore {
public:
  RasterTileStore() = default;
  RasterTileStore(const RasterTileStore&) = delete;
  RasterTileStore& operator=(const RasterTileStore&) = delete;

  // Begin a save pass. The memo being built is empty; lookups still consult the PREVIOUS
  // pass's memo, which is what makes a hit a hit.
  void begin_pass(PixelFormat storage);

  // The content hash of the tile in `ref`, under the storage format `begin_pass` was
  // given. A hit carries the previous pass's hash and pin forward without touching the
  // bytes; a miss reads the blob through `pool.peek`, converts it to storage bytes,
  // hashes, and takes its own pin. Either way the entry lands in the pass being built.
  //
  // `pool` must be the pool the ref came from, and the CALLER must already hold a count
  // on `ref` (the pinned `TileTablePtr` does). Returns the 32-char hex blob name.
  std::string hash_of(BigBlockPool& pool, BlockSlotRef ref);

  // Finish the pass: the memo being built becomes the memo, and the previous one is
  // dropped -- releasing the pins of every tile that is no longer in the saved version.
  void end_pass();

  // Seed an entry directly (the LOAD path): a tile we have just decoded from a blob whose
  // name we already know needs no re-hashing, and seeding it here is what makes the
  // reader's params-only residual re-serialize a pure memo sweep rather than a full
  // re-hash of the document on every open. Lands in the LIVE memo, so the next save's
  // pass hits it.
  void seed(BigBlockPool& pool, BlockSlotRef ref, PixelFormat storage, const std::string& hash);

  // Behavioral counter (doc 16 tier 4), and the STRONGER of the two incremental-save
  // witnesses: write-if-absent alone would give the right blob count while still
  // re-hashing the whole document on every save -- which is the difference between a
  // gesture-cadence autosave and a lie. Advances by exactly one per tile actually hashed,
  // and not on a memo hit.
  std::uint64_t tiles_hashed() const noexcept {
    return d_tiles_hashed.load(std::memory_order_relaxed);
  }

  // Live memo entries -- the number of tiles the store is currently pinning.
  std::size_t memoized() const;

private:
  // The memo key. `BlockSlotRef` has no `std::hash`, and its release-build identity is
  // exactly `{index, size}` -- which is the hazard above, not a convenience. The storage
  // format is part of the key because the hash is over STORAGE bytes: the same pixels at
  // `rgba16f` and at `rgba32f` are different content and must not alias.
  struct Key {
    std::uint32_t index{0};
    std::uint32_t size{0};
    PixelFormat storage{PixelFormat::Rgba16fLinearPremul};

    friend bool operator==(const Key&, const Key&) = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key& k) const noexcept;
  };
  struct Entry {
    std::string hash;
    BlockRef pin; // THE validity token: while this lives, the slot cannot be recycled
  };
  using Memo = std::unordered_map<Key, Entry, KeyHash>;

  mutable std::mutex d_mutex;
  Memo d_live; // the last completed pass
  Memo d_pass; // the pass being built
  bool d_in_pass{false};
  PixelFormat d_storage{PixelFormat::Rgba16fLinearPremul};
  std::atomic<std::uint64_t> d_tiles_hashed{0};
};

} // namespace arbc
