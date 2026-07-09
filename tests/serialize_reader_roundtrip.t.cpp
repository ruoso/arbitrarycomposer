// serialize.reader byte-exact load->save round-trip + version-0 baseline +
// unknown-format-major rejection. The reader is the exact inverse of the canonical
// writer for core placement: loading the writer's golden string and re-serializing
// reproduces it byte-for-byte. Cross-component (drives the model + writer + a
// journal), so it lives here and links the umbrella `arbc`.

#include <arbc/base/ids.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/media/audio_block.hpp> // ChannelLayout
#include <arbc/media/audio_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/journal.hpp>
#include <arbc/model/model.hpp>
#include <arbc/serialize/load_context.hpp>
#include <arbc/serialize/reader.hpp>
#include <arbc/serialize/writer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

// The byte-canonical serialization of the writer's golden document (mirrors
// serialize_writer_golden.t.cpp:54 -- the shared canonical fixture this reader is
// the exact inverse of). A default still layer (bottom) and a fully-placed layer
// (top) exercise every omit-when-default twin on the read side.
const char* const k_golden = R"json({
  "arbc": {
    "format": 1
  },
  "composition": {
    "canvas": [
      0,
      0,
      1920,
      1080
    ],
    "layers": [
      {
        "opacity": 1.0,
        "transform": [
          1.0,
          0.0,
          0.0,
          1.0,
          0.0,
          0.0
        ],
        "visible": true
      },
      {
        "audible": false,
        "gain": 2.0,
        "opacity": 0.5,
        "span": [
          0,
          705600000
        ],
        "time_map": {
          "in": 100,
          "offset": 5,
          "rate": [
            1,
            2
          ]
        },
        "transform": [
          2.0,
          0.0,
          0.0,
          2.0,
          10.5,
          20.0
        ],
        "visible": false
      }
    ]
  }
}
)json";

// enforces: 08-serialization#load-save-round-trips-canonically
TEST_CASE("load_document then serialize_document reproduces the canonical golden byte-for-byte") {
  arbc::Model model;
  arbc::Registry registry;
  arbc::LoadContext ctx("mem://golden.arbc");

  const auto loaded = arbc::load_document(k_golden, registry, ctx, model);
  REQUIRE(loaded);

  const auto pin = model.current();
  const auto out = arbc::serialize_document(*pin);
  REQUIRE(out);
  CHECK(*out == std::string(k_golden)); // the exact inverse of the writer
}

// enforces: 08-serialization#load-save-round-trips-canonically
TEST_CASE("load_document round-trips a non-default working_space / working_audio_format") {
  // The golden omits both (they are at their doc-07 / doc-12 defaults); this pins
  // the present-side parse of working_space + working_audio_format by using the
  // writer as the oracle: writer(doc) -> load -> writer must be a fixed point.
  arbc::Model authored;
  {
    auto txn = authored.transact("cfg");
    const arbc::ObjectId comp = txn.add_composition(640.0, 480.0);
    txn.set_working_space(comp, arbc::k_working_rgba16f);
    txn.set_working_audio_format(comp, arbc::AudioFormat{44100, arbc::ChannelLayout::Mono});
    const arbc::ObjectId content = txn.add_content(1);
    const arbc::ObjectId layer = txn.add_layer(content, arbc::Affine::identity(), 0.25);
    txn.attach_layer(comp, layer);
    REQUIRE(txn.commit());
  }
  const auto authored_bytes = arbc::serialize_document(*authored.current());
  REQUIRE(authored_bytes);

  arbc::Model reloaded;
  arbc::Registry registry;
  arbc::LoadContext ctx("mem://cfg.arbc");
  REQUIRE(arbc::load_document(*authored_bytes, registry, ctx, reloaded));

  const auto reloaded_bytes = arbc::serialize_document(*reloaded.current());
  REQUIRE(reloaded_bytes);
  CHECK(*reloaded_bytes == *authored_bytes);
}

// enforces: 08-serialization#load-save-round-trips-canonically
TEST_CASE("load_document round-trips the bare envelope of a composition-less document") {
  // The inverse of the writer's bare-envelope case: a document with no
  // composition loads to an empty version-0 document and re-serializes to just the
  // format envelope (exercises the no-composition load_baseline path).
  const std::string bare = "{\n  \"arbc\": {\n    \"format\": 1\n  }\n}\n";
  arbc::Model model;
  arbc::Registry registry;
  arbc::LoadContext ctx("mem://bare.arbc");

  REQUIRE(arbc::load_document(bare, registry, ctx, model));
  CHECK(model.current()->revision() == 0);

  const auto out = arbc::serialize_document(*model.current());
  REQUIRE(out);
  CHECK(*out == bare);
}

// enforces: 08-serialization#load-installs-version-0-baseline
TEST_CASE("a load installs a version-0 baseline with an empty journal; undo is a no-op") {
  arbc::Model model;
  arbc::Journal journal(model);
  model.set_commit_sink(&journal); // a real history is watching

  arbc::Registry registry;
  arbc::LoadContext ctx("mem://golden.arbc");
  REQUIRE(arbc::load_document(k_golden, registry, ctx, model));

  // The load published the baseline at revision 0 and journaled nothing.
  CHECK(model.current()->revision() == 0);
  CHECK(journal.depth() == 0);
  CHECK_FALSE(journal.can_undo());

  // Undo after load is a no-op: it never reverts the freshly-loaded document to
  // empty (doc 14:263-264, 40-43).
  CHECK_FALSE(journal.undo());
  CHECK(model.current()->revision() == 0);

  // The loaded graph is intact -- it still re-serializes to the golden.
  arbc::ObjectId comp;
  const arbc::CompositionRecord* rec = nullptr;
  REQUIRE(model.current()->find_first_composition(comp, rec));
  const auto out = arbc::serialize_document(*model.current());
  REQUIRE(out);
  CHECK(*out == std::string(k_golden));
}

// enforces: 08-serialization#reader-rejects-unknown-format-major
TEST_CASE("load_document rejects an unknown format major with no document mutation") {
  arbc::Model model;
  arbc::Registry registry;
  arbc::LoadContext ctx("mem://future.arbc");

  const char* const future = R"json({"arbc":{"format":999},"composition":{"canvas":[0,0,8,8]}})json";
  const auto r = arbc::load_document(future, registry, ctx, model);
  REQUIRE_FALSE(r);
  CHECK(r.error().kind == arbc::ReaderError::Kind::UnknownFormatMajor);

  // The target Model is untouched: still the empty fresh document at revision 0.
  CHECK(model.current()->revision() == 0);
  arbc::ObjectId comp;
  const arbc::CompositionRecord* rec = nullptr;
  CHECK_FALSE(model.current()->find_first_composition(comp, rec));
}

} // namespace
