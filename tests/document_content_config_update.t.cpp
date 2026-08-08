// In-place content-config update (issue #34): rewriting one content's persisted parameters
// WITHOUT minting a new `ObjectId`.
//
// The mutator surface could mint a content and remove one, and nothing in between, so a host
// changing one field -- Consolidate rewriting an image's URI to the copy it just made inside
// the project, Relink repointing a cell at a moved file -- had to delete and re-insert. That
// mints a new id, and everything holding the old one (a selection, another composition's
// nested reference, per-cell UI state) silently names a dead id; the journal records a
// delete/add pair rather than an edit, for an operation whose whole meaning is "same picture,
// different file location".
//
// The three things the host asked for are the three things pinned here: same id, config
// changes, one journal entry that undoes -- and undoes the OBJECT, not just the record.
//
// Cross-component (tests/, linking the umbrella `arbc`): a `Document` driven with the codec
// bridge's real capture and reconstruct hooks over concrete kinds.

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/builtin_kinds.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (to hold builtin_codecs() by value)
#include <arbc/serialize/load_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

namespace {

using arbc::Document;
using arbc::KindBridge;
using arbc::ObjectId;
using arbc::Rgba;
using arbc::SolidContent;

arbc::Registry builtin_registry() {
  arbc::Registry registry;
  arbc::register_builtin_kinds(registry);
  return registry;
}

// A document wired exactly as a host wires one: the codec-backed capture hook, so records are
// self-describing (issue #19), and the codec-backed reconstruct hook, so they can be rebuilt.
struct Wired {
  arbc::CodecTable codecs = arbc::builtin_codecs();
  arbc::Registry registry = builtin_registry();
  arbc::LoadContext ctx{std::string{}};
  KindBridge bridge;
  Document doc;

  Wired() {
    doc.set_content_identity_capture(arbc::codec_identity_capture(codecs, bridge));
    doc.set_content_reconstruct(arbc::codec_content_reconstruct(codecs, registry, ctx));
  }
};

const SolidContent& solid_at(Document& doc, ObjectId id) {
  const auto* const solid = dynamic_cast<const SolidContent*>(doc.resolve(id));
  REQUIRE(solid != nullptr);
  return *solid;
}

} // namespace

// enforces: 14-data-model-and-editing#content-config-updates-in-place
TEST_CASE("a content's config is rewritten in place, keeping its ObjectId") {
  Wired w;
  const ObjectId comp = w.doc.add_composition(8.0, 8.0);
  const ObjectId cid =
      w.doc.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.0F, 0.0F, 1.0F}),
                        w.bridge.intern(SolidContent::kind_id, "1"));
  const ObjectId layer = w.doc.add_layer(cid, arbc::Affine::identity());
  w.doc.attach_layer(comp, layer);
  const std::uint64_t before_depth = w.doc.journal().depth();

  CHECK(w.doc.update_content_config(cid, R"({"color":[0.0,1.0,0.0,1.0]})", "recolour"));

  // SAME id, same record slot, same layer binding -- which is the whole point: nothing that
  // held the id has to be found and rewritten.
  CHECK(w.doc.pin()->find_content(cid) != nullptr);
  CHECK(w.doc.pin()->find_layer(layer)->content == cid);
  // ... and the object behind it really is the new config, not the old one with new params.
  CHECK(solid_at(w.doc, cid).color().g == 1.0F);
  CHECK(solid_at(w.doc, cid).color().r == 0.0F);
  // The record's persisted identity moved with it, so a save writes the new config.
  CHECK(w.doc.pin()->content_identity(cid).params.find("1.0") != std::string::npos);
  CHECK(w.doc.pin()->content_identity(cid).kind_id == SolidContent::kind_id);

  // ONE journal entry: one user action, one undo press (issue #20's rule).
  CHECK(w.doc.journal().depth() == before_depth + 1);
}

