// Runtime binding of org.arbc.raster's editable state (`kinds.raster_runtime_binding`,
// docs 03:113-118, 14:133-152/167-190, 17:66-72). The production closure of the
// wiring `src/kind_raster/t/raster_paint.t.cpp` drives by hand: a raster
// instantiated through the ordinary `Document::add_content` path participates in
// the document's history with NO `set_*_sink` call anywhere in this file.
//
// Cross-component (kind_raster + runtime), so it lives in tests/ -- a
// src/runtime/t/ TU may not include <arbc/kind_raster/...>: the doc-17 table
// keeps `runtime` free of a concrete-kind edge, and the binding itself reaches
// the content only through the `contract` Editable facet (which is the whole
// point -- it is dlopen-safe and kind-agnostic). tests/ is exempt from that scan.
// The kind-agnostic protocol assertions live in src/runtime/t/editable_binding.t.cpp.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/offline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using namespace arbc;

// A flat 4x4 opaque-white premultiplied rgba32f buffer; tile edge 2 gives a 2x2
// grid of level-0 tiles, so a one-tile paint has three untouched tiles to share.
DecodedImage white_4x4() {
  DecodedImage img;
  img.width = 4;
  img.height = 4;
  img.format = k_working_rgba32f;
  const std::vector<float> f(64, 1.0F);
  const auto* src = reinterpret_cast<const std::byte*>(f.data());
  img.bytes.assign(src, src + f.size() * sizeof(float));
  return img;
}

const WorkingPixel k_red{1.0F, 0.0F, 0.0F, 1.0F};
const WorkingPixel k_white{1.0F, 1.0F, 1.0F, 1.0F};

std::array<float, 4> pixel(const Surface& surface, int x, int y) {
  const std::span<const float> data = surface.span<PixelFormat::Rgba32fLinearPremul>();
  const std::size_t at =
      4 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(surface.width()) +
           static_cast<std::size_t>(x));
  return {data[at], data[at + 1], data[at + 2], data[at + 3]};
}

} // namespace

