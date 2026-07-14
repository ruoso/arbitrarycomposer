// Hostile `org.arbc.raster` documents (serialize.raster_tile_store Constraint 7).
//
// `zstd_dep` bounded decompression by the CALLER's declared size, and doc 08:440-442
// promised that bound comes from "the tile geometry the document declares, never from the
// frame header." This file closes the consequence that promise leaves open: once the
// GEOMETRY is itself attacker-controlled, it must be validated before it is used as an
// allocation bound. A document claiming `edge: 4294967295` must be a `ReaderError`, not a
// 64 GB `resize`.
//
// Every case here is a VALUE, never a throw, never a crash, and never silent wrong pixels.

#include <arbc/base/ids.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/filesystem_asset_source.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>

using namespace arbc;
using json = nlohmann::json;

namespace {

// A one-raster-layer document whose `params` are whatever the caller says.
json document_json(const json& params) {
  json layer = json::object();
  layer["kind"] = "org.arbc.raster";
  layer["kind_version"] = "1";
  layer["opacity"] = 1.0;
  layer["params"] = params;
  layer["transform"] = json::array({1.0, 0.0, 0.0, 1.0, 0.0, 0.0});
  layer["visible"] = true;

  json composition = json::object();
  composition["canvas"] = json::array({0, 0, 64, 64});
  composition["layers"] = json::array({std::move(layer)});

  json envelope = json::object();
  envelope["format"] = 1;

  json doc = json::object();
  doc["arbc"] = std::move(envelope);
  doc["composition"] = std::move(composition);
  return doc;
}

std::string document_with_params(const json& params) {
  return document_json(params).dump(2) + "\n";
}

// The shape of a WELL-FORMED params object, which each case then corrupts in one way.
json good_params() {
  json blobs = json::array();
  for (int i = 0; i < 4; ++i) { // 32x32 at edge 16 => a 2x2 grid
    blobs.push_back("00000000000000000000000000000000");
  }
  json p = json::object();
  p["tiles"] = "assets/tiles/";
  p["edge"] = 16;
  p["width"] = 32;
  p["height"] = 32;
  p["blobs"] = std::move(blobs);
  return p;
}

// Load `params` and report the reader's verdict. A hostile document must never reach the
// model, and must never take the process down.
expected<std::monostate, ReaderError> try_load(const json& params) {
  const Registry registry;
  FilesystemAssetSource source; // resolves nothing: every blob is absent
  Document doc;
  KindBridge bridge;
  return load_document(document_with_params(params), doc, bridge, registry,
                       "/nonexistent/project.arbc", &source);
}

} // namespace

// enforces: 08-serialization#raster-tile-geometry-is-validated-before-allocation
TEST_CASE("an absurd tile edge is rejected before it sizes an allocation") {
  json p = good_params();

  // Non-power-of-two: our tiling is power-of-two by construction, so this document was
  // not produced by us.
  p["edge"] = 17;
  CHECK_FALSE(try_load(p).has_value());

  // Enormous. The decompression bound is edge^2 * 4 * 4 bytes; unbounded here means an
  // unbounded allocation, which is the whole hazard.
  p["edge"] = 65536;
  CHECK_FALSE(try_load(p).has_value());

  // Beyond int32 entirely -- the case a narrowing `get<int>()` would silently truncate
  // into something plausible.
  p["edge"] = 4294967296LL;
  CHECK_FALSE(try_load(p).has_value());

  p["edge"] = 0;
  CHECK_FALSE(try_load(p).has_value());
  p["edge"] = -16;
  CHECK_FALSE(try_load(p).has_value());
  p["edge"] = "sixteen";
  CHECK_FALSE(try_load(p).has_value());
}

// enforces: 08-serialization#raster-tile-geometry-is-validated-before-allocation
TEST_CASE("an overflowing width x height is rejected before it sizes an allocation") {
  json p = good_params();

  p["width"] = 1LL << 40;
  CHECK_FALSE(try_load(p).has_value());

  p["width"] = 32;
  p["height"] = 1LL << 40;
  CHECK_FALSE(try_load(p).has_value());

  // Individually legal sides whose implied tile grid is not: the dense load buffer is
  // w * h * 16 bytes, so this is the multiply that must not be trusted.
  p["edge"] = 1;
  p["width"] = 1048576;
  p["height"] = 1048576;
  CHECK_FALSE(try_load(p).has_value());

  p = good_params();
  p["height"] = 0;
  CHECK_FALSE(try_load(p).has_value());
  p["height"] = -32;
  CHECK_FALSE(try_load(p).has_value());
}

