// serialize.writer component unit tests: the errors-as-values boundary, the
// empty-document envelope, and the non-default working_space / working_audio_format
// emission paths. The byte-exact canonical golden, pinned-version fidelity, and the
// TSan concurrency stress are cross-component and live in tests/ (serialize.writer
// Constraint 6).

#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/media/audio_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/serialize/writer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

namespace {

TEST_CASE("serialize_document surfaces a non-finite placement scalar as an error value") {
  // Errors as values (serialize.writer Constraint 3, doc 10:15-17): a NaN opacity
  // cannot round-trip through JSON, so the entry point returns an error value and
  // no exception escapes -- never a silent `null`.
  arbc::Model model;
  arbc::ObjectId layer;
  {
    auto txn = model.transact("nan");
    const arbc::ObjectId comp = txn.add_composition(16.0, 16.0);
    const arbc::ObjectId content = txn.add_content(1);
    layer = txn.add_layer(content, arbc::Affine::identity(), 1.0);
    txn.set_opacity(layer, std::numeric_limits<double>::quiet_NaN());
    txn.attach_layer(comp, layer);
    REQUIRE(txn.commit());
  }

  const auto pin = model.current();
  const auto result = arbc::serialize_document(*pin);
  REQUIRE_FALSE(result);
  CHECK(result.error().kind == arbc::SerializeError::Kind::NonFiniteValue);
  CHECK(result.error().object == layer);
}

TEST_CASE("serialize_document emits the bare envelope for a document with no composition") {
  // A fresh model has no composition: `composition` is omitted entirely, leaving
  // just the format envelope (exercises the find_first_composition absent path).
  arbc::Model model;
  const auto pin = model.current();
  const auto result = arbc::serialize_document(*pin);
  REQUIRE(result);
  CHECK(*result == "{\n  \"arbc\": {\n    \"format\": 1\n  }\n}\n");
}

TEST_CASE("serialize_document emits working_space / working_audio_format when non-default") {
  // Omit-when-default gate's present side: a non-default working space and audio
  // format round-trip losslessly (serialize.writer Constraint 4).
  SECTION("rgba16f working space + 44.1 kHz mono audio") {
    arbc::Model model;
    {
      auto txn = model.transact("cfg");
      const arbc::ObjectId comp = txn.add_composition(8.0, 8.0);
      txn.set_working_space(comp, arbc::k_working_rgba16f);
      txn.set_working_audio_format(comp, arbc::AudioFormat{44100, arbc::ChannelLayout::Mono});
      REQUIRE(txn.commit());
    }
    const auto pin = model.current();
    const auto result = arbc::serialize_document(*pin);
    REQUIRE(result);
    const std::string& s = *result;
    CHECK(s.find("\"working_space\"") != std::string::npos);
    CHECK(s.find("\"format\": \"rgba16f-linear-premul\"") != std::string::npos);
    CHECK(s.find("\"premultiplied\": true") != std::string::npos);
    CHECK(s.find("\"transfer\": \"linear\"") != std::string::npos);
    CHECK(s.find("\"working_audio_format\"") != std::string::npos);
    CHECK(s.find("\"channels\": \"mono\"") != std::string::npos);
    CHECK(s.find("\"sample_rate\": 44100") != std::string::npos);
  }

  SECTION("rgba8-srgb fast-mode working space covers the straight-alpha / srgb-transfer arms") {
    arbc::Model model;
    {
      auto txn = model.transact("cfg8");
      const arbc::ObjectId comp = txn.add_composition(8.0, 8.0);
      txn.set_working_space(comp, arbc::k_fast_rgba8srgb);
      REQUIRE(txn.commit());
    }
    const auto pin = model.current();
    const auto result = arbc::serialize_document(*pin);
    REQUIRE(result);
    const std::string& s = *result;
    CHECK(s.find("\"format\": \"rgba8-srgb\"") != std::string::npos);
    CHECK(s.find("\"premultiplied\": false") != std::string::npos);
    CHECK(s.find("\"transfer\": \"srgb\"") != std::string::npos);
  }
}

} // namespace
