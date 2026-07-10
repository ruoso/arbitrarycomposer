#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/model/journal.hpp>
#include <arbc/model/model.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
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

struct NoopDamageSink final : DamageSink {
  void flush(const std::vector<Damage>&) override {}
};

struct RecordingDamageSink final : DamageSink {
  std::vector<Damage> last;
  void flush(const std::vector<Damage>& d) override { last = d; }
};

} // namespace

// Raster reads any of the three decoded input formats through media PixelTraits
// (codec-free, format-generic input, doc 17:145-153): the level-0 pixels equal
// the format's decode of the input bytes, with no backend-cpu kernel.
TEST_CASE("raster decodes each input pixel format to the working space") {
  // rgba8 sRGB straight-alpha input.
  {
    DecodedImage img;
    img.width = 2;
    img.height = 1;
    img.format = k_fast_rgba8srgb;
    const std::array<std::uint8_t, 8> px{{200, 100, 50, 255, 40, 180, 90, 128}};
    img.bytes.resize(px.size());
    std::memcpy(img.bytes.data(), px.data(), px.size());
    RasterContent content(img, /*tile_edge=*/2);
    const TileTablePtr t = content.store().base_table();
    REQUIRE(t->pixel(0, 0, 0) == PixelTraits<PixelFormat::Rgba8Srgb>::decode(&px[0]));
    REQUIRE(t->pixel(0, 1, 0) == PixelTraits<PixelFormat::Rgba8Srgb>::decode(&px[4]));
  }
  // rgba16f linear premultiplied input.
  {
    DecodedImage img;
    img.width = 2;
    img.height = 1;
    img.format = k_working_rgba16f;
    std::array<std::uint16_t, 8> px{};
    const WorkingPixel a{0.5F, 0.25F, 0.125F, 0.75F};
    const WorkingPixel b{0.9F, 0.4F, 0.2F, 1.0F};
    PixelTraits<PixelFormat::Rgba16fLinearPremul>::encode(a, &px[0]);
    PixelTraits<PixelFormat::Rgba16fLinearPremul>::encode(b, &px[4]);
    const auto* src = reinterpret_cast<const std::byte*>(px.data());
    img.bytes.assign(src, src + px.size() * sizeof(std::uint16_t));
    RasterContent content(img, /*tile_edge=*/2);
    const TileTablePtr t = content.store().base_table();
    REQUIRE(t->pixel(0, 0, 0) == PixelTraits<PixelFormat::Rgba16fLinearPremul>::decode(&px[0]));
    REQUIRE(t->pixel(0, 1, 0) == PixelTraits<PixelFormat::Rgba16fLinearPremul>::decode(&px[4]));
  }
}

// The doc-14 reference proof: a paint stroke touching tile set T shares every
// tile outside T by refcount (no copy), allocates exactly |T| level-0 blobs plus
// the mip blobs geometrically above T, and emits damage equal to T.
//
// enforces: 14-data-model-and-editing#raster-paint-copies-only-touched-tiles
TEST_CASE("a raster paint copies only the touched tiles and shares the rest by refcount") {
  // 4x4 buffer, tile edge 2 -> level 0 is a 2x2 grid of tiles; level 1 is one
  // 2x2 tile; level 2 is one 1x1 tile.
  RasterContent content(white_4x4(), /*tile_edge=*/2);
  RasterStore& store = content.store();
  const StateHandle base = content.base_handle();
  const TileTablePtr base_table = store.resolve(base);
  REQUIRE(base_table);
  REQUIRE(base_table->level(0).tiles_x == 2);
  REQUIRE(base_table->level(0).tiles_y == 2);

  const std::uint64_t blobs_before = store.blobs_allocated();

  // Paint the single top-left level-0 tile (pixels [0,2) x [0,2)).
  Rect touched{};
  const StateHandle after = store.paint(base, Rect{0.0, 0.0, 2.0, 2.0}, k_red, touched);
  const TileTablePtr after_table = store.resolve(after);
  REQUIRE(after_table);

  // Exactly |T| = 1 level-0 blob + the mip blobs above it (level 1 tile + level 2
  // tile) = 3 new blobs.
  REQUIRE(store.blobs_allocated() - blobs_before == 3);

  // The three untouched level-0 tiles keep their blob identity (shared, not
  // copied); only the touched tile [index 0] got a fresh blob.
  const Level& b0 = base_table->level(0);
  const Level& a0 = after_table->level(0);
  REQUIRE(a0.tiles[0].get() != b0.tiles[0].get()); // touched: fresh blob
  REQUIRE(a0.tiles[1].get() == b0.tiles[1].get()); // shared
  REQUIRE(a0.tiles[2].get() == b0.tiles[2].get()); // shared
  REQUIRE(a0.tiles[3].get() == b0.tiles[3].get()); // shared

  // The emitted damage equals the touched tile's rect.
  REQUIRE(touched == Rect{0.0, 0.0, 2.0, 2.0});

  // The paint actually changed the touched tile and left the rest intact.
  REQUIRE(after_table->pixel(0, 0, 0) == k_red);
  REQUIRE(after_table->pixel(0, 3, 3) == base_table->pixel(0, 3, 3));
}

