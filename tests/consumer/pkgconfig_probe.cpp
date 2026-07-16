// The pkg-config half of the out-of-tree consumer (packaging.install).
//
// Compiled and linked by the FOREIGN consumer project using ONLY the flags the
// installed arbc.pc yields (via CMake's PkgConfig::ARBC_PC imported target, built from
// `pkg-config --cflags`/`--libs arbc` against the staged prefix). That it compiles
// proves the .pc's Cflags reach the installed headers; that it links and resolves an
// out-of-line libarbc symbol proves the .pc's Libs reach the installed core archive.
// Deliberately touches only geometry + the version symbol -- no serialize path -- so a
// core-only link needs nothing from zstd, keeping the probe honest across the
// fetched/system/shared zstd lanes.
//
// No Catch2: the pkg-config audience is plain C/Makefile embedders of the core, who
// have no test framework.

#include <arbc/base/geometry.hpp>
#include <arbc/version.hpp>

#include <cstdio>

int main() {
  const arbc::Rect region = arbc::Rect::from_size(2.0, 3.0);
  if (region.width() != 2.0 || region.height() != 3.0 || region.empty()) {
    std::puts("pkgconfig_probe: geometry from the pkg-config include path is wrong");
    return 1;
  }
  // An out-of-line symbol from the installed archive, so this is a real link and not a
  // header-only pass -- the .pc's -larbc had to resolve it.
  std::printf("pkgconfig_probe: linked installed arbc %s via pkg-config\n",
              arbc::linked_version_string());
  return 0;
}
