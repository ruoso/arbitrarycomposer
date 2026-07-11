#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/pool/big_block_pool.hpp>
#include <arbc/pool/chunk_source.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using namespace arbc;

// A flat 4x4 opaque-white premultiplied rgba32f buffer.
DecodedImage white_4x4() {
  DecodedImage img;
  img.width = 4;
  img.height = 4;
  img.format = k_working_rgba32f;
  std::vector<float> f(64, 1.0F);
  const auto* src = reinterpret_cast<const std::byte*>(f.data());
  img.bytes.assign(src, src + f.size() * sizeof(float));
  return img;
}

const WorkingPixel k_red{1.0F, 0.0F, 0.0F, 1.0F};

bool page_aligned(const void* p) {
  return (reinterpret_cast<std::uintptr_t>(p) & (BigBlockPool::k_page - 1)) == 0;
}

} // namespace

// The doc-15 storage-split reference proof: org.arbc.raster's tile-pixel payloads
// live in a per-content BigBlockPool (page-aligned, size-classed) while the tile
// table holds compact BlockSlotRefs in slabs; untouched tiles are shared across
// CoW versions by the pool's inside-out refcount, and dropping a no-longer-shared
// version returns its blob slots to the class free pool for reuse with no
// capacity growth.
//
// enforces: 15-memory-model#raster-tile-pixels-pool-backed
TEST_CASE("raster tile pixels are pool-backed, page-aligned, shared and reclaimed by refcount") {
  // Exercise the ChunkSource injection point (Decision 3): the runtime supplies
  // its own source; the store builds its dedicated pool over it.
  AnonymousChunkSource source;
  RasterStore store(source);

  const int edge = 2; // 4x4 buffer, edge 2 -> level 0 is a 2x2 tile grid.
  const StateHandle base = store.build(white_4x4(), edge);
  const TileTablePtr base_table = store.resolve(base);
  REQUIRE(base_table);
  REQUIRE(base_table->level(0).tiles_x == 2);
  REQUIRE(base_table->level(0).tiles_y == 2);

  BigBlockPool& pool = store.pool();

  // (a) A resolved tile handle peeks to exactly edge*edge*4*sizeof(float)
  // page-aligned bytes, and the base version holds one pool count on each blob.
  const std::size_t expected_bytes = static_cast<std::size_t>(edge) *
                                     static_cast<std::size_t>(edge) * k_tile_channels *
                                     sizeof(float);
  const BlockSlotRef tile0 = base_table->level(0).tiles[0];
  const std::span<const std::byte> blob = pool.peek(tile0);
  REQUIRE(blob.size() == expected_bytes);
  REQUIRE(page_aligned(blob.data()));
  REQUIRE(pool.count(tile0) == 1);

  // (b) A paint shares untouched blobs by a count() bump with no allocate for
  // them; only the touched tile (+ the mip blobs above it) is a fresh allocation.
  const std::uint64_t blobs_before = store.blobs_allocated();
  Rect touched{};
  const StateHandle v1 = store.paint(base, Rect{0.0, 0.0, 2.0, 2.0}, k_red, touched);
  TileTablePtr v1_table = store.resolve(v1);
  REQUIRE(v1_table);

  // |T| = 1 level-0 blob + level-1 tile + level-2 tile = 3 new allocations.
  REQUIRE(store.blobs_allocated() - blobs_before == 3);
  REQUIRE(v1_table->level(0).tiles[0] != base_table->level(0).tiles[0]); // touched: fresh slot
  for (std::size_t i = 1; i < 4; ++i) {
    const BlockSlotRef shared = base_table->level(0).tiles[i];
    REQUIRE(v1_table->level(0).tiles[i] == shared); // same pool slot, shared
    REQUIRE(pool.count(shared) == 2);               // base + v1 both hold it, no new alloc
  }

  // (c) Releasing a no-longer-shared version returns its unique blob slots to the
  // class free pool and the next same-size allocation reuses them with no
  // capacity growth. Pin v1, then paint the SAME tile again so v2 REPLACES v1's
  // touched blobs at every level -> those blobs become unique to v1.
  v1_table.reset(); // drop the local pin so releasing v1 can reclaim its blobs
  store.retain_version(v1);
  Rect t2{};
  const StateHandle v2 = store.paint(v1, Rect{0.0, 0.0, 2.0, 2.0}, k_red, t2);
  REQUIRE(store.resolve(v2));

  const std::size_t reserved_peak = pool.arena().total_bytes_reserved();
  const std::size_t live_peak = pool.arena().total_slots_live();

  // Drop the last reference to v1 -> its TileTable destructs and releases its
  // blobs; v1's three unique blobs (touched level-0 tile + the two mip tiles v2
  // recomputed) hit count 0 and return their slots to the free pool.
  store.release_version(v1);
  REQUIRE(store.version_refcount(v1) == 0);
  REQUIRE_FALSE(store.resolve(v1));
  REQUIRE(pool.arena().total_slots_live() == live_peak - 3);

  // The next same-size paint reuses the freed slots: live count returns to the
  // peak and reserved bytes stay flat (no new chunk).
  Rect t3{};
  const StateHandle v3 = store.paint(v2, Rect{0.0, 0.0, 2.0, 2.0}, k_red, t3);
  REQUIRE(store.resolve(v3));
  REQUIRE(pool.arena().total_slots_live() == live_peak);
  REQUIRE(pool.arena().total_bytes_reserved() == reserved_peak);
}
