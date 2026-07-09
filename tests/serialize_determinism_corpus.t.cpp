// serialize.format_tests: the determinism / load->save round-trip CORPUS test. Where
// each serialize predecessor pinned its own hand-picked slice, this drives a corpus of
// representative canonical documents through the full inverse-and-canonical set at once
// (doc 08 Principle 5/6, doc 16 tier 3 -- byte-exact goldens, no tolerances):
//
//   * serialize(load(x)) == x   byte-exact for every canonical x, plus idempotence
//     serialize(load(serialize(load(x)))) == x;
//   * unknown-kind / unknown-field verbatim preservation through the round-trip
//     (a missing plugin never destroys data);
//   * the HAND-AUTHORED-ID normalization case serialize.sharing deferred here
//     (sharing.md:434-436): a file with non-canonical `contents` ids and unsorted keys
//     re-serializes to canonical byte-exact output with ids collapsed to first-encounter
//     ordinals -- canonicalization, not data loss.
//
// Content-bearing corpus entries drive the runtime content-aware load/save over the
// built-in codec table (Decision D4); the placement-only + bare-envelope entries drive
// the content-free serialize API (their exact inverse). Cross-component, so it links the
// umbrella `arbc` and lives under top-level tests/ (Constraint 5). Goldens are inline
// raw strings, the serialize golden convention (Decision D3).

#include <arbc/base/ids.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/serialize/load_context.hpp>
#include <arbc/serialize/reader.hpp>
#include <arbc/serialize/writer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace {

// --- content-aware round-trip over the built-in codecs (solid/tone/fade/crossfade) ---
// load(x) into a fresh Document, then save it. The two-arg save_document uses
// builtin_codecs() internally, so this is the exact inverse the goldens below were
// emitted by (document_serialize_golden / operator_codecs_golden).
std::string content_roundtrip(std::string_view bytes) {
  arbc::Document doc;
  arbc::KindBridge bridge;
  arbc::Registry registry;
  const arbc::expected<std::monostate, arbc::ReaderError> loaded =
      arbc::load_document(bytes, doc, bridge, registry);
  REQUIRE(loaded.has_value());
  const arbc::expected<std::string, arbc::SerializeError> saved = arbc::save_document(doc, bridge);
  REQUIRE(saved.has_value());
  return *saved;
}

// --- content-free round-trip (the placement-only inverse) --------------------------
std::string placement_roundtrip(std::string_view bytes) {
  arbc::Model model;
  arbc::Registry registry;
  arbc::LoadContext ctx("mem://corpus.arbc");
  const arbc::expected<std::monostate, arbc::ReaderError> loaded =
      arbc::load_document(bytes, registry, ctx, model);
  REQUIRE(loaded.has_value());
  const arbc::expected<std::string, arbc::SerializeError> saved =
      arbc::serialize_document(*model.current());
  REQUIRE(saved.has_value());
  return *saved;
}

// ---------------------------------------------------------------------------------
// The canonical corpus (byte-exact goldens). Each is the writer's own canonical output
// -- sorted keys, 2-space indent, trailing newline, shortest-round-trip numbers.

// A built-in solid + tone document (the runtime.document_serialize golden).
constexpr const char* k_solid_tone = R"json({
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
        "kind": "org.arbc.solid",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "color": [
            1.0,
            0.5,
            0.25,
            1.0
          ]
        },
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
        "kind": "org.arbc.tone",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "amplitude": 0.5,
          "frequency_hz": 440
        },
        "transform": [
          1.0,
          0.0,
          0.0,
          1.0,
          0.0,
          0.0
        ],
        "visible": true
      }
    ]
  }
}
)json";

// An operator graph: fade + crossfade sharing one solid input (hoisted once into the
// `contents` table under "$ref": "0"); the crossfade's singly-referenced second input
// stays inline. The runtime.operator_codecs golden -- the canonical target the
// hand-authored variant below must normalize to.
constexpr const char* k_operator = R"json({
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
        "inputs": [
          {
            "$ref": "0"
          }
        ],
        "kind": "org.arbc.fade",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "in": {
            "end": 705600000,
            "start": 0
          },
          "shape": "linear"
        },
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
        "inputs": [
          {
            "$ref": "0"
          },
          {
            "kind": "org.arbc.solid",
            "kind_version": "1",
            "params": {
              "color": [
                0.0,
                0.0,
                1.0,
                1.0
              ]
            }
          }
        ],
        "kind": "org.arbc.crossfade",
        "kind_version": "1",
        "opacity": 1.0,
        "params": {
          "duration": 705600000,
          "shape": "linear",
          "start": 0
        },
        "transform": [
          1.0,
          0.0,
          0.0,
          1.0,
          0.0,
          0.0
        ],
        "visible": true
      }
    ]
  },
  "contents": {
    "0": {
      "kind": "org.arbc.solid",
      "kind_version": "1",
      "params": {
        "color": [
          1.0,
          0.5,
          0.25,
          1.0
        ]
      }
    }
  }
}
)json";

// A placement-only document: a default still layer and a fully-placed layer (the
// serialize.writer / serialize.reader shared canonical fixture).
constexpr const char* k_placement = R"json({
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

// The bare format envelope of a composition-less document.
constexpr const char* k_bare = "{\n  \"arbc\": {\n    \"format\": 1\n  }\n}\n";

