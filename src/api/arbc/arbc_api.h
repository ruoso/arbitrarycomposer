// arbc/arbc_api.h -- the visibility annotation on libarbc's public symbol surface
// (packaging.shared_library_build; design doc 17:173-191).
//
// HAND-WRITTEN and checked in (unlike arbc/version.hpp, which configure_file()s
// PROJECT_VERSION): the macro derives from nothing, so there is nothing to
// generate. It is the library-wide sibling of ARBC_PLUGIN_EXPORT
// (arbc/contract/plugin.hpp), one level up: that macro exports the ONE plugin
// entry symbol out of a plugin; ARBC_API exports libarbc's C++ public surface out
// of the host image so a plugin loaded into the process resolves those symbols
// from the single libarbc rather than from a private static copy.
//
// This header includes NOTHING, on purpose, exactly like arbc/version.hpp. It is
// owned by the L6 umbrella and by no component (doc 17:33), so it installs at
// <prefix>/include/arbc/arbc_api.h -- at the include root, not under an
// arbc/<component>/ subdirectory -- and it must compile standalone and
// warning-clean under a CONSUMER's own flags, which are not ours (the exported
// interface is scrubbed of arbc_build_flags, so -Wall -Wextra -Wpedantic -Werror
// is our discipline and cannot be assumed downstream).
//
// The export/import branch keys off ARBC_BUILDING, a PRIVATE build-side define
// that arbc_add_component() puts on EVERY component object library and
// arbc_finalize_library() puts on the umbrella (cmake/ArbcComponent.cmake). It
// must be on every object library, not just the umbrella's lone version.cpp,
// because libarbc's public symbols are compiled in the object libraries: a
// define present only on the umbrella (the failure mode of CMake's auto-set
// <target>_EXPORTS) would mark the whole component surface *import* and export
// nothing -- reproducing the "-fvisibility=hidden SHARED build exports nothing"
// bug doc 17:174 records. A consumer NEVER defines ARBC_BUILDING, so it takes the
// import branch.
//
// On ELF the export and import branches are the symmetric visibility("default"),
// so the whole gcc BUILD_SHARED_LIBS lane is proven with one branch; the
// __declspec(dllexport)/(dllimport) asymmetry is the Windows form, used by a Clang
// Windows build (MSVC/cl.exe is not a supported toolchain and is untested).

#ifndef ARBC_ARBC_API_H
#define ARBC_ARBC_API_H

#if defined(_WIN32)
#if defined(ARBC_BUILDING)
#define ARBC_API __declspec(dllexport)
#else
#define ARBC_API __declspec(dllimport)
#endif
#else
#define ARBC_API __attribute__((visibility("default")))
#endif

#endif // ARBC_ARBC_API_H
