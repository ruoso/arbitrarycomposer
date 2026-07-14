// The core-only half of the out-of-tree consumer (quality.testing_artifact).
//
// Links arbc::arbc ALONE, from a foreign project configured WITHOUT
// `COMPONENTS testing` and with -DCMAKE_DISABLE_FIND_PACKAGE_Catch2=ON, so Catch2
// genuinely cannot be found -- the negative is real, not an accident of the CI
// image. That it compiles proves the component public headers installed; that it
// links proves the installed static libarbc resolves its own symbols and that the
// generated package config expresses whatever zstd requirement the build was made
// against; that find_package(arbc) succeeded at all is doc 17:14's "never by
// libarbc" edge, tested rather than asserted in a comment.
//
// The other half of the claim -- that arbc::testing is not even DEFINED in this
// configuration -- is a CMake-time assertion in tests/consumer/CMakeLists.txt.
// Deliberately no Catch2 here: needing it would defeat the point.

#include <arbc/base/geometry.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/version.hpp>

#include <cstdio>
#include <cstring>

// enforces: 17-internal-components#libarbc-never-requires-arbc-testing
// enforces: 16-sdlc-and-quality#library-version-is-single-sourced
int main() {
  // packaging.version_api, and the half an in-tree test structurally cannot give:
  // <arbc/version.hpp> is reached from the STAGED INSTALL PREFIX, at the include root
  // rather than under an arbc/<component>/ subdirectory. An in-tree test would pass
  // happily on a header that never ships; this one does not compile if the header did
  // not install. Three paths out of the ONE project(VERSION ...) declaration meet here:
  // the installed header (compiled_version, inlined from its macros), the installed
  // archive (linked_version, an out-of-line symbol resolved at link), and the installed
  // CMake package config (ARBC_PACKAGE_VERSION, from arbc_VERSION -- see CMakeLists.txt).
  if (!(arbc::compiled_version() == arbc::linked_version())) {
    std::puts("core_only: the installed header and the installed libarbc disagree on the "
              "version");
    return 1;
  }
  if (std::strcmp(arbc::linked_version_string(), ARBC_PACKAGE_VERSION) != 0 ||
      std::strcmp(ARBC_VERSION_STRING, ARBC_PACKAGE_VERSION) != 0) {
    std::printf("core_only: version disagreement -- header %s, library %s, package %s\n",
                ARBC_VERSION_STRING, arbc::linked_version_string(), ARBC_PACKAGE_VERSION);
    return 1;
  }

  const arbc::Rect region = arbc::Rect::from_size(8.0, 4.0);
  if (region.width() != 8.0 || region.height() != 4.0 || region.empty()) {
    std::puts("core_only: geometry from the installed headers is wrong");
    return 1;
  }

  // One plain core API, reached across the installed archive: a reference kind's
  // out-of-line constructor and virtual -- an actual link, not a header-only pass.
  const arbc::SolidContent content{arbc::Rgba{0.5F, 0.25F, 0.125F, 1.0F}};
  if (content.stability() != arbc::Stability::Static || content.bounds().has_value()) {
    std::puts("core_only: org.arbc.solid from the installed libarbc is wrong");
    return 1;
  }

  std::puts("core_only: installed arbc::arbc is usable with no Catch2 in sight");
  return 0;
}
