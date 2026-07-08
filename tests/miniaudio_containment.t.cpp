// The device-line containment proof (doc 17:150-159 generalized to device
// backends; device_monitor.md D4/§acceptance). The miniaudio-class backend
// dependency (here third_party/maudio.h, a drop-in stand-in for miniaudio whose
// public symbols are prefixed `maudio_`, mirroring `ma_`) must resolve ONLY in the
// plugin -- its impl archive and its loadable module -- and never in `libarbc` or
// `arbc-testing`, so an OS-audio backend never rides into an embedder's link line
// (doc 10). The device analog of imageseq_containment.t.cpp.
//
// Implemented as a byte scan of the built artifacts (paths passed as compile
// definitions): the backend symbols appear as ASCII in the archives'/module's
// symbol tables, so `libarbc` containing the substring at all would mean the
// backend dependency leaked into core. Self-contained: no subprocess, no gate edit.

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <string>

namespace {

// The backend dependency's public symbol prefix (would be "ma_" with real
// miniaudio). Present iff a translation unit compiled the backend into the target.
constexpr const char* k_backend_symbol = "maudio_device_open";

std::string read_binary(const char* path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains_backend_symbol(const char* path) {
  return read_binary(path).find(k_backend_symbol) != std::string::npos;
}

} // namespace

TEST_CASE("the miniaudio backend dependency stays out of libarbc and arbc-testing") {
  // The containment guarantee: no backend symbol appears in libarbc or arbc-testing.
  REQUIRE_FALSE(contains_backend_symbol(ARBC_LIBARBC_FILE));
  REQUIRE_FALSE(contains_backend_symbol(ARBC_TESTING_FILE));

  // ...and it genuinely lives in the plugin (so the guarantee is not vacuous): the
  // impl static archive always retains its symbol table, and the loadable module
  // carries the backend code it was built with.
  REQUIRE(contains_backend_symbol(ARBC_MINIAUDIO_IMPL_FILE));
  REQUIRE(contains_backend_symbol(ARBC_MINIAUDIO_PLUGIN_FILE));
}
