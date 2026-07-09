// serialize.writer byte-exact canonical golden + pinned-version fidelity. A small
// known document is built through the model transaction API, pinned, and
// serialized; the output is asserted byte-for-byte against an inline canonical
// `.arbc` string (doc 16 byte-exact goldens -- deterministic serialization gets a
// golden, not a tolerance). The golden demonstrates sorted keys, the
// fixed-number-formatting rule, and omit-when-default for the still-default twins.
// Cross-component (drives the model), so it lives here and links the umbrella arbc.

#include <arbc/base/ids.hpp>
#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/model/model.hpp>
#include <arbc/serialize/writer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

// The layer ids the golden document is built from.
struct BuiltDoc {
  arbc::ObjectId composition;
  arbc::ObjectId still_layer; // bottom: default still -- omit-when-default twins
  arbc::ObjectId placed_layer; // top: every core-owned placement key non-default
};

// Build the canonical golden document. Layer 0 (bottom) is a default still -- only
// transform/opacity/visible emit. Layer 1 (top) exercises a non-identity transform,
// a non-unit opacity + gain, an invisible + inaudible layer, a non-all() span, and a
// non-identity time map, so every omit-when-default twin appears.
BuiltDoc build_golden(arbc::Model& model) {
  BuiltDoc d;
  auto txn = model.transact("golden");
  d.composition = txn.add_composition(1920.0, 1080.0);
  const arbc::ObjectId c0 = txn.add_content(1);
  const arbc::ObjectId c1 = txn.add_content(2);
  d.still_layer = txn.add_layer(c0, arbc::Affine::identity(), 1.0);
  d.placed_layer = txn.add_layer(c1, arbc::Affine{2.0, 0.0, 0.0, 2.0, 10.5, 20.0}, 0.5);
  txn.set_gain(d.placed_layer, 2.0);
  txn.set_visible(d.placed_layer, false);
  txn.set_audible(d.placed_layer, false);
  txn.set_span(d.placed_layer, arbc::TimeRange{arbc::Time{0}, arbc::Time{705'600'000}});
  txn.set_time_map(d.placed_layer,
                   arbc::TimeMap{arbc::Time{100}, arbc::Rational{1, 2}, arbc::Time{5}});
  txn.attach_layer(d.composition, d.still_layer);
  txn.attach_layer(d.composition, d.placed_layer);
  REQUIRE(txn.commit());
  return d;
}

// The byte-canonical serialization of build_golden()'s document (doc 08).
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

// enforces: 08-serialization#canonical-output-is-byte-stable
TEST_CASE("serialize_document emits the byte-exact canonical golden and is re-serialization-stable") {
  arbc::Model model;
  build_golden(model);
  const auto pin = model.current();

  const auto first = arbc::serialize_document(*pin);
  REQUIRE(first);
  CHECK(*first == std::string(k_golden));

  // Byte-stable across re-serialization of the same pinned version.
  const auto second = arbc::serialize_document(*pin);
  REQUIRE(second);
  CHECK(*second == *first);
}

// enforces: 08-serialization#writer-serializes-the-pinned-version
TEST_CASE("serialize_document emits the pinned revision, unaffected by later transactions") {
  arbc::Model model;
  const BuiltDoc d = build_golden(model);
  const auto pin = model.current();
  const std::uint64_t pinned_revision = pin->revision();

  // Commit several further transactions on the live model after the pin.
  for (int i = 0; i < 4; ++i) {
    auto txn = model.transact("post-pin edit");
    txn.set_opacity(d.placed_layer, 0.123);
    txn.set_visible(d.placed_layer, true); // toggles the flag back on (the set arm)
    txn.set_transform(d.still_layer, arbc::Affine{9.0, 0.0, 0.0, 9.0, 0.0, 0.0});
    REQUIRE(txn.commit());
  }
  REQUIRE(model.current()->revision() > pinned_revision);

  // The held pin still serializes to the pinned revision's bytes, not the edits.
  const auto result = arbc::serialize_document(*pin);
  REQUIRE(result);
  CHECK(*result == std::string(k_golden));
}

} // namespace
