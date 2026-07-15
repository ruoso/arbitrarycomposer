// The TILEWISE build seam (kinds.raster_tilewise_load; doc 15 "Reconstructing a tiled
// payload is tilewise").
//
// `RasterStore::build_from_tiles` exists so a load never materializes a dense `w * h`
// working buffer on the way into the pool. Three properties make that real rather than
// intended, and this file pins all three at the KIND's side of the seam (the codec's side
// -- that a real document load's heap peak is O(tile) -- is `tests/raster_tilewise_load.t.cpp`):
//
//   1. the fill sees EXACTLY ONE tile at a time, in flat row-major order, at exactly one
//      blob's size -- the structural bound, asserted as a behavioral counter;
//   2. a tilewise build is BYTE-IDENTICAL to a dense build, every rung of the pyramid --
//      which is what makes the mip goldens and `raster-mips-are-not-persisted` survive the
//      route change (the higher-level decimation loop is literally the same code); and
//   3. an ABANDONED build (a fill that declines at tile k) strands nothing: the pool
//      reclaims every slot the partial build took, and the store stays reusable.

#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/pool/big_block_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

using namespace arbc;

namespace {

// A deterministic per-pixel value: distinct for every (x, y, channel), so a tile that
// lands at the wrong index, or a padding sample that leaks into a level, shows up as a
// mismatch rather than as a coincidence.
float sample_at(int x, int y, std::size_t c) {
  const std::uint32_t k =
      (static_cast<std::uint32_t>(y) * 7919U + static_cast<std::uint32_t>(x)) * 4U +
      static_cast<std::uint32_t>(c);
  const std::uint32_t s = k * 2654435761U;
  return static_cast<float>(s >> 8U) / static_cast<float>(1U << 24U);
}

// The same pixels as a dense `DecodedImage` (the route `build` takes).
DecodedImage dense_image(int w, int h) {
  DecodedImage img;
  img.width = w;
  img.height = h;
  img.format = k_working_rgba32f;
  std::vector<float> f(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * k_tile_channels);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      for (std::size_t c = 0; c < k_tile_channels; ++c) {
        f[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
           static_cast<std::size_t>(x)) *
              k_tile_channels +
          c] = sample_at(x, y, c);
      }
    }
  }
  const auto* src = reinterpret_cast<const std::byte*>(f.data());
  img.bytes.assign(src, src + f.size() * sizeof(float));
  return img;
}

int tiles_across(int extent, int edge) { return (extent + edge - 1) / edge; }

// The tilewise equivalent of `dense_image`: writes tile `index` of the same field into
// `dst`, in-bounds pixels only, leaving the right/bottom padding at the pre-zeroed value
// the dense route's re-tiling would also produce. (The codec's fill instead copies a
// persisted blob VERBATIM, padding and all -- Decision 3 -- which is what
// `tests/raster_tilewise_load.t.cpp` exercises; here the point is that the two BUILD
// routes agree, so the fill must feed the same pixels the dense grid holds.)
void fill_tile(std::size_t index, std::span<float> dst, int w, int h, int edge) {
  const int tiles_x = tiles_across(w, edge);
  const int tx = static_cast<int>(index) % tiles_x;
  const int ty = static_cast<int>(index) / tiles_x;
  for (int iy = 0; iy < edge; ++iy) {
    for (int ix = 0; ix < edge; ++ix) {
      const int gx = tx * edge + ix;
      const int gy = ty * edge + iy;
      if (gx >= w || gy >= h) {
        continue;
      }
      const std::size_t o = (static_cast<std::size_t>(iy) * static_cast<std::size_t>(edge) +
                             static_cast<std::size_t>(ix)) *
                            k_tile_channels;
      for (std::size_t c = 0; c < k_tile_channels; ++c) {
        dst[o + c] = sample_at(gx, gy, c);
      }
    }
  }
}

} // namespace

