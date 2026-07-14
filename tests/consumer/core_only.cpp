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

#include <cstdio>

// enforces: 17-internal-components#libarbc-never-requires-arbc-testing
int main() {
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