// enforces: 14-data-model-and-editing#content-config-updates-in-place
TEST_CASE("undoing a config update puts back the content, not just the params") {
  Wired w;
  const ObjectId comp = w.doc.add_composition(8.0, 8.0);
  const ObjectId cid =
      w.doc.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.0F, 0.0F, 1.0F}),
                        w.bridge.intern(SolidContent::kind_id, "1"));
  w.doc.attach_layer(comp, w.doc.add_layer(cid, arbc::Affine::identity()));
  REQUIRE(w.doc.update_content_config(cid, R"({"color":[0.0,1.0,0.0,1.0]})"));
  REQUIRE(solid_at(w.doc, cid).color().g == 1.0F);

  // The record's identity is an ordinary chunk record, so history restores it with no new
  // machinery -- but the id->Content side-map is runtime state BESIDE the model, so the live
  // object would be left behind. `Document::undo` reconciles the two.
  REQUIRE(w.doc.undo());
  CHECK(w.doc.pin()->find_content(cid) != nullptr); // same id, still there
  CHECK(solid_at(w.doc, cid).color().r == 1.0F);    // and the ORIGINAL object is back
  CHECK(solid_at(w.doc, cid).color().g == 0.0F);

  REQUIRE(w.doc.redo());
  CHECK(solid_at(w.doc, cid).color().g == 1.0F);

  // The reconcile is also callable on its own, for a host that navigates through the journal
  // directly -- and it is a no-op when nothing is stale.
  CHECK(w.doc.reconcile_content_bindings() == 0);
}

// enforces: 14-data-model-and-editing#content-config-updates-in-place
TEST_CASE("a config update refuses, publishing nothing, when it cannot rebuild") {
  Wired w;
  const ObjectId comp = w.doc.add_composition(8.0, 8.0);
  const ObjectId cid =
      w.doc.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.0F, 0.0F, 1.0F}),
                        w.bridge.intern(SolidContent::kind_id, "1"));
  w.doc.attach_layer(comp, w.doc.add_layer(cid, arbc::Affine::identity()));
  const arbc::Content* const before = w.doc.resolve(cid);
  const std::uint64_t depth = w.doc.journal().depth();

  // Params that are not a JSON object: a decline, never a guess.
  CHECK_FALSE(w.doc.update_content_config(cid, "not-json"));
  // An id that names no content record.
  CHECK_FALSE(w.doc.update_content_config(ObjectId{999999}, R"({"color":[0,0,0,1]})"));

  // Nothing published, nothing rebound, no entry appended.
  CHECK(w.doc.resolve(cid) == before);
  CHECK(w.doc.journal().depth() == depth);

  // And a document with NO reconstruct hook -- every pre-issue-#34 host -- refuses outright
  // rather than half-applying: the record must never move without its object.
  Document bare;
  const ObjectId bare_comp = bare.add_composition(8.0, 8.0);
  const ObjectId bare_cid = bare.add_content(std::make_shared<SolidContent>(Rgba{}));
  bare.attach_layer(bare_comp, bare.add_layer(bare_cid, arbc::Affine::identity()));
  CHECK_FALSE(bare.update_content_config(bare_cid, R"({"color":[0,0,0,1]})"));
  CHECK(bare.reconcile_content_bindings() == 0);
}

// enforces: 14-data-model-and-editing#content-config-updates-in-place
TEST_CASE("an operator's config is rewritten over its own live input edges") {
  // An operator is rebuilt through the same routing a reopen uses, which adopts the live
  // objects the record's input edges name -- so rewriting a fade's envelope keeps the fade
  // over the very leaf it was already over, rather than rebuilding a fade over nothing.
  Wired w;
  const ObjectId comp = w.doc.add_composition(8.0, 8.0);
  const auto leaf = std::make_shared<SolidContent>(Rgba{1.0F, 0.0F, 0.0F, 1.0F});
  const ObjectId leaf_id = w.doc.add_content(leaf, w.bridge.intern(SolidContent::kind_id, "1"));
  const ObjectId fade_id =
      w.doc.add_content(std::make_shared<arbc::FadeContent>(leaf.get(), arbc::FadeParams{}),
                        w.bridge.intern(arbc::FadeContent::kind_id, "1"));
  w.doc.attach_layer(comp, w.doc.add_layer(fade_id, arbc::Affine::identity()));

  const std::string params = w.doc.pin()->content_identity(fade_id).params;
  REQUIRE_FALSE(params.empty());
  REQUIRE(w.doc.update_content_config(fade_id, params, "re-fade"));

  const arbc::Content* const rebuilt = w.doc.resolve(fade_id);
  REQUIRE(rebuilt != nullptr);
  REQUIRE(rebuilt->inputs().size() == 1);
  CHECK(rebuilt->inputs()[0] == w.doc.resolve(leaf_id)); // the SAME leaf object, not a copy
}