// ---------------------------------------------------------------------------------
// A HAND-AUTHORED variant of k_operator: arbitrary non-canonical `contents` id
// ("shared-99" instead of "0"), unsorted keys throughout (visible before transform
// before kind; params before inputs; canvas after layers; contents' body keys
// reordered). It carries the SAME graph, so loading it and re-saving must reproduce
// k_operator byte-for-byte -- ids collapsed to first-encounter ordinals over the
// canonical layers-then-inputs traversal (doc 08:105-131, sharing.md Decision 2).
constexpr const char* k_authored_ids = R"json({
  "composition": {
    "layers": [
      {
        "visible": true,
        "transform": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
        "opacity": 1.0,
        "params": { "shape": "linear", "in": { "start": 0, "end": 705600000 } },
        "kind": "org.arbc.fade",
        "kind_version": "1",
        "inputs": [ { "$ref": "shared-99" } ]
      },
      {
        "kind": "org.arbc.crossfade",
        "kind_version": "1",
        "visible": true,
        "opacity": 1.0,
        "transform": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
        "params": { "start": 0, "duration": 705600000, "shape": "linear" },
        "inputs": [
          { "$ref": "shared-99" },
          {
            "params": { "color": [0.0, 0.0, 1.0, 1.0] },
            "kind_version": "1",
            "kind": "org.arbc.solid"
          }
        ]
      }
    ],
    "canvas": [0, 0, 1920, 1080]
  },
  "arbc": { "format": 1 },
  "contents": {
    "shared-99": {
      "params": { "color": [1.0, 0.5, 0.25, 1.0] },
      "kind": "org.arbc.solid",
      "kind_version": "1"
    }
  }
})json";

// A HAND-AUTHORED unknown-kind document: a plugin the host lacks, carrying params the
// host understands nothing about (an unknown `scale` field and an unsorted nested
// object). It must round-trip to a canonical fixed point with every byte of the
// unknown content body preserved and canonicalized (a PlaceholderContent, doc 08
// Principles 2/4/5) -- a missing plugin never destroys data.
constexpr const char* k_unknown_kind = R"json({
  "arbc": { "format": 1 },
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {
        "kind": "com.example.unknown",
        "kind_version": "3.0",
        "opacity": 1.0,
        "params": { "scale": 2.5, "nested": { "b": true, "a": [1, 2, 3] } },
        "transform": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
        "visible": true
      }
    ]
  }
})json";

} // namespace

// enforces: 08-serialization#canonical-output-is-byte-stable
// enforces: 08-serialization#load-save-round-trips-canonically
TEST_CASE(
    "every canonical corpus document round-trips byte-exact and is re-serialization idempotent") {
  // serialize(load(x)) == x for each canonical x, and the fixed point holds a second
  // time (idempotence): re-loading the output and re-saving reproduces it unchanged.
  SECTION("content-bearing documents (built-in codecs)") {
    for (const char* x : {k_solid_tone, k_operator}) {
      const std::string once = content_roundtrip(x);
      CHECK(once == std::string(x));
      CHECK(content_roundtrip(once) == std::string(x)); // idempotent
    }
  }

  SECTION("placement-only and bare-envelope documents (content-free inverse)") {
    for (const char* x : {k_placement, k_bare}) {
      const std::string once = placement_roundtrip(x);
      CHECK(once == std::string(x));
      CHECK(placement_roundtrip(once) == std::string(x)); // idempotent
    }
  }
}

// enforces: 08-serialization#unknown-kind-round-trips-verbatim
TEST_CASE("an unknown kind's content body round-trips verbatim to a canonical fixed point") {
  // A missing plugin must never destroy data: the unknown kind loads to a placeholder
  // preserving its whole content body (kind, kind_version, and every params field the
  // host does not understand), and re-saves to canonical form -- a fixed point on the
  // second pass.
  const std::string canonical = content_roundtrip(k_unknown_kind);
  CHECK(content_roundtrip(canonical) == canonical); // idempotent canonical fixed point

  // Nothing in the body is dropped or coerced: the unknown kind, its version, and the
  // unknown params (including the nested object) survive the round-trip verbatim.
  CHECK(canonical.find("\"com.example.unknown\"") != std::string::npos);
  CHECK(canonical.find("\"kind_version\": \"3.0\"") != std::string::npos);
  CHECK(canonical.find("\"scale\": 2.5") != std::string::npos);
  CHECK(canonical.find("\"nested\"") != std::string::npos);
}

// enforces: 08-serialization#hand-authored-ids-normalize-deterministically
// enforces: 08-serialization#shared-content-dedups-via-ref
TEST_CASE(
    "a hand-authored file with arbitrary contents ids and unsorted keys normalizes to canonical") {
  // The case serialize.sharing deferred here (sharing.md:434-436): non-canonical
  // `contents` ids ("shared-99") + unsorted keys re-serialize to canonical byte-exact
  // output, ids collapsed to first-encounter ordinals ("0") over the layers-then-inputs
  // traversal -- canonicalization, not data loss.
  const std::string normalized = content_roundtrip(k_authored_ids);
  CHECK(normalized == std::string(k_operator));
  CHECK(content_roundtrip(normalized) == std::string(k_operator)); // and it is a fixed point
}