// paint() rides a real transaction: it assigns the captured handle via
// set_content_state and adds the touched-tile damage to the transaction, flushed
// once at commit.
TEST_CASE("raster paint through a transaction assigns state and damages the touched tiles") {
  // Sinks are declared before the model so they outlive ~Model's drain (which
  // fires the final releases through them).
  RasterContent content(white_4x4(), /*tile_edge=*/2);
  RasterStateRefSink refsink(content.store());
  RecordingDamageSink dsink;
  Model model;
  model.set_state_ref_sink(&refsink);
  model.set_damage_sink(&dsink);

  ObjectId cid{};
  {
    auto txn = model.transact("add");
    cid = txn.add_content(1);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  {
    auto txn = model.transact("paint");
    content.paint(txn, cid, Rect{0.0, 0.0, 2.0, 2.0}, k_red);
    REQUIRE(txn.commit().has_value());
  }

  // The captured handle is the content's new live base, retained once for the
  // published record and resolvable as the current content state.
  REQUIRE(content.store().version_refcount(content.base_handle()) == 1);
  REQUIRE(model.current()->content_state(cid) == content.base_handle());

  // Exactly one damage record for the content, spanning the touched tile.
  REQUIRE(dsink.last.size() == 1);
  REQUIRE(dsink.last[0].object == cid);
  REQUIRE(dsink.last[0].rect == Rect{0.0, 0.0, 2.0, 2.0});
}

// Raster's sink wiring drives the already-landed L2 lifecycle end-to-end against
// a real Model: a pin holds the captured state, and superseding + draining
// releases the superseded version by refcount (dropping its no-longer-shared
// tile blobs).
//
// enforces: 14-data-model-and-editing#pin-holds-content-state
// enforces: 14-data-model-and-editing#content-state-reclaimed-by-refcount
TEST_CASE("a pinned raster version holds its state; superseding reclaims it by refcount") {
  RasterContent content(white_4x4(), /*tile_edge=*/2);
  RasterStateRefSink refsink(content.store());
  NoopDamageSink dsink;
  Model model;
  model.set_state_ref_sink(&refsink);
  model.set_damage_sink(&dsink);

  ObjectId cid{};
  {
    auto txn = model.transact("add");
    cid = txn.add_content(1);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();

  {
    auto txn = model.transact("paint1");
    content.paint(txn, cid, Rect{0.0, 0.0, 2.0, 2.0}, k_red);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  const StateHandle v1 = content.base_handle();
  REQUIRE(content.store().version_refcount(v1) == 1);

  // Pin the version that captured v1; a later paint leaves the pin's resolved
  // handle frozen and releases nothing.
  DocStatePtr pinned = model.current();
  REQUIRE(pinned->content_state(cid) == v1);

  {
    auto txn = model.transact("paint2");
    content.paint(txn, cid, Rect{2.0, 2.0, 4.0, 4.0}, k_red);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  const StateHandle v2 = content.base_handle();
  REQUIRE(v2 != v1);
  REQUIRE(pinned->content_state(cid) == v1);          // pinned: frozen
  REQUIRE(model.current()->content_state(cid) == v2); // live: newest
  REQUIRE(content.store().resolve(v1));               // still live: pin holds it

  // Dropping the pin (no journal installed) and draining reclaims the superseded
  // v1 record slot, which releases v1's version -> its blobs drop by refcount.
  pinned.reset();
  model.drain();
  REQUIRE(content.store().version_refcount(v1) == 0);
  REQUIRE_FALSE(content.store().resolve(v1)); // released: unreachable
  REQUIRE(content.store().resolve(v2));       // the live tip survives
}

// A coalesced capture gesture keeps only the first-before / last-after handles at
// the tip; the intermediate captured versions are released after drain.
//
// enforces: 14-data-model-and-editing#coalesced-gesture-captures-once
TEST_CASE("a coalesced raster gesture keeps only first-before and last-after") {
  RasterContent content(white_4x4(), /*tile_edge=*/2);
  RasterStateRefSink refsink(content.store());
  RasterStateCostFn costfn(content.store());
  RasterRestoreSink restoresink(content, ObjectId{});
  NoopDamageSink dsink;
  Model model;
  Journal journal(model);
  model.set_commit_sink(&journal);
  model.set_state_ref_sink(&refsink);
  journal.set_state_cost_fn(&costfn);
  journal.set_restore_sink(&restoresink);
  model.set_damage_sink(&dsink);

  ObjectId cid{};
  {
    auto txn = model.transact("add");
    cid = txn.add_content(1);
    REQUIRE(txn.commit().has_value());
  }
  restoresink.set_object(cid);
  model.drain();

  // A pre-gesture captured state (the first-before of the gesture).
  {
    auto txn = model.transact("seed");
    content.paint(txn, cid, Rect{0.0, 0.0, 2.0, 2.0}, k_red);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  const StateHandle seed = content.base_handle();
  // The registered cost fn contributed the tile-table byte cost to the journal
  // budget (doc 14:120-122), so the journal accounts more than record sizes.
  REQUIRE(journal.byte_cost() >= content.store().state_cost(seed));
  REQUIRE(content.store().state_cost(seed) > 0);

  // Three coalesced captures under one key; each publishes its own revision. Each
  // step supersedes the prior intermediate, whose record reclaims on the next
  // drain -- so the live-version count stays BOUNDED across the gesture (the
  // intermediates are released, not accumulated), the "captures once" witness.
  const CoalesceKey key = 0xBEEF;
  std::size_t live_after_first = 0;
  for (int i = 0; i < 3; ++i) {
    {
      auto txn = model.transact("drag");
      txn.coalesce(key);
      content.paint(txn, cid, Rect{2.0, 2.0, 4.0, 4.0}, k_red);
      REQUIRE(txn.commit().has_value());
    }
    model.drain();
    if (i == 0) {
      live_after_first = content.store().live_versions();
    }
  }

  // The gesture collapses to one journal entry; the live-version count did not
  // grow with the gesture length (intermediates released), and only seed
  // (first-before, held by the journal) and the live tip remain resolvable.
  REQUIRE(journal.depth() == 3); // add + seed + one coalesced gesture entry
  REQUIRE(content.store().live_versions() == live_after_first);
  REQUIRE(content.store().resolve(seed));
  REQUIRE(content.store().resolve(content.base_handle()));

  // One undo reverts the whole gesture: navigation fires the RestoreSink once for
  // the content, rebasing the live content to the pre-gesture seed handle.
  REQUIRE(journal.undo());
  REQUIRE(model.current()->content_state(cid) == seed);
  REQUIRE(content.base_handle() == seed);
}

// The Editable facet round-trips a captured state: restoring a prior handle
// rebases the live content so its render reproduces the captured pixels.
TEST_CASE("raster Editable capture/restore rebases the live base") {
  RasterContent content(white_4x4(), /*tile_edge=*/2);
  Editable* editable = content.editable();
  REQUIRE(editable != nullptr);

  const StateHandle before = editable->capture();
  REQUIRE(before == content.base_handle());
  REQUIRE(editable->state_cost(before) > 0);

  RasterStateRefSink refsink(content.store());
  NoopDamageSink dsink;
  Model model;
  model.set_state_ref_sink(&refsink);
  model.set_damage_sink(&dsink);
  ObjectId cid{};
  {
    auto txn = model.transact("add");
    cid = txn.add_content(1);
    REQUIRE(txn.commit().has_value());
  }
  {
    auto txn = model.transact("paint");
    content.paint(txn, cid, Rect{0.0, 0.0, 2.0, 2.0}, k_red);
    REQUIRE(txn.commit().has_value());
  }
  REQUIRE(content.base_handle() != before);

  // Restore the pre-paint state: the live base returns to `before`.
  editable->restore(before);
  REQUIRE(content.base_handle() == before);
  REQUIRE(content.store().base_table()->pixel(0, 0, 0) == WorkingPixel{1.0F, 1.0F, 1.0F, 1.0F});
}
