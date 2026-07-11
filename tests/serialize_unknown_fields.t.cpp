// serialize.unknown_field_preservation: the rules the determinism corpus can only prove
// by byte-equality, pinned directly (doc 08 Principle 4).
//
//   * NEVER-SHADOW (doc 08:96): a preserved unknown that collides with a key the writer
//     emits LOSES. `working_space.primaries` is the live case -- the reader ignores it
//     (reader.cpp), the writer emits it -- so a document carrying a BOGUS `primaries`
//     re-saves with exactly one `primaries`, carrying the writer's value.
//   * The residual/merge core: recursion through known sub-objects, arrays and scalars
//     treated atomically, and a stash whose bytes fail to parse degrading to EMPTY rather
//     than to a fault (Constraint 10).
//   * KNOWN-KIND `params` interiors (Decision 4): an unknown key inside a registered
//     kind's `params` round-trips byte-exact, while the codec still produces the live
//     content -- and the residual is frozen at LOAD, so CLEARING a param the codec did
//     consume (org.arbc.fade's optional `params.in`) does NOT resurrect it on save.
//
// It names the serialize-internal `unknown_json.hpp` (which names nlohmann::json), so it
// links the umbrella `arbc` plus nlohmann explicitly, exactly as serialize_sharing.t.cpp
// and serialize_kind_params.t.cpp do.

#include <arbc/base/ids.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (to hold builtin_codecs() by value)
#include <arbc/serialize/unknown_fields.hpp>
#include <arbc/serialize/unknown_json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace {

using arbc::Content;
using arbc::Document;
using arbc::FadeContent;
using arbc::KindBridge;
using arbc::ObjectId;
using arbc::Registry;
using arbc::UnknownFields;
using arbc::UnknownFieldStore;
using arbc::UnknownScope;
using json = nlohmann::json;

// load(x) into a fresh Document over the built-in codecs, then save it -- the same
// content-aware inverse pair the determinism corpus drives.
std::string roundtrip(std::string_view bytes) {
  Document doc;
  KindBridge bridge;
  Registry registry;
  const arbc::expected<std::monostate, arbc::ReaderError> loaded =
      arbc::load_document(bytes, doc, bridge, registry);
  REQUIRE(loaded.has_value());
  const arbc::expected<std::string, arbc::SerializeError> saved = arbc::save_document(doc, bridge);
  REQUIRE(saved.has_value());
  return *saved;
}

// How many times `needle` occurs in `haystack` -- "exactly one `primaries` key".
std::size_t count_of(const std::string& haystack, std::string_view needle) {
  std::size_t n = 0;
  for (std::size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

// A composition whose `working_space` carries a BOGUS `primaries` beside a genuinely
// unknown sibling. `primaries` is read-ignored (so the loaded format keeps the srgb
// default) and writer-emitted, so it is an unknown at load and a known at save.
constexpr const char* k_shadowing = R"json({
  "arbc": { "format": 1 },
  "composition": {
    "canvas": [0, 0, 16, 16],
    "working_space": {
      "format": "rgba16f-linear-premul",
      "premultiplied": true,
      "primaries": "bogus-not-a-real-gamut",
      "transfer": "linear",
      "vendor_gamut": "acescg"
    },
    "layers": []
  }
})json";

// A KNOWN kind (org.arbc.solid) whose `params` carries a key its codec never consumes.
constexpr const char* k_params_unknown = R"json({
  "arbc": { "format": 1 },
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {
        "kind": "org.arbc.solid",
        "kind_version": "1",
        "opacity": 1.0,
        "params": { "color": [1.0, 0.5, 0.25, 1.0], "gamma": 2.2, "lut": [1, 2, 3] },
        "transform": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
        "visible": true
      }
    ]
  }
})json";

// An org.arbc.fade over a solid, with the OPTIONAL `params.in` present -- a key the fade
// codec genuinely consumes, so it must NOT land in the residual.
constexpr const char* k_fade_with_in = R"json({
  "arbc": { "format": 1 },
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {
        "kind": "org.arbc.fade",
        "kind_version": "1",
        "opacity": 1.0,
        "params": { "shape": "linear", "in": { "start": 0, "end": 705600000 } },
        "inputs": [
          {
            "kind": "org.arbc.solid",
            "kind_version": "1",
            "params": { "color": [1.0, 0.5, 0.25, 1.0] }
          }
        ],
        "transform": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
        "visible": true
      }
    ]
  }
})json";

} // namespace

