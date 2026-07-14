// The codec-line containment proof for org.arbc.image (doc 17 "The codec line";
// kinds/image.md Constraint 1), mirroring `tests/imageseq_containment.t.cpp`.
//
// The vendored stb-class decode dependency (`plugins/imdec/third_party/imdec.h`, whose public
// symbols are prefixed `imdec_` exactly as real stb_image's are prefixed `stbi_`) must resolve
// ONLY in the plugin tier -- the image plugin's impl archive and its loadable module -- and
// NEVER in `libarbc` or `arbc-testing`, so a decoder never rides into an embedder's link line
// (doc 10:29: image codecs are "not core -- and no in-lib kind needs one").
//
// This is what makes "the codec line is a DECODER line" (doc 17, doc 00) an enforced statement
// rather than a slogan: `org.arbc.image`'s SERIALIZE codec lives in `runtime`, inside libarbc,
// and that is fine -- it parses our own JSON and a URI string. Its DECODER does not, and this
// test is why.
//
// Implemented as a byte scan of the built artifacts (paths passed as compile definitions): the
// decode symbols appear as ASCII in the archives'/module's symbol tables, so `libarbc`
// containing the substring at all would mean the decode dependency leaked into core.
// Self-contained -- no subprocess, no gate edit (check_levels.py has no symbol/link-line
// notion; this is the enforcing test that pins the claim).

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <string>

namespace {

// The decode dependency's public symbol prefix (would be "stbi_" with real stb_image).
// Present iff a translation unit compiled the decoder into the target.
constexpr const char* k_codec_symbol = "imdec_";

std::string read_binary(const char* path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains_codec_symbol(const char* path) {
  return read_binary(path).find(k_codec_symbol) != std::string::npos;
}

} // namespace

// enforces: 17-internal-components#image-decode-dep-stays-out-of-libarbc
TEST_CASE("the image decode dependency stays out of libarbc and arbc-testing") {
  // The containment guarantee: no decode symbol in core, and none in the conformance suite an
  // embedder also links.
  REQUIRE_FALSE(contains_codec_symbol(ARBC_LIBARBC_FILE));
  REQUIRE_FALSE(contains_codec_symbol(ARBC_TESTING_FILE));

  // ...and it genuinely lives in the plugin, so the guarantee is not vacuous: the shared
  // vendored archive carries it, and so do the image plugin's impl archive (which links that
  // archive PRIVATE) and its loadable module.
  REQUIRE(contains_codec_symbol(ARBC_IMDEC_FILE));
  REQUIRE(contains_codec_symbol(ARBC_IMAGE_IMPL_FILE));
  REQUIRE(contains_codec_symbol(ARBC_IMAGE_PLUGIN_FILE));
}
