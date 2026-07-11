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

// ---------------------------------------------------------------------------------
// The CANONICAL two-composition document (serialize.compositions_table, doc 08
// Principle 7): the root keeps its home at `composition` and holds the reserved ordinal
// "0"; the one child lands in `compositions` under "1"; the nesting body names it through
// the core-owned `composition` field. The nesting kind is one this build has NO codec for
// -- the child reference rides its `PlaceholderContent`, so the child composition stays
// reachable and a missing plugin never orphans it (Principle 2, Decision 6).
constexpr const char* k_compositions = R"json({
  "arbc": {
    "format": 1
  },
  "composition": {
    "canvas": [
      0,
      0,
      16,
      16
    ],
    "layers": [
      {
        "composition": "1",
        "kind": "com.example.nest",
        "kind_version": "3.2",
        "opacity": 1.0,
        "params": {
          "blend": "over"
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
  "compositions": {
    "1": {
      "canvas": [
        0,
        0,
        8,
        8
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
        }
      ]
    }
  }
}
)json";

// A HAND-AUTHORED variant of k_compositions: arbitrary non-canonical composition keys
// ("main" for the reachable child, "zzz" for one NOTHING references), unsorted keys
// throughout, and the nesting body naming its child by the authored key. It carries the
// same reachable graph, so loading it and re-saving must reproduce k_compositions
// byte-for-byte: composition ids collapse to first-encounter ordinals over the canonical
// traversal (Constraint 2/3), and the unreachable entry is ignored on load and dropped on
// save -- canonicalization, like a renumbered id, not data loss (Principle 4, Decision 3's
// sibling rule). It also pins that `composition` is a KNOWN body key: were it not, the
// authored "main" would be stashed as an unknown AND the core would emit its own re-derived
// "1" beside it (unknown_field_preservation Constraint 7).
constexpr const char* k_authored_compositions = R"json({
  "compositions": {
    "zzz": {
      "layers": [
        { "kind": "org.arbc.solid", "kind_version": "1",
          "params": { "color": [0.0, 0.0, 0.0, 1.0] } }
      ],
      "canvas": [0, 0, 4, 4]
    },
    "main": {
      "layers": [
        {
          "params": { "color": [1.0, 0.5, 0.25, 1.0] },
          "kind_version": "1",
          "kind": "org.arbc.solid",
          "opacity": 1.0,
          "transform": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
          "visible": true
        }
      ],
      "canvas": [0, 0, 8, 8]
    }
  },
  "composition": {
    "layers": [
      {
        "visible": true,
        "composition": "main",
        "params": { "blend": "over" },
        "transform": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
        "kind_version": "3.2",
        "opacity": 1.0,
        "kind": "com.example.nest"
      }
    ],
    "canvas": [0, 0, 16, 16]
  },
  "arbc": { "format": 1 }
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

// ---------------------------------------------------------------------------------
// serialize.unknown_field_preservation: unknown SIBLING fields at every tier (doc 08
// Principle 4). Each of these is already canonical, so `serialize(load(x)) == x`
// byte-exact IS the preservation proof -- a dropped key would shorten the output.

// Unknown siblings at the envelope ("generator"), the document root ("vendor"), the
// composition ("title"), inside the known `working_space` sub-object ("vendor_gamut"),
// at the layer (doc 08's own example field, `docs/design/08-serialization.md:35`:
// "name"), and inside the known `time_map` sub-object ("curve"). `working_space`'s
// `primaries` is read-ignored by the reader and emitted by the writer, so it is an
// unknown at load and a known at save -- the never-shadow case, pinned byte-exactly here
// and behaviorally in serialize_unknown_fields.t.cpp.
constexpr const char* k_unknown_all_tiers = R"json({
  "arbc": {
    "format": 1,
    "generator": "acme-authoring/2.1"
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
        "name": "backdrop",
        "opacity": 1.0,
        "params": {
          "color": [
            1.0,
            0.5,
            0.25,
            1.0
          ]
        },
        "time_map": {
          "curve": "ease-in",
          "in": 100,
          "offset": 5,
          "rate": [
            1,
            2
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
      }
    ],
    "title": "scene one",
    "working_space": {
      "format": "rgba16f-linear-premul",
      "premultiplied": true,
      "primaries": "srgb",
      "transfer": "linear",
      "vendor_gamut": "acescg"
    }
  },
  "vendor": {
    "tool": "acme"
  }
}
)json";

