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

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

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

} // namespace

// The literal deliverable: add a raster through the normal runtime path, paint
// it, and get journaled/undoable/budgeted state -- host wires nothing.
//
// enforces: 14-data-model-and-editing#editable-runtime-bound
TEST_CASE("a raster added to a Document is journaled, budgeted and undoable with no manual wiring") {
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

  // Painting a raster into the SAME document (the one editable content v1 allows)
  // does register a coster, and the budget then carries the tile-table bytes on top
  // -- the contrast that shows the tone path was inert, not merely cheap.
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
