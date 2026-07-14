// The incremental-save hash memo (serialize.raster_tile_store Decision 5), driven against
// a bare `BigBlockPool` -- no raster type, no document. That is deliberate: the property
// under test is a POOL property, and the pool is where the hazard lives.
//
// THE HAZARD. `BlockSlotRef` is `{uint32 index, uint32 size}` in release builds -- the
// generation tag is `#ifndef NDEBUG`-gated -- and `SlotStore::allocate` reuses the
// most-recently-released slot first. So a freed slot re-allocated at the same size class
// yields a ref BIT-IDENTICAL to the stale one, and neither `peek()` nor `count()` can tell
// them apart. A memo keyed on the ref alone would hand back a stale hash for entirely
// different pixels -- silent corruption of the user's painting, in release builds ONLY,
// which is exactly the class of bug a debug-only generation tag cannot protect against.
//
// THE FIX, and what this file pins: the memo holds an OWNING `BlockRef` per entry, so a
// memoized slot cannot be recycled while the entry lives. The refcount IS the validity
// token.

#include <arbc/pool/big_block_pool.hpp>
#include <arbc/runtime/raster_tile_store.hpp>
#include <arbc/serialize/tile_blob.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

using namespace arbc;

namespace {

constexpr std::size_t k_samples = 16;                        // 4 pixels x 4 channels
constexpr std::size_t k_blob = k_samples * sizeof(float);    // one tiny "tile"

// Allocate a blob filled with `value` in every float, and return a ref holding one count
// (the caller stands in for the `TileTable` that would normally hold it).
BlockRef make_blob(BigBlockPool& pool, float value) {
  BlockRef ref = *pool.allocate(k_blob);
  std::vector<float> f(k_samples, value);
  std::memcpy(ref.data(), f.data(), k_blob);
  return ref;
}

} // namespace

TEST_CASE("the memo hashes on a miss and carries the hash forward on a hit") {
  BigBlockPool pool;
  RasterTileStore memo;
  const BlockRef a = make_blob(pool, 0.25F);

  memo.begin_pass(PixelFormat::Rgba32fLinearPremul);
  const std::string h1 = memo.hash_of(pool, a.slot());
  CHECK(memo.tiles_hashed() == 1);
  CHECK(is_tile_hash(h1));

  // The same slot again WITHIN the pass: already carried, no second hash. This is the
  // all-identical-layer case, where one slot is named many times in one grid.
  CHECK(memo.hash_of(pool, a.slot()) == h1);
  CHECK(memo.tiles_hashed() == 1);
  memo.end_pass();
  CHECK(memo.memoized() == 1);

  // A SECOND pass over the same untouched slot: a hit. Zero hashes. This is the whole
  // incremental-save win -- an untouched tile keeps its `BlockSlotRef` across a CoW paint.
  memo.begin_pass(PixelFormat::Rgba32fLinearPremul);
  CHECK(memo.hash_of(pool, a.slot()) == h1);
  CHECK(memo.tiles_hashed() == 1); // still 1: not re-hashed
  memo.end_pass();
}

// The soundness property Decision 5 exists for. If this regressed, a release build would
// silently serialize one tile's pixels under another tile's name.
TEST_CASE("a memoized slot cannot be recycled out from under its key") {
  BigBlockPool pool;
  RasterTileStore memo;

  BlockSlotRef slot;
  std::string hash;
  {
    const BlockRef a = make_blob(pool, 1.0F);
    slot = a.slot();
    memo.begin_pass(PixelFormat::Rgba32fLinearPremul);
    hash = memo.hash_of(pool, slot);
    memo.end_pass();
    // `a` drops here: the caller's count goes away. The MEMO's pin does not.
  }

  // The memo still holds a count, so the slot is NOT free and the pool cannot hand it back
  // out. A fresh allocation at the same size class therefore gets a DIFFERENT slot -- which
  // is what makes the memo's key unambiguous.
  CHECK(pool.count(slot) >= 1);
  const BlockRef other = make_blob(pool, 2.0F);
  CHECK(other.slot() != slot);

  // And the memoized hash still describes the bytes actually behind `slot`.
  memo.begin_pass(PixelFormat::Rgba32fLinearPremul);
  CHECK(memo.hash_of(pool, slot) == hash);
  const std::string other_hash = memo.hash_of(pool, other.slot());
  CHECK(other_hash != hash); // different pixels, different name
  memo.end_pass();
}

TEST_CASE("end_pass trims the memo to the version just saved") {
  BigBlockPool pool;
  RasterTileStore memo;
  const BlockRef a = make_blob(pool, 0.5F);
  const BlockRef b = make_blob(pool, 0.75F);

  memo.begin_pass(PixelFormat::Rgba32fLinearPremul);
  static_cast<void>(memo.hash_of(pool, a.slot()));
  static_cast<void>(memo.hash_of(pool, b.slot()));
  memo.end_pass();
  CHECK(memo.memoized() == 2);

  // A pass naming only `a` -- `b` is no longer in the saved version. The swap-and-drop
  // releases `b`'s pin, so the memo self-trims to exactly the last-saved document's tiles.
  memo.begin_pass(PixelFormat::Rgba32fLinearPremul);
  static_cast<void>(memo.hash_of(pool, a.slot()));
  memo.end_pass();
  CHECK(memo.memoized() == 1);
}

TEST_CASE("the storage format is part of the memo key") {
  BigBlockPool pool;
  RasterTileStore memo;
  const BlockRef a = make_blob(pool, 0.3F);

  memo.begin_pass(PixelFormat::Rgba32fLinearPremul);
  const std::string h32 = memo.hash_of(pool, a.slot());
  memo.end_pass();

  // The SAME slot at a different storage format is different CONTENT -- the hash is over
  // storage bytes. Aliasing the two would name one tile's f16 bytes with its f32 hash.
  memo.begin_pass(PixelFormat::Rgba16fLinearPremul);
  const std::string h16 = memo.hash_of(pool, a.slot());
  memo.end_pass();

  CHECK(h32 != h16);
  CHECK(memo.tiles_hashed() == 2); // a miss, not a hit: the key differs
}

TEST_CASE("seed lands in the live memo so the next pass hits it") {
  BigBlockPool pool;
  RasterTileStore memo;
  const BlockRef a = make_blob(pool, 0.6F);

  // The LOAD path: we already know this tile's name, so re-hashing it would be pure waste.
  const std::string known = "0123456789abcdef0123456789abcdef";
  memo.seed(pool, a.slot(), PixelFormat::Rgba32fLinearPremul, known);
  CHECK(memo.memoized() == 1);
  CHECK(memo.tiles_hashed() == 0);

  memo.begin_pass(PixelFormat::Rgba32fLinearPremul);
  CHECK(memo.hash_of(pool, a.slot()) == known);
  CHECK(memo.tiles_hashed() == 0); // seeded, so never hashed
  memo.end_pass();

  // Seeding twice is idempotent (a shared tile named by two layers).
  memo.seed(pool, a.slot(), PixelFormat::Rgba32fLinearPremul, known);
  CHECK(memo.memoized() == 1);
}

TEST_CASE("end_pass outside a pass is inert") {
  RasterTileStore memo;
  memo.end_pass(); // no pass was begun: nothing to swap, and nothing to fault on
  CHECK(memo.memoized() == 0);
  CHECK(memo.tiles_hashed() == 0);
}