// THE STRUCTURAL BOUND, as a behavioral counter (doc 16 tier 4 -- never a wall-clock, never
// an RSS sample): the fill is handed one blob's memory at a time, so at most ONE tile's
// pixels are live at any instant, whatever the image's size.
TEST_CASE("build_from_tiles hands the fill exactly one tile at a time, in row-major order") {
  constexpr int k_w = 48;
  constexpr int k_h = 40;
  constexpr int k_edge = 16; // => a 3 x 3 grid, with right/bottom padding on the last row/col

  RasterStore store;

  int live = 0;     // spans currently inside a fill invocation
  int max_live = 0; // ...and the high-water of that
  std::vector<std::size_t> order;
  std::vector<std::size_t> sizes;

  const std::optional<StateHandle> built =
      store.build_from_tiles(k_w, k_h, k_edge, [&](std::size_t index, std::span<float> dst) {
        ++live;
        max_live = std::max(max_live, live);
        order.push_back(index);
        sizes.push_back(dst.size());
        fill_tile(index, dst, k_w, k_h, k_edge);
        --live;
        return true;
      });
  REQUIRE(built.has_value());

  // ONE tile live at a time. This is the whole memory argument: a dense build holds
  // `w * h * 4` floats across the entire level-0 loop; this holds `edge * edge * 4`.
  CHECK(max_live == 1);
  CHECK(live == 0);

  // Invoked exactly once per level-0 tile, and NOT once per pixel or once per level.
  CHECK(order.size() == 3U * 3U);

  // Each `dst` is exactly one blob: `edge * edge * 4` floats, padding included.
  const std::size_t blob_samples =
      static_cast<std::size_t>(k_edge) * static_cast<std::size_t>(k_edge) * k_tile_channels;
  CHECK(std::ranges::all_of(sizes, [&](std::size_t n) { return n == blob_samples; }));

  // Ascending flat row-major order -- the ordering the codec's `blobs` array and its hash
  // memo are both positionally keyed on (Constraint 2). A permuted fill would still build
  // a valid pyramid and would still round-trip; it would silently seed the memo with the
  // WRONG name for every tile.
  std::vector<std::size_t> expected(order.size());
  for (std::size_t t = 0; t < expected.size(); ++t) {
    expected[t] = t;
  }
  CHECK(order == expected);

  // And the table it built has the geometry the fill was indexed against.
  const TileTablePtr table = store.resolve(*built);
  REQUIRE(table);
  CHECK(table->level(0).tiles_x == 3);
  CHECK(table->level(0).tiles_y == 3);
  CHECK(table->level(0).tiles.size() == 9U);
}

// The route change is not allowed to move a single pixel of a single rung. It cannot: the
// pyramid above level 0 is decimated by the SAME function object -- `append_higher_levels`
// -- that `build` calls, unmoved, and a second copy of a byte-exact filter is precisely the
// defect `raster_content.cpp`'s own comment forbids recreating. This asserts it rather than
// asserting the code shape.
//
// enforces: 08-serialization#raster-mips-are-not-persisted
TEST_CASE("a tilewise build is byte-identical to a dense build, every rung of the pyramid") {
  constexpr int k_w = 48;
  constexpr int k_h = 40;
  constexpr int k_edge = 16;

  RasterStore dense;
  const StateHandle a = dense.build(dense_image(k_w, k_h), k_edge);
  const TileTablePtr ta = dense.resolve(a);
  REQUIRE(ta);

  RasterStore tilewise;
  const std::optional<StateHandle> b =
      tilewise.build_from_tiles(k_w, k_h, k_edge, [&](std::size_t index, std::span<float> dst) {
        fill_tile(index, dst, k_w, k_h, k_edge);
        return true;
      });
  REQUIRE(b.has_value());
  const TileTablePtr tb = tilewise.resolve(*b);
  REQUIRE(tb);

  // The whole chain -- level 0 through the 1x1 apex -- pixel for pixel.
  REQUIRE(ta->level_count() == tb->level_count());
  REQUIRE(ta->level_count() > 1); // there IS a mip chain to disagree about
  for (std::size_t l = 0; l < ta->level_count(); ++l) {
    CHECK(ta->level_pixels(l) == tb->level_pixels(l));
  }
}

