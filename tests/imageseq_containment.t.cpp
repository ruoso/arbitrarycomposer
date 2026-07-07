// The codec-line containment proof (doc 17:150-159; imageseq_plugin.md §1 and
// §acceptance). The stb-class decode dependency (here third_party/imdec.h, a
// drop-in stand-in for stb_image whose public symbols are prefixed `imdec_`,
// mirroring `stbi_`) must resolve ONLY in the plugin -- its impl archive and its
// loadable module -- and never in `libarbc`, so a codec never rides into an
// embedder's link line (doc 10).
//
// Implemented as a byte scan of the built artifacts (paths passed as compile
// definitions): the decode symbols appear as ASCII in the archives'/module's
// symbol tables, so `libarbc` containing the substring at all would mean the
// decode dependency leaked into core. Self-contained: no subprocess, no gate
// edit (check_levels.py has no symbol/link-line notion; this is the enforcing
// test that pins the claim).

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <string>

namespace {

// The decode dependency's public symbol prefix (would be "stbi_" with real
// stb_image). Present iff a translation unit compiled the codec into the target.
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

// enforces: 17-internal-components#imageseq-decode-dep-stays-out-of-libarbc
TEST_CASE("the imageseq decode dependency stays out of libarbc") {
  // The containment guarantee: no codec symbol appears in libarbc.
  REQUIRE_FALSE(contains_codec_symbol(ARBC_LIBARBC_FILE));

  // ...and it genuinely lives in the plugin (so the guarantee is not vacuous):
  // the impl static archive always retains its symbol table, and the loadable
  // module carries the decode code it was built with.
  REQUIRE(contains_codec_symbol(ARBC_IMAGESEQ_IMPL_FILE));
  REQUIRE(contains_codec_symbol(ARBC_IMAGESEQ_PLUGIN_FILE));
}