// enforces: 08-serialization#raster-tile-geometry-is-validated-before-allocation
TEST_CASE("a blobs array that disagrees with the geometry is rejected") {
  json p = good_params();

  // Short: 32x32 at edge 16 needs exactly four. Padding it out would invent pixels.
  p["blobs"] = json::array({"00000000000000000000000000000000"});
  CHECK_FALSE(try_load(p).has_value());

  // Long: truncating would silently drop the user's tiles.
  json many = json::array();
  for (int i = 0; i < 9; ++i) {
    many.push_back("00000000000000000000000000000000");
  }
  p["blobs"] = many;
  CHECK_FALSE(try_load(p).has_value());

  p["blobs"] = json::array();
  CHECK_FALSE(try_load(p).has_value());

  p = good_params();
  p["blobs"] = "not-an-array";
  CHECK_FALSE(try_load(p).has_value());

  p.erase("blobs");
  CHECK_FALSE(try_load(p).has_value());
}

// enforces: 08-serialization#raster-tile-geometry-is-validated-before-allocation
TEST_CASE("a malformed hash string is rejected, and never becomes a path") {
  json p = good_params();

  // The blob name is a canonical spelling, not a value: 32 lowercase hex chars. Anything
  // else must never be handed to a URI resolver -- a `blobs` entry is the one place a
  // hostile document gets to name a FILE.
  p["blobs"] = json::array({"../../../../etc/passwd", "00000000000000000000000000000000",
                            "00000000000000000000000000000000", "00000000000000000000000000000000"});
  CHECK_FALSE(try_load(p).has_value());

  p["blobs"] = json::array({"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", "00000000000000000000000000000000",
                            "00000000000000000000000000000000", "00000000000000000000000000000000"});
  CHECK_FALSE(try_load(p).has_value());

  p["blobs"] = json::array({"deadbeef", "00000000000000000000000000000000",
                            "00000000000000000000000000000000", "00000000000000000000000000000000"});
  CHECK_FALSE(try_load(p).has_value());

  p["blobs"] = json::array({42, "00000000000000000000000000000000",
                            "00000000000000000000000000000000", "00000000000000000000000000000000"});
  CHECK_FALSE(try_load(p).has_value());
}

TEST_CASE("required geometry keys are required") {
  for (const char* key : {"edge", "width", "height"}) {
    json p = good_params();
    p.erase(key);
    CHECK_FALSE(try_load(p).has_value());
  }
}

TEST_CASE("a well-formed geometry whose blobs are absent is an unresolvable reference") {
  // The geometry is FINE here; the blobs simply are not on disk. Painted pixels are
  // document state with no pending rendering (Decision 7): a raster layer whose tiles have
  // not arrived is not a layer in a pending state, it is a BROKEN DOCUMENT. So this is a
  // ReaderError -- distinct from the geometry rejections above, and distinct from an
  // absent IMPORTED image, which degrades benignly.
  const expected<std::monostate, ReaderError> loaded = try_load(good_params());
  REQUIRE_FALSE(loaded.has_value());
  CHECK(loaded.error().kind == ReaderError::Kind::UnresolvableReference);
}

TEST_CASE("an unrecognized storage_format is a clean ReaderError, not a silent default") {
  const Registry registry;
  FilesystemAssetSource source;
  Document doc;
  KindBridge bridge;

  json d = document_json(good_params());
  // Silently falling back to rgba16f would reinterpret every byte of every tile at the
  // wrong precision. The lossy/lossless call is the user's to author (doc 08).
  d["arbc"]["storage_format"] = "rgba8";
  const expected<std::monostate, ReaderError> bad =
      load_document(d.dump(2) + "\n", doc, bridge, registry, "/nonexistent/project.arbc", &source);
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().kind == ReaderError::Kind::MalformedField);
  CHECK(bad.error().path == "/arbc/storage_format");

  // The composition-level working-space spelling is a DIFFERENT key at a different tier.
  // Accepting it here would be exactly the confusion Decision 4 exists to prevent.
  d["arbc"]["storage_format"] = "rgba16f-linear-premul";
  Document doc2;
  KindBridge bridge2;
  CHECK_FALSE(
      load_document(d.dump(2) + "\n", doc2, bridge2, registry, "/nonexistent/project.arbc", &source)
          .has_value());
}