// enforces: 08-serialization#preserved-unknown-never-shadows-known
TEST_CASE("a preserved unknown never shadows a key the writer emits") {
  const std::string out = roundtrip(k_shadowing);

  // Exactly ONE `primaries`, and it carries the WRITER's value -- the stashed
  // "bogus-not-a-real-gamut" lost the collision (doc 08:96). Preservation must never
  // become a corruption vector.
  CHECK(count_of(out, "\"primaries\"") == 1);
  CHECK(out.find("\"primaries\": \"srgb\"") != std::string::npos);
  CHECK(out.find("bogus-not-a-real-gamut") == std::string::npos);

  // The genuinely-unknown sibling inside the same known sub-object DID survive: the
  // subtraction recurses through `working_space`, and the merge recurses back into it.
  CHECK(out.find("\"vendor_gamut\": \"acescg\"") != std::string::npos);

  // And the result is a canonical fixed point -- the collision resolves the same way on
  // every subsequent save, so the loser never reappears.
  CHECK(roundtrip(out) == out);
}

// enforces: 08-serialization#preserved-unknown-never-shadows-known
TEST_CASE("the residual/merge core: recursion, atomicity, and a malformed stash") {
  constexpr std::string_view k_known[] = {"a", "b"};

  SECTION("the subtraction keeps only unnamed keys, and is by key NAME alone") {
    // `b` is a known key carrying a MALFORMED value (a string where a number belongs).
    // It stays KNOWN -- the reader's leniency substitutes the default and the bad value
    // is NOT preserved (Decision 2). Only `c` and `d` are unknown.
    const json in = json::parse(R"({"a": 1, "b": "not-a-number", "c": [1, 2], "d": {"x": 1}})");
    const json residual = arbc::unknown_residual(in, k_known);
    CHECK(residual.size() == 2);
    CHECK(residual.contains("c"));
    CHECK(residual.contains("d"));
    CHECK_FALSE(residual.contains("b"));
    // Arrays and scalars are ATOMIC: `c` is stashed whole, never partially.
    CHECK(residual["c"] == json::parse("[1, 2]"));
  }

  SECTION("the merge recurses through shared sub-objects; the known side wins outright") {
    json known = json::parse(R"({"keep": 1, "sub": {"known": "core"}, "arr": [1]})");
    const json stash =
        json::parse(R"({"keep": 99, "sub": {"known": "stale", "extra": 7}, "arr": [9], "new": 3})");
    arbc::merge_unknown(known, stash);

    CHECK(known["keep"] == 1);                 // collision: the known scalar wins
    CHECK(known["arr"] == json::parse("[1]")); // collision on an array: known wins whole
    CHECK(known["new"] == 3);                  // a fresh key is adopted
    CHECK(known["sub"]["known"] == "core");    // collision INSIDE a shared sub-object
    CHECK(known["sub"]["extra"] == 7);         // ... and the unknown sibling beside it lands
  }

  SECTION("a non-object at any seam degrades to 'nothing preserved', never to a fault") {
    // Errors stay values (Constraint 10). The core tiers always hand these functions an
    // object, but the seam is defensive: nothing here may throw or fault.
    const json arr = json::parse("[1, 2, 3]");
    CHECK(arbc::unknown_residual(arr, k_known).empty());
    CHECK(arbc::unknown_residual_at(arr, "a", k_known).empty());
    CHECK(arbc::unknown_residual_at(json::parse(R"({"a": 7})"), "a", k_known).empty());
    CHECK(arbc::unknown_residual_diff(arr, json::object()).empty());
    // A codec that produced something non-object consumed no key, so every input key is
    // residual -- preservation errs toward keeping data, never toward dropping it.
    CHECK(arbc::unknown_residual_diff(json::parse(R"({"x": 1})"), arr) ==
          json::parse(R"({"x": 1})"));
    CHECK(arbc::to_unknown_fields(arr).empty());
    CHECK(arbc::to_unknown_fields(json::object()).empty());
    json target = arr;
    arbc::merge_unknown(target, json::parse(R"({"x": 1})")); // a non-object known side
    CHECK(target == arr);                                    // untouched
  }

  SECTION("a stash whose bytes do not parse degrades to EMPTY, never to a fault") {
    json known = json::parse(R"({"keep": 1})");
    const UnknownFields garbage{"{not json at all"};
    const UnknownFields not_an_object{"[1, 2, 3]"};
    arbc::merge_unknown_fields(known, &garbage);
    arbc::merge_unknown_fields(known, &not_an_object);
    arbc::merge_unknown_fields(known, nullptr);
    CHECK(known == json::parse(R"({"keep": 1})")); // untouched; no error path taken
  }

  SECTION("the store keys by (scope, ObjectId); an empty stash is the absence of one") {
    UnknownFieldStore store;
    store.set(UnknownScope::Layer, ObjectId{7}, UnknownFields{R"({"name":"x"})"});
    store.set(UnknownScope::Content, ObjectId{7}, UnknownFields{R"({"author":"y"})"});
    CHECK(store.size() == 2); // the same id in two scopes is two distinct entries
    REQUIRE(store.find(UnknownScope::Layer, ObjectId{7}) != nullptr);
    CHECK(store.find(UnknownScope::Layer, ObjectId{7})->bytes == R"({"name":"x"})");
    CHECK(store.find(UnknownScope::Layer, ObjectId{8}) == nullptr);
    store.set(UnknownScope::Layer, ObjectId{7}, UnknownFields{});
    CHECK(store.find(UnknownScope::Layer, ObjectId{7}) == nullptr);
    CHECK(store.size() == 1);
  }
}