// The literal deliverable: add a raster through the normal runtime path, paint
// it, and get journaled/undoable/budgeted state -- host wires nothing.
//
// enforces: 14-data-model-and-editing#editable-runtime-bound
TEST_CASE(
    "a raster added to a Document is journaled, budgeted and undoable with no manual wiring") {
  const auto raster = std::make_shared<RasterContent>(white_4x4(), /*tile_edge=*/2);
  RasterStore& store = raster->store();
  const StateHandle base = raster->base_handle();

  // Blob accounting taken BEFORE the content is bound: the binding itself is inert
  // w.r.t. pixel storage -- registering sinks and capturing the initial handle
  // allocate no tile blob (doc 16 counter witness).
  const std::uint64_t blobs_before_bind = store.blobs_allocated();

  Document doc;
  const ObjectId cid = doc.add_content(raster, /*kind=*/1);

  CHECK(store.blobs_allocated() == blobs_before_bind);

  // The minted record names the raster's real initial state (not the inert handle
  // `model.content_binding` left behind), and the binding retained it exactly once.
  CHECK(doc.pin()->content_state(cid) == base);
  CHECK(store.version_refcount(base) == 1);

  // Paint under transactional discipline (doc 03:152-158), through the document's
  // own transaction seam. The runtime-registered StateRefSink retains the captured
  // version; the runtime-registered StateCostFn prices it into the journal budget.
  {
    auto txn = doc.transact("paint");
    raster->paint(txn, cid, Rect{0.0, 0.0, 2.0, 2.0}, k_red);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();

  const StateHandle painted = raster->base_handle();
  REQUIRE(painted != base);

  // The published version's handle was retained EXACTLY once, by the binding.
  CHECK(store.version_refcount(painted) == 1);
  CHECK(doc.pin()->content_state(cid) == painted);

  // The cost fn was consulted: the journal's budget carries the tile-table byte
  // cost, not just record sizes (doc 14:120-122).
  CHECK(store.state_cost(painted) > 0);
  CHECK(doc.journal().byte_cost() >= store.state_cost(painted));

  // The paint allocated the touched tile's blob (+ the mips above it) and nothing
  // else; binding contributed zero allocations of its own.
  CHECK(store.blobs_allocated() - blobs_before_bind == 3);

  // Undo runs through the document's Journal and rebases the live content through
  // the runtime-registered RestoreSink: the pre-paint pixels are back.
  REQUIRE(doc.journal().undo());
  CHECK(raster->base_handle() == base);
  CHECK(doc.pin()->content_state(cid) == base);
  CHECK(store.base_table()->pixel(0, 0, 0) == k_white);

  // ... and redo re-applies it, symmetric.
  REQUIRE(doc.journal().redo());
  CHECK(raster->base_handle() == painted);
  CHECK(store.base_table()->pixel(0, 0, 0) == k_red);
}

// Render purity across an edit (doc 14:181-190): a version pinned before a paint
// still resolves the state it was published with, and the binding does NOT release
// that state out from under the pin -- an in-flight render worker reading the
// pinned handle keeps seeing its own tile blobs.
//
// enforces: 14-data-model-and-editing#editable-runtime-bound
TEST_CASE("a pinned raster version survives a later paint under the runtime binding") {
  const auto raster = std::make_shared<RasterContent>(white_4x4(), /*tile_edge=*/2);
  RasterStore& store = raster->store();

  Document doc;
  const ObjectId cid = doc.add_content(raster, 1);

  {
    auto txn = doc.transact("paint1");
    raster->paint(txn, cid, Rect{0.0, 0.0, 2.0, 2.0}, k_red);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();
  const StateHandle v1 = raster->base_handle();

  // Pin the version v1 was published with -- what a render worker holds.
  const DocStatePtr pinned = doc.pin();
  REQUIRE(pinned->content_state(cid) == v1);

  {
    auto txn = doc.transact("paint2");
    raster->paint(txn, cid, Rect{2.0, 2.0, 4.0, 4.0}, k_red);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();
  const StateHandle v2 = raster->base_handle();
  REQUIRE(v2 != v1);

  // The pin is frozen at v1 and v1 is still fully resolvable: its tile blobs were
  // not reclaimed by the newer paint, so the pinned read stays valid.
  CHECK(pinned->content_state(cid) == v1);
  CHECK(doc.pin()->content_state(cid) == v2);
  REQUIRE(store.resolve(v1));
  CHECK(store.resolve(v1)->pixel(0, 3, 3) == k_white); // v1's own pixels, untouched by paint2
  CHECK(store.version_refcount(v1) == 1);
  CHECK(store.resolve(v2));
}

// The release half of the lifecycle (doc 14:173-176): tearing the document down
// reclaims the content's records, and each reclaim releases its retained version
// through the still-live binding -- exactly once. The raster outlives the document
// here (this test holds the shared_ptr), so its counters survive to be read.
//
// enforces: 14-data-model-and-editing#editable-runtime-bound
TEST_CASE("document teardown releases the raster's pinned versions by refcount") {
  const auto raster = std::make_shared<RasterContent>(white_4x4(), /*tile_edge=*/2);
  RasterStore& store = raster->store();

  StateHandle v1{};
  StateHandle v2{};
  {
    Document doc;
    const ObjectId cid = doc.add_content(raster, 1);

    {
      auto txn = doc.transact("paint1");
      raster->paint(txn, cid, Rect{0.0, 0.0, 2.0, 2.0}, k_red);
      REQUIRE(txn.commit().has_value());
    }
    doc.drain();
    v1 = raster->base_handle();

    {
      auto txn = doc.transact("paint2");
      raster->paint(txn, cid, Rect{2.0, 2.0, 4.0, 4.0}, k_red);
      REQUIRE(txn.commit().has_value());
    }
    doc.drain();
    v2 = raster->base_handle();

    REQUIRE(store.version_refcount(v1) == 1);
    REQUIRE(store.version_refcount(v2) == 1);
  } // ~Document: journal entries drop, records reclaim, release fires per retain

  // Every retained version was released exactly once, so neither is left pinned and
  // both version slots are gone -- their no-longer-shared tile blobs went back to
  // the pool by refcount. (The live base TABLE survives: the store pins it with its
  // own shared_ptr, independently of the model-driven slot refcount, so an unpinned
  // render of the surviving content still reads its pixels.)
  CHECK(store.version_refcount(v1) == 0);
  CHECK(store.version_refcount(v2) == 0);
  CHECK_FALSE(store.resolve(v1));
  CHECK_FALSE(store.resolve(v2));
  REQUIRE(store.base_table());
  CHECK(store.base_table()->pixel(0, 0, 0) == k_red);
}

// The inert-path control (doc 14:176-182): a non-editable kind (org.arbc.tone,
// `editable() == nullptr`) instantiated through the SAME runtime path registers
// zero sinks, so the journal's budget is bounded by record sizes alone and the
// document's behaviour is byte-identical to the pre-binding runtime.
TEST_CASE("a non-editable content registers no state sinks through the runtime path") {
  const auto raster = std::make_shared<RasterContent>(white_4x4(), /*tile_edge=*/2);

  Document doc;
  const ObjectId tone = doc.add_content(std::make_shared<ToneContent>(440, 0.5F), /*kind=*/2);

  // Tone exposes no `Editable` facet, so the runtime registered nothing for it: its
  // record keeps the inert handle and its journal entry is priced by record size
  // alone (there is no StateCostFn to ask).
  REQUIRE(doc.resolve(tone) != nullptr);
  CHECK(doc.resolve(tone)->editable() == nullptr);
  CHECK_FALSE(doc.pin()->find_content(tone)->state.has_state());

  const std::size_t inert_cost = doc.journal().byte_cost();
  CHECK(inert_cost > 0); // record sizes only

  // Painting a raster into the SAME document does price its state in, and the budget
  // then carries the tile-table bytes on top -- the contrast that shows the tone path
  // was inert, not merely cheap. (The document's sink trio is installed either way:
  // what keeps the tone path inert is its handle naming no state, not the absence of
  // a coster.)
  const ObjectId cid = doc.add_content(raster, /*kind=*/1);
  {
    auto txn = doc.transact("paint");
    raster->paint(txn, cid, Rect{0.0, 0.0, 2.0, 2.0}, k_red);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();

  const std::size_t state_cost = raster->store().state_cost(raster->base_handle());
  CHECK(state_cost > 0);
  CHECK(doc.journal().byte_cost() >= inert_cost + state_cost);
}

// The failure mode the multiplex exists to prevent (`runtime.editable_sink_multiplex`),
// pinned at the kind level where it is destructive rather than merely wrong.
//
// Since `kinds.raster_pool_backing`, a raster `StateHandle` transitively OWNS its
// tile blobs: `Editable::release` -> `RasterStore::release_version` -> a cascade of
// `BigBlockPool::release`. Two rasters in one document hold COLLIDING slot indices
// (each store numbers its own versions from 0, so both sit at slot 0, then both at
// slot 1) -- so a release routed by handle alone, as the pre-multiplex seams were
// forced to do, would free the WRONG content's pixels. This is the test that would
// have caught a naive multiplex.
//
// enforces: 14-data-model-and-editing#state-release-never-crosses-contents
TEST_CASE("releasing one raster's version leaves the other raster's tile blobs intact") {
  const auto a = std::make_shared<RasterContent>(white_4x4(), /*tile_edge=*/2);
  const auto b = std::make_shared<RasterContent>(white_4x4(), /*tile_edge=*/2);
  RasterStore& a_store = a->store();
  RasterStore& b_store = b->store();

  Document doc;
  const ObjectId ia = doc.add_content(a, /*kind=*/1);
  const ObjectId ib = doc.add_content(b, /*kind=*/1);

  // Both bases are slot 0: the handles are EQUAL by value, in different stores.
  const StateHandle a_base = a->base_handle();
  const StateHandle b_base = b->base_handle();
  REQUIRE(a_base == b_base);

  // Interleave the paints so both contents climb the same slot sequence in lockstep.
  {
    auto txn = doc.transact("paint-a");
    a->paint(txn, ia, Rect{0.0, 0.0, 2.0, 2.0}, k_red);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();
  {
    auto txn = doc.transact("paint-b");
    b->paint(txn, ib, Rect{0.0, 0.0, 2.0, 2.0}, k_red);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();

  const StateHandle a_v1 = a->base_handle();
  const StateHandle b_v1 = b->base_handle();
  REQUIRE(a_v1 == b_v1); // colliding again, one level up
  REQUIRE(a_v1 != a_base);
  REQUIRE(a_store.version_refcount(a_v1) == 1);
  REQUIRE(b_store.version_refcount(b_v1) == 1);

  const std::uint64_t a_blobs = a_store.blobs_allocated();

  // Now force a release of B's painted version ALONE, inside the live document: undo
  // the "paint-b" entry, then commit a fresh non-coalescing edit, which drops the redo
  // tail. That destroys the "paint-b" journal entry, so the record embedding b_v1
  // reaches zero count and reclaims -- firing exactly one release, named `ib`.
  REQUIRE(doc.journal().undo());
  {
    auto txn = doc.transact("paint-a-again");
    a->paint(txn, ia, Rect{2.0, 2.0, 4.0, 4.0}, k_red);
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();

  // B's painted version is gone: released once, its tile blobs back in B's pool.
  CHECK(b_store.version_refcount(b_v1) == 0);
  CHECK_FALSE(b_store.resolve(b_v1));

  // ...and A's identically-valued version is UNTOUCHED. This is the whole claim: the
  // release named `ib`, so it reached B's store and only B's store. Had it been routed
  // by the bare handle it would have freed A's slot-1 tile blobs, and A's still-live
  // version would now resolve to freed pixels -- or not resolve at all.
  REQUIRE(a_store.resolve(a_v1));
  CHECK(a_store.resolve(a_v1)->pixel(0, 0, 0) == k_red);   // A's painted tile
  CHECK(a_store.resolve(a_v1)->pixel(0, 3, 3) == k_white); // A's untouched tile
  CHECK(a_store.version_refcount(a_v1) == 1);
  CHECK(a_store.blobs_allocated() >= a_blobs); // A's pool never handed blocks back

  // B's own live base survived the release of its superseded version.
  REQUIRE(b_store.base_table());
  CHECK(b_store.base_table()->pixel(0, 0, 0) == k_white); // rebased by the undo

  CHECK(doc.editable_binding().unrouted_state_calls() == 0);
}

// Byte-exact goldens (doc 16): the multiplex is a state-routing change, and the
// render path is untouched -- so a scene with TWO editable rasters in one document
// must produce exactly the pixels the same scene produces as two documents' worth,
// one editable raster each (all v1 could hold). Not "within a tolerance": identical
// floats. Any diff is a bug in the routing, because nothing else changed.
//
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("a two-editable-raster document renders byte-identical to two one-raster documents") {
  // The same construction + paint recipe, replayed per document. Rasters are 4x4 and
  // land on disjoint halves of an 8x8 viewport, so the two solo renders splice into
  // the combined one pixel-for-pixel over a transparent background.
  const auto make_a = [] {
    const auto r = std::make_shared<RasterContent>(white_4x4(), /*tile_edge=*/2);
    return r;
  };
  const auto make_b = make_a;
  const auto paint_a = [](Document& doc, const std::shared_ptr<RasterContent>& r, ObjectId id) {
    auto txn = doc.transact("paint-a");
    r->paint(txn, id, Rect{0.0, 0.0, 2.0, 2.0}, k_red);
    REQUIRE(txn.commit().has_value());
  };
  const auto paint_b = [](Document& doc, const std::shared_ptr<RasterContent>& r, ObjectId id) {
    auto txn = doc.transact("paint-b");
    r->paint(txn, id, Rect{2.0, 2.0, 4.0, 4.0}, k_red);
    REQUIRE(txn.commit().has_value());
  };

  CpuBackend backend;
  const Viewport viewport{8, 8, Affine::identity()};

  // The multiplexed document: BOTH editable rasters, one journal, one sink trio.
  Document both;
  const auto both_a = make_a();
  const auto both_b = make_b();
  const ObjectId both_ia = both.add_content(both_a, /*kind=*/1);
  const ObjectId both_ib = both.add_content(both_b, /*kind=*/1);
  both.add_layer(both_ia, Affine::identity());
  both.add_layer(both_ib, Affine::translation(4.0, 0.0));
  paint_a(both, both_a, both_ia);
  paint_b(both, both_b, both_ib);
  both.drain();
  REQUIRE(both.editable_binding().unrouted_state_calls() == 0);

  // The two single-editable documents, each exactly what v1 could express.
  Document solo_a;
  const auto only_a = make_a();
  const ObjectId solo_ia = solo_a.add_content(only_a, /*kind=*/1);
  solo_a.add_layer(solo_ia, Affine::identity());
  paint_a(solo_a, only_a, solo_ia);
  solo_a.drain();

  Document solo_b;
  const auto only_b = make_b();
  const ObjectId solo_ib = solo_b.add_content(only_b, /*kind=*/1);
  solo_b.add_layer(solo_ib, Affine::translation(4.0, 0.0));
  paint_b(solo_b, only_b, solo_ib);
  solo_b.drain();

  const auto both_out = render_offline(both, viewport, backend);
  const auto a_out = render_offline(solo_a, viewport, backend);
  const auto b_out = render_offline(solo_b, viewport, backend);
  REQUIRE(both_out.has_value());
  REQUIRE(a_out.has_value());
  REQUIRE(b_out.has_value());

  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      // Each raster owns its own half of the viewport; the rest is transparent in
      // every render, so the combined frame is the two solo frames spliced.
      const std::array<float, 4> expected = x < 4 ? pixel(**a_out, x, y) : pixel(**b_out, x, y);
      CAPTURE(x, y);
      REQUIRE(pixel(**both_out, x, y) == expected);
    }
  }
}
