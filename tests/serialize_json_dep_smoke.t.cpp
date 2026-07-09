// serialize.json_dep build-integration smoke probe. Not a component unit test
// and carries no claim (see tests/CMakeLists.txt): it links the chosen JSON
// library (nlohmann/json) standalone and PROVES -- in the real build, not on
// paper -- the two capabilities the whole serialize format rests on, plus the
// key-ordering half of canonical output this task can pin cheaply:
//
//   (i)   verbatim round-trip of an arbitrary/unknown-kind JSON tree with
//         nested params (doc 08 Principle 2 file side, 08:58-64);
//   (ii)  a NON-THROWING parse path -- parse(bad, nullptr, false) yields a
//         discarded value on malformed input with no exception escaping, the
//         error-as-value shape an L4 API wraps as arbc::expected (doc 08
//         Dependency note req (c), doc 10:15-17);
//   (iii) a default json object serializes its keys in SORTED order (doc 08
//         Principle 5 key-ordering, 08:75-77).
//
// Byte-canonical *number* formatting and full canonical output are deferred to
// serialize.writer's goldens; this probe pins only what the library gives for
// free.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>

using nlohmann::json;

TEST_CASE("nlohmann/json round-trips an unknown-kind tree verbatim") {
  // A layer whose kind the host does not recognize, carrying nested, arbitrary
  // params -- exactly the shape a missing plugin must never destroy (doc 08
  // Principle 2). Keys are deliberately out of order and the params nest
  // arrays/objects/scalars of every type.
  const std::string source = R"({
    "kind": "com.example.unknown-to-this-host",
    "params": {
      "z_last": 1,
      "a_first": [1, 2, {"nested": true, "list": [null, 3.5, "str"]}],
      "weird": {"deep": {"deeper": [false, "x", 0]}}
    }
  })";

  const json parsed = json::parse(source);
  // Dump and re-parse: the value tree must survive verbatim (modulo formatting).
  const json reparsed = json::parse(parsed.dump());
  REQUIRE(parsed == reparsed);

  // The unknown params subtree is preserved intact, not flattened or dropped.
  REQUIRE(parsed.at("params") == reparsed.at("params"));
  REQUIRE(parsed.at("params").at("a_first").at(2).at("list").at(1) == 3.5);
  REQUIRE(parsed.at("kind") == "com.example.unknown-to-this-host");
}

TEST_CASE("nlohmann/json parses malformed input without throwing") {
  const std::string malformed = R"({"kind": "x", "params": {)"; // truncated

  // The non-throwing overload: no callback, allow_exceptions == false. On a
  // parse error it returns a discarded value instead of throwing -- the shape an
  // L4 loader surfaces as arbc::expected, so no nlohmann exception crosses the
  // API boundary (doc 10:15-17).
  const json result = json::parse(malformed, /*cb=*/nullptr,
                                  /*allow_exceptions=*/false);
  REQUIRE(result.is_discarded());

  // The same overload on VALID input yields a live (non-discarded) value.
  const json ok = json::parse(R"({"kind": "x"})", /*cb=*/nullptr,
                              /*allow_exceptions=*/false);
  REQUIRE_FALSE(ok.is_discarded());
  REQUIRE(ok.at("kind") == "x");
}

TEST_CASE("nlohmann/json default object emits keys in sorted order") {
  // The default json object is std::map-backed, so it hands canonical key order
  // to serialize.writer for free (doc 08 Principle 5, key half). Insert out of
  // order; the dump must be lexicographically sorted.
  json object;
  object["zebra"] = 1;
  object["apple"] = 2;
  object["mango"] = 3;

  REQUIRE(object.dump() == R"({"apple":2,"mango":3,"zebra":1})");
}