// enforces: 08-serialization#known-kind-params-unknowns-preserved
TEST_CASE("a known kind's unknown params interiors are preserved without disturbing it") {
  Document doc;
  KindBridge bridge;
  Registry registry;
  REQUIRE(arbc::load_document(k_params_unknown, doc, bridge, registry).has_value());

  // The codec still produced the LIVE kind -- preservation is not placeholder fallback.
  const arbc::SolidContent* solid = nullptr;
  ObjectId solid_id{};
  doc.for_each_content([&](ObjectId id, Content* c) {
    if (const auto* s = dynamic_cast<const arbc::SolidContent*>(c)) {
      solid = s;
      solid_id = id;
    }
  });
  REQUIRE(solid != nullptr);

  // The residual holds EXACTLY the keys the codec did not consume -- `color` was
  // consumed and re-emitted, so it is not in the stash (Decision 4).
  const UnknownFields* stash = doc.unknown_fields().find(UnknownScope::Content, solid_id);
  REQUIRE(stash != nullptr);
  const json parsed = json::parse(stash->bytes);
  REQUIRE(parsed.contains("params"));
  CHECK(parsed["params"].size() == 2);
  CHECK(parsed["params"]["gamma"] == 2.2);
  CHECK(parsed["params"]["lut"] == json::parse("[1, 2, 3]"));
  CHECK_FALSE(parsed["params"].contains("color"));

  // ... and both survive the save, merged back into the codec's freshly-built `params`.
  const arbc::expected<std::string, arbc::SerializeError> saved = arbc::save_document(doc, bridge);
  REQUIRE(saved.has_value());
  CHECK(saved->find("\"gamma\": 2.2") != std::string::npos);
  CHECK(saved->find("\"lut\"") != std::string::npos);
  CHECK(saved->find("\"color\"") != std::string::npos);
  CHECK(roundtrip(*saved) == *saved); // a canonical fixed point
}

