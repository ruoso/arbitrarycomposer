// packaging.version_api: the in-tree half of the single-source-of-truth proof.
//
// The load-bearing assertions are the ones against ARBC_TEST_PROJECT_VERSION_*, injected
// on THIS TARGET ONLY (tests/CMakeLists.txt) straight from `project(... VERSION ...)`.
// That is a SECOND, independent path from the one declaration in the top-level
// CMakeLists to the assertion, so a version literal reintroduced into src/version.cpp or
// into the header template fails here rather than passing vacuously against itself
// (version_api Constraint 1).
//
// The other half of the claim -- that the header is INSTALLED and reachable, and that the
// CMake package config advertises the same triple -- cannot be shown from inside the
// build tree at all, and lives in tests/consumer/core_only.cpp, driven across a staged
// install prefix by the install.consumer CTest.

#include <arbc/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>

// enforces: 16-sdlc-and-quality#library-version-is-single-sourced
TEST_CASE("the version is single-sourced from project(VERSION ...)", "[version]") {
  SECTION("the linked library reports what the compiled-against header declares") {
    // In the build tree these are necessarily the same header, so this is a tautology
    // TODAY and a real check the moment a prebuilt libarbc meets a different header.
    // What it does pin unconditionally: linked_version() is a real out-of-line symbol
    // that resolves from the archive (version_api Constraint 4 / D2).
    CHECK(arbc::compiled_version() == arbc::linked_version());
    CHECK(std::string{arbc::linked_version_string()} == std::string{ARBC_VERSION_STRING});
  }

  SECTION("the macros, the encoding, and the struct agree") {
    const arbc::Version compiled = arbc::compiled_version();
    CHECK(compiled.major == ARBC_VERSION_MAJOR);
    CHECK(compiled.minor == ARBC_VERSION_MINOR);
    CHECK(compiled.patch == ARBC_VERSION_PATCH);

    CHECK(ARBC_VERSION ==
          ARBC_VERSION_ENCODE(ARBC_VERSION_MAJOR, ARBC_VERSION_MINOR, ARBC_VERSION_PATCH));
    // The decimal-legible 1000-radix of D5, spelled out so a bad encoding cannot hide
    // behind the macro that produced it.
    CHECK(ARBC_VERSION ==
          ARBC_VERSION_MAJOR * 1000000 + ARBC_VERSION_MINOR * 1000 + ARBC_VERSION_PATCH);
    // Comparands are built with the macro, which is the whole point of shipping it:
    // a consumer writes `#if ARBC_VERSION >= ARBC_VERSION_ENCODE(0, 2, 0)`.
    CHECK(ARBC_VERSION >= ARBC_VERSION_ENCODE(0, 1, 0));

    // The string spells the same triple, rendered independently of ARBC_VERSION_STRING.
    char spelled[64];
    std::snprintf(spelled, sizeof spelled, "%d.%d.%d", compiled.major, compiled.minor,
                  compiled.patch);
    CHECK(std::string{spelled} == std::string{ARBC_VERSION_STRING});
  }

  SECTION("all three halves equal the ONE declaration in the top-level CMakeLists") {
    // The independent path. ARBC_TEST_PROJECT_VERSION_* come from PROJECT_VERSION_*, not
    // from the header or the .cpp, so a second literal anywhere in the tree fails here.
    const arbc::Version declared{ARBC_TEST_PROJECT_VERSION_MAJOR, ARBC_TEST_PROJECT_VERSION_MINOR,
                                 ARBC_TEST_PROJECT_VERSION_PATCH};
    CHECK(arbc::compiled_version() == declared);
    CHECK(arbc::linked_version() == declared);
    CHECK(std::string{arbc::linked_version_string()} ==
          std::string{ARBC_TEST_PROJECT_VERSION_STRING});
    CHECK(std::string{ARBC_VERSION_STRING} == std::string{ARBC_TEST_PROJECT_VERSION_STRING});
  }
}