// Unknown siblings on a STANDALONE `contents`-table body ("author") -- the one position
// where a content body carries its own unknown siblings rather than the layer's
// (Decision 5) -- plus an unknown key inside a KNOWN kind's `params` ("gamma"), which the
// solid codec never consumed and the load-time residual recovers (Decision 4).
constexpr const char* k_unknown_shared_body = R"json({
  "arbc": {
    "format": 1
  },
  "composition": {
    "canvas": [
      0,
      0,
      16,
      16
    ],
    "layers": [
      {
        "$ref": "0",
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
        "$ref": "0",
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
      }
    ]
  },
  "contents": {
    "0": {
      "author": "acme",
      "kind": "org.arbc.solid",
      "kind_version": "1",
      "params": {
        "color": [
          1.0,
          0.5,
          0.25,
          1.0
        ],
        "gamma": 2.2
      }
    }
  }
}
)json";

// An UNKNOWN-kind body at a LAYER position carrying an unrecognized sibling ("badge").
// Before this task `extract_content_body`'s 4-key filter truncated the body before the
// placeholder could see it, so `badge` died on the floor (Constraint 1).
constexpr const char* k_unknown_kind_sibling = R"json({
  "arbc": {
    "format": 1
  },
  "composition": {
    "canvas": [
      0,
      0,
      16,
      16
    ],
    "layers": [
      {
        "badge": "vendor-x",
        "kind": "com.example.unknown",
        "kind_version": "3.0",
        "opacity": 1.0,
        "params": {
          "scale": 2.5
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

} // namespace

// enforces: 08-serialization#unknown-fields-preserved-at-every-tier
// enforces: 08-serialization#unknown-kind-round-trips-verbatim
TEST_CASE("unknown sibling fields survive a round-trip at every document tier") {
  // Doc 08 Principle 4 is a DATA-LOSS guarantee: within a known format major, a field
  // this build does not name is preserved-and-ignored, not dropped. Each corpus entry is
  // already canonical, so a byte-exact `serialize(load(x)) == x` is the proof -- and the
  // second pass pins it as a fixed point, so the stash itself round-trips.
  for (const char* x : {k_unknown_all_tiers, k_unknown_shared_body, k_unknown_kind_sibling}) {
    const std::string once = content_roundtrip(x);
    CHECK(once == std::string(x));
    CHECK(content_roundtrip(once) == std::string(x)); // idempotent
  }
}

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

// enforces: 08-serialization#hand-authored-ids-normalize-deterministically
// enforces: 08-serialization#child-compositions-round-trip-in-document
TEST_CASE("canonicalization holds across the composition id space too") {
  // The canonical two-composition document is a fixed point...
  CHECK(content_roundtrip(k_compositions) == std::string(k_compositions));

  // ...and a hand-authored file carrying the SAME reachable graph under arbitrary
  // composition keys ("main"), unsorted keys, and an UNREACHABLE table entry ("zzz")
  // normalizes to it byte-for-byte: the composition ids collapse to first-encounter
  // ordinals over the canonical traversal, and the entry no reference reaches is dropped
  // (doc 08 Principle 4 -- an id-keyed table is not a sibling surface).
  const std::string normalized = content_roundtrip(k_authored_compositions);
  CHECK(normalized == std::string(k_compositions));
  CHECK(content_roundtrip(normalized) == std::string(k_compositions));

  // The authored key is GONE, not stashed-and-re-emitted beside the core's own ordinal:
  // `composition` is a known body key, so it is never mistaken for an unknown field
  // (unknown_field_preservation Constraint 7). The unreachable composition's only
  // distinguishing content is likewise gone.
  CHECK(normalized.find("\"main\"") == std::string::npos);
  CHECK(normalized.find("\"zzz\"") == std::string::npos);
  CHECK(normalized.find("\"composition\": \"1\"") != std::string::npos);
}