// enforces: 08-serialization#known-kind-params-unknowns-preserved
TEST_CASE("clearing a param the codec DID consume does not resurrect it on save") {
  // Decision 4's rejected alternative -- diffing at SAVE time against a stashed raw
  // `params` -- would see the user-cleared `in` as "a key the codec dropped", classify it
  // as unknown, and re-emit the stale value. Freezing the residual at LOAD makes that
  // impossible: at that instant the codec's own re-serialization reproduced `in`, so `in`
  // is a CONSUMED key and never enters the stash.
  Document doc;
  KindBridge bridge;
  Registry registry;
  const arbc::CodecTable codecs = arbc::builtin_codecs();
  REQUIRE(arbc::load_document(k_fade_with_in, doc, bridge, registry).has_value());

  const arbc::expected<std::string, arbc::SerializeError> before =
      arbc::save_document(doc, bridge, codecs);
  REQUIRE(before.has_value());
  REQUIRE(before->find("\"in\"") != std::string::npos); // it loaded, and it round-trips

  // The stash names nothing: the fade codec consumed `shape` AND `in`.
  ObjectId fade_id{};
  const FadeContent* fade = nullptr;
  doc.for_each_content([&](ObjectId id, Content* c) {
    if (const auto* f = dynamic_cast<const FadeContent*>(c)) {
      fade = f;
      fade_id = id;
    }
  });
  REQUIRE(fade != nullptr);
  CHECK(doc.unknown_fields().find(UnknownScope::Content, fade_id) == nullptr);

  // Now EDIT the fade: the same content ObjectId, its optional `in` cleared. FadeParams
  // is immutable, so the edit is modelled by rebinding the snapshot entry to a fresh
  // FadeContent over the same input under the same id -- exactly what the save path would
  // see after an in-place edit.
  arbc::ContentSnapshot snap = arbc::capture_snapshot(doc, bridge);
  arbc::FadeParams cleared;
  cleared.shape = arbc::FadeShape::Linear; // `in` and `out` left nullopt
  const auto edited = std::make_shared<FadeContent>(fade->inputs()[0], cleared);
  for (arbc::ContentSnapshot::Entry& e : snap.entries) {
    if (e.content == fade) {
      snap.by_ptr.erase(e.content);
      e.content = edited.get();
      snap.by_ptr.emplace(edited.get(), static_cast<std::size_t>(&e - snap.entries.data()));
    }
  }

  const arbc::expected<std::string, arbc::SerializeError> after =
      arbc::serialize_snapshot(snap, codecs);
  REQUIRE(after.has_value());
  CHECK(after->find("\"in\"") == std::string::npos); // the cleared param stays cleared
  CHECK(after->find("\"shape\": \"linear\"") != std::string::npos);
}

// enforces: 08-serialization#unknown-fields-preserved-at-every-tier
// enforces: 08-serialization#preserved-unknown-never-shadows-known
TEST_CASE("unknown fields survive at the tiers serialize.compositions_table adds") {
  // Two NEW places an unknown sibling can land (doc 08 Principle 7 + Principle 4):
  //
  //   * beside `layers` INSIDE a `compositions` entry -- a non-root composition's own
  //     residual, which stashes under `UnknownScope::Composition` keyed by ITS OWN
  //     ObjectId (the scope already existed and was already ObjectId-keyed, so this adds
  //     no tier -- Constraint 9);
  //   * beside the core-owned `composition` field in a nesting content's body.
  //
  // The nesting kind here has no codec anywhere in this build, so the whole thing also
  // proves the missing-plugin path preserves both.
  constexpr const char* k_comp_unknowns = R"json({
  "arbc": { "format": 1 },
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      { "kind": "com.example.nest", "kind_version": "1.0", "params": { "blend": "over" },
        "composition": "1", "note": "an unknown sibling beside composition" }
    ]
  },
  "compositions": {
    "1": {
      "author": "a newer tool wrote this",
      "canvas": [0, 0, 8, 8],
      "working_space": {
        "format": "rgba16f-linear-premul",
        "premultiplied": true,
        "primaries": "bogus-not-a-real-gamut",
        "transfer": "linear"
      },
      "layers": [
        { "kind": "org.arbc.solid", "kind_version": "1",
          "params": { "color": [1.0, 0.0, 0.0, 1.0] } }
      ]
    }
  }
})json";

  const std::string canonical = roundtrip(k_comp_unknowns);
  CHECK(roundtrip(canonical) == canonical); // an idempotent canonical fixed point

  // Both new-tier unknowns survive verbatim...
  CHECK(canonical.find("\"note\": \"an unknown sibling beside composition\"") != std::string::npos);
  CHECK(canonical.find("\"author\": \"a newer tool wrote this\"") != std::string::npos);
  // ...beside the core-owned keys they neighbour, which the core still emits itself.
  CHECK(canonical.find("\"composition\": \"1\"") != std::string::npos);
  CHECK(canonical.find("\"blend\": \"over\"") != std::string::npos);
  CHECK(canonical.find("\"src\"") == std::string::npos);

  // And the never-shadow rule bites inside a `compositions` entry exactly as it does in
  // the root composition: the read-ignored-but-writer-emitted `primaries` round-trips as
  // exactly ONE key carrying the WRITER's value, never the stashed bogus one (doc 08:96).
  CHECK(count_of(canonical, "\"primaries\"") == 1);
  CHECK(canonical.find("\"primaries\": \"srgb\"") != std::string::npos);
  CHECK(canonical.find("bogus-not-a-real-gamut") == std::string::npos);
  // The child composition's own working_space really did load (it is not the default).
  CHECK(canonical.find("\"rgba16f-linear-premul\"") != std::string::npos);
}