// A fill that declines is a LOAD FAILURE, and a load failure must not be a leak. The
// partial build holds `k + 1` owning BlockRefs and no TileTable; dropping them returns
// every slot to the pool's class free list, so the NEXT build on the same store reuses
// them and reserves not one byte more than a store that never saw the failure.
TEST_CASE("an abandoned build releases every blob it allocated, and the store stays usable") {
  constexpr int k_w = 64;
  constexpr int k_h = 64;
  constexpr int k_edge = 16; // => a 4 x 4 grid = 16 level-0 tiles

  const auto good = [&](std::size_t index, std::span<float> dst) {
    fill_tile(index, dst, k_w, k_h, k_edge);
    return true;
  };

  // The reference: a store that only ever did the successful build.
  RasterStore reference;
  REQUIRE(reference.build_from_tiles(k_w, k_h, k_edge, good).has_value());

  // The subject: the same successful build, but AFTER an abandoned one on the same store.
  RasterStore subject;
  std::size_t seen = 0;
  const std::optional<StateHandle> abandoned =
      subject.build_from_tiles(k_w, k_h, k_edge, [&](std::size_t index, std::span<float> dst) {
        ++seen;
        if (index == 9) { // decline mid-grid, with nine blobs already allocated
          return false;
        }
        fill_tile(index, dst, k_w, k_h, k_edge);
        return true;
      });
  REQUIRE_FALSE(abandoned.has_value()); // no handle, and no TileTable was ever constructed
  CHECK(seen == 10U);                   // it stopped AT the declining tile, not after the grid
  CHECK_FALSE(subject.base_table());    // ...and did not publish a half-built base version
  CHECK(subject.live_versions() == 0U);

  // The partial build really did allocate -- this is not a vacuous reclaim.
  CHECK(subject.blobs_allocated() == 10U);

  const std::optional<StateHandle> after = subject.build_from_tiles(k_w, k_h, k_edge, good);
  REQUIRE(after.has_value());

  // THE COUNTER: the abandoned build's ten slots were RECYCLED, not stranded. Live slots and
  // reserved bytes both match the store that never failed -- had the partial build leaked,
  // `subject` would carry ten extra live slots (and, at the pool's chunk granularity,
  // likely extra reserved bytes too).
  CHECK(subject.pool().arena().total_slots_live() == reference.pool().arena().total_slots_live());
  CHECK(subject.pool().arena().total_bytes_reserved() ==
        reference.pool().arena().total_bytes_reserved());

  // ...and it really did allocate them a second time (10 abandoned + a full pyramid), which
  // is the witness that the slots came back rather than never having been taken.
  CHECK(subject.blobs_allocated() == 10U + reference.blobs_allocated());

  // The store survives the failure and the version it finally built is the right one.
  const TileTablePtr subject_table = subject.resolve(*after);
  const TileTablePtr reference_table = reference.base_table();
  REQUIRE(subject_table);
  REQUIRE(reference_table);
  for (std::size_t l = 0; l < subject_table->level_count(); ++l) {
    CHECK(subject_table->level_pixels(l) == reference_table->level_pixels(l));
  }
}

// `RasterContent::from_tiles` must never hand back a half-built content: a declined fill
// yields `nullptr`, not an object whose base version names no pixels.
TEST_CASE("RasterContent::from_tiles yields nullptr on a declined fill, and pixels otherwise") {
  constexpr int k_w = 32;
  constexpr int k_h = 32;
  constexpr int k_edge = 16;

  CHECK(RasterContent::from_tiles(k_w, k_h, k_edge,
                                  [](std::size_t, std::span<float>) { return false; }) == nullptr);

  const std::unique_ptr<RasterContent> content =
      RasterContent::from_tiles(k_w, k_h, k_edge, [&](std::size_t index, std::span<float> dst) {
        fill_tile(index, dst, k_w, k_h, k_edge);
        return true;
      });
  REQUIRE(content != nullptr);
  REQUIRE(content->bounds().has_value());
  CHECK(content->bounds()->x1 == 32.0);
  CHECK(content->bounds()->y1 == 32.0);

  // The same pixels the dense constructor would have produced, through the same store.
  const RasterContent reference(dense_image(k_w, k_h), k_edge);
  const TileTablePtr a = content->store().base_table();
  const TileTablePtr b = reference.store().base_table();
  REQUIRE(a);
  REQUIRE(b);
  REQUIRE(a->level_count() == b->level_count());
  for (std::size_t l = 0; l < a->level_count(); ++l) {
    CHECK(a->level_pixels(l) == b->level_pixels(l));
  }
}
