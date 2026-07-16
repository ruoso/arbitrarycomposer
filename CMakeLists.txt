cmake_minimum_required(VERSION 3.24)

project(
  arbitrarycomposer
  VERSION 0.1.0
  DESCRIPTION "2D scene composer with pluggable layer kinds"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Components are object libraries composed into the single shared/static
# libarbc (design doc 17); objects must be PIC for the shared build.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ArbcComponent)
# The shipped one-line plugin builder (doc 10:47-49): the same file the installed
# arbcConfig.cmake include()s, dogfooded in-tree by the three plugins/* MODULEs.
include(ArbcAddPlugin)

add_library(arbc_build_flags INTERFACE)
target_compile_options(
  arbc_build_flags
  INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive->
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>)

set(ARBC_SANITIZERS
    ""
    CACHE STRING "Comma-separated sanitizer list (e.g. address,undefined)")
set(ARBC_EFFECTIVE_SANITIZERS "${ARBC_SANITIZERS}")
if(ARBC_EFFECTIVE_SANITIZERS STREQUAL "realtime")
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-fsanitize=realtime" ARBC_HAS_REALTIME_SANITIZER)
  if(NOT ARBC_HAS_REALTIME_SANITIZER)
    message(WARNING "RealtimeSanitizer not supported by this compiler; building without sanitizer flags")
    set(ARBC_EFFECTIVE_SANITIZERS "")
  endif()
endif()
if(ARBC_EFFECTIVE_SANITIZERS)
  add_compile_options(-fsanitize=${ARBC_EFFECTIVE_SANITIZERS} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${ARBC_EFFECTIVE_SANITIZERS})
endif()

# Coverage rides arbc_build_flags so only project code is instrumented —
# fetched dependencies (Catch2) stay clean, which also keeps gcovr away
# from _deps object directories.
option(ARBC_COVERAGE "Instrument for coverage" OFF)
if(ARBC_COVERAGE)
  target_compile_options(arbc_build_flags INTERFACE --coverage)
  target_link_options(arbc_build_flags INTERFACE --coverage)
endif()

# RT-safety Layer B (audio.rt_safety, doc 16:70-73,103, Decision D1). The global
# `operator new`/`operator delete` guard and the `RtScope` aborts ride the
# debug-hardened per-push build: a plain Debug build with NO sanitizer and NO
# coverage instrumentation. The sanitizer lanes (asan/tsan/rtsan) own the global
# allocator themselves and RealtimeSanitizer is the Clang-side catch (Layer A);
# the coverage lane is excluded so the build-failing abort branch never has to be
# "covered" (it aborts the instant it runs); Release carries no guard. The define
# rides arbc_build_flags, which every component and test links, so the base TU
# that owns the override and every RT-chain TU see it uniformly.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ARBC_EFFECTIVE_SANITIZERS AND NOT ARBC_COVERAGE)
  target_compile_definitions(arbc_build_flags INTERFACE ARBC_RT_HARDENED)
endif()

# Benchmarks (design doc 15/16 tier 9) are OFF by default so no wall-clock cost
# enters the per-push gate: dev/asan/coverage builds neither fetch Google
# Benchmark nor compile the bench targets. The `bench` preset (Release) turns
# this on; the benchmark bodies still get diff coverage via the bench-smoke
# CTest in the normal build. CI tracked-history wiring is quality.benchmark_history.
option(ARBC_BENCHMARKS "Fetch Google Benchmark and build the benchmark targets" OFF)

# libFuzzer loader harness (serialize.format_tests, doc 16:79). OFF by default so the
# per-push gcc lanes never touch the clang-only -fsanitize=fuzzer targets: the portable
# corpus-replay regression (tests/fuzz_corpus_replay.t.cpp) rides the ordinary dev/asan
# ctest lanes and needs neither this option nor clang. The `fuzz` preset turns this on
# (Clang only, checked in tests/CMakeLists.txt) for the nightly coverage-guided run.
option(ARBC_FUZZER "Build the -fsanitize=fuzzer libFuzzer targets (Clang only)" OFF)

include(CTest)

# Catch2 is acquired UNCONDITIONALLY (testing_artifact D3). It is not merely the
# test framework any more: it is the public assertion runtime of `arbc-testing`, a
# SHIPPED artifact (doc 17:14) that must exist in a packager's -DBUILD_TESTING=OFF
# release build like the two plugin artifacts below it. Doc 10:28's promise is
# unchanged -- an embedder of the core never sees Catch2, because the installed
# package carries the suite behind an optional `testing` component (doc 10, D4).
# Test dependencies still come before src/ so component unit tests can use them.
include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  FIND_PACKAGE_ARGS 3)
FetchContent_MakeAvailable(Catch2)
if(catch2_SOURCE_DIR)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
elseif(Catch2_DIR)
  list(APPEND CMAKE_MODULE_PATH "${Catch2_DIR}")
endif()
include(Catch)

# Google Benchmark: fetched ONLY when benchmarks are enabled (mirrors the Catch2
# declare above, at a pinned tag). Its headers are marked SYSTEM so our
# -Werror -Wpedantic flags do not fire on third-party code.
if(ARBC_BENCHMARKS)
  include(FetchContent)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    FIND_PACKAGE_ARGS)
  FetchContent_MakeAvailable(benchmark)
  if(TARGET benchmark)
    get_target_property(_arbc_bm_incs benchmark INTERFACE_INCLUDE_DIRECTORIES)
    if(_arbc_bm_incs)
      set_target_properties(
        benchmark PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_arbc_bm_incs}")
    endif()
  endif()
endif()

# nlohmann/json: the core's single JSON reader/writer dependency (design doc 08
# serialization; decided in serialize.json_dep, recorded in doc 10 §Dependency
# policy). Header-only, so it imposes NO transitive link/system requirement on an
# embedder of libarbc (doc 10:33-34). Consumed find-first with a pinned
# FetchContent fallback (mirrors the Catch2 declare above), never an in-tree
# vendored copy -- the concrete reading of doc 08 §Dependency note's
# "unproblematic vendoring." Its headers are marked SYSTEM so our -Werror
# -Wpedantic flags do not fire on third-party code. The arbc_serialize component
# (serialize.writer) will link it onto libarbc; for now the serialize.json_dep
# smoke test is its only consumer.
include(FetchContent)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  FIND_PACKAGE_ARGS 3)
FetchContent_MakeAvailable(nlohmann_json)
if(TARGET nlohmann_json)
  get_target_property(_arbc_json_incs nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
  if(_arbc_json_incs)
    set_target_properties(
      nlohmann_json PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_arbc_json_incs}")
  endif()
endif()

# zstd: the core's SECOND (and, on present evidence, last) dependency -- the raster
# tile-blob compressor of design doc 08 Principle 8, decided in doc 10:27 and wired
# in serialize.zstd_dep. Unconditional, exactly like the nlohmann block above and for
# the same reason: it is a dependency of shipped code, not of the tests.
#
# Unlike nlohmann, zstd is a COMPILED C library, so it does not inherit the
# "header-only, therefore no transitive burden" argument (json_dep Constraint 5).
# What holds instead (zstd_dep Decision 2): it is linked PRIVATE onto arbc_serialize
# and `zstd.h` appears in no installed header, so no embedder ever COMPILES against
# zstd; a shared libarbc folds the static zstd's symbols in and exposes nothing
# (hence POSITION_INDEPENDENT_CODE, set globally at the top of this file); a static
# libarbc carries the requirement to its consumer's final link, as every static
# library carries its dependencies. That requirement is now expressed by the exported
# package config, exactly as this note asked (cmake/ArbcInstall.cmake, discharged by
# quality.testing_artifact): find_dependency(zstd 1.5) when the system copy was used,
# nothing when the pinned static build was folded in (its objects are archived into
# libarbc, so the consumer links nothing extra). The shared-libarbc variant of the
# same seam stays with packaging.shared_library_build.
#
# Consumed find-first with a version-pinned FetchContent fallback (doc 10:32-33),
# never an in-tree vendored copy. Two wrinkles a compiled library brings that a
# header-only one did not:
#   - SOURCE_SUBDIR: the zstd repo has NO CMakeLists.txt at its root; its CMake build
#     lives in build/cmake/. Omitting this fails configure with a confusing "does not
#     contain a CMakeLists.txt".
#   - Target-name normalization: a system zstd (zstdConfig.cmake) presents
#     zstd::libzstd_static / zstd::libzstd_shared; the fetched build presents a plain
#     libzstd_static. The arbc_zstd shim below resolves that once, so component
#     CMakeLists name exactly one thing (Decision 3).
#
# Find-first is genuinely find-first, and CI may well resolve a SYSTEM zstd on the
# Linux lanes while Windows builds the pinned tag -- two zstd versions across the
# matrix. That is fine, and it is fine only because the content hash is over
# UNCOMPRESSED tile bytes (doc 08 Principle 8, zstd_dep Decision 1): nothing
# observable depends on which zstd built. The frame format is standardized (RFC 8878)
# and the four functions used (ZSTD_compress/_decompress/_compressBound/_isError) are
# ABI-stable across all of 1.x, so version drift is unobservable -- and letting the
# distro path actually run in CI is valuable, since it is the path downstream
# packagers take. A lane needing determinism can set
# FETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER; no new CI lane, no new pinning machinery.
include(FetchContent)
set(ZSTD_BUILD_PROGRAMS OFF CACHE INTERNAL "")
set(ZSTD_BUILD_TESTS OFF CACHE INTERNAL "")
set(ZSTD_BUILD_CONTRIB OFF CACHE INTERNAL "")
set(ZSTD_BUILD_SHARED OFF CACHE INTERNAL "")
set(ZSTD_BUILD_STATIC ON CACHE INTERNAL "")
set(ZSTD_LEGACY_SUPPORT OFF CACHE INTERNAL "")
FetchContent_Declare(
  zstd
  GIT_REPOSITORY https://github.com/facebook/zstd.git
  GIT_TAG v1.5.7
  SOURCE_SUBDIR build/cmake
  FIND_PACKAGE_ARGS 1.5)
FetchContent_MakeAvailable(zstd)

# One name for the two consumption paths (zstd_dep Decision 3). Resolved ONCE here
# rather than as an if(TARGET ...) cascade scattered across every consumer's
# CMakeLists. The preference order is BUILD_SHARED_LIBS-aware because the two linkage
# shapes have OPPOSITE correct answers (shared_library_zstd_shared_link Decision D1):
#
#   - A STATIC libarbc prefers a system STATIC zstd: folding/relinking a static zstd
#     keeps a shared libarbc from imposing a runtime libzstd.so on its embedder, the
#     transitive burden doc 10:34-35 is actually about.
#   - A SHARED libarbc prefers a system SHARED zstd: folding a system libzstd.a of
#     unknown PIC-ness into libarbc.so links with bad relocations and SIGSEGVs inside
#     ZSTD_decompress. A DT_NEEDED libzstd.so imposes nothing on the embedder's LINK
#     line (the loader resolves it, and libzstd is not a codec/GPU/GUI burden), so it
#     is the correct dependency shape for the .so.
#
# The fetched (non-imported) `libzstd_static` is built PIC (below), so it is a safe
# fold on EITHER shape and stays a fallback ahead of the last-resort system static.
add_library(arbc_zstd INTERFACE)
if(BUILD_SHARED_LIBS)
  if(TARGET zstd::libzstd_shared)
    target_link_libraries(arbc_zstd INTERFACE zstd::libzstd_shared)
  elseif(TARGET zstd::libzstd)
    target_link_libraries(arbc_zstd INTERFACE zstd::libzstd)
  elseif(TARGET libzstd_static)
    target_link_libraries(arbc_zstd INTERFACE libzstd_static)
  elseif(TARGET zstd::libzstd_static)
    target_link_libraries(arbc_zstd INTERFACE zstd::libzstd_static)
  else()
    message(
      FATAL_ERROR "zstd resolved but exposes no known target "
                  "(zstd::libzstd_shared, zstd::libzstd, libzstd_static, zstd::libzstd_static)")
  endif()
else()
  if(TARGET zstd::libzstd_static)
    target_link_libraries(arbc_zstd INTERFACE zstd::libzstd_static)
  elseif(TARGET zstd::libzstd_shared)
    target_link_libraries(arbc_zstd INTERFACE zstd::libzstd_shared)
  elseif(TARGET zstd::libzstd)
    target_link_libraries(arbc_zstd INTERFACE zstd::libzstd)
  elseif(TARGET libzstd_static)
    target_link_libraries(arbc_zstd INTERFACE libzstd_static)
  else()
    message(FATAL_ERROR "zstd resolved but exposes no known target "
                        "(zstd::libzstd_static/_shared, zstd::libzstd, libzstd_static)")
  endif()
endif()

# The fetched (non-imported) static target needs both fixups; an imported target from
# a system zstdConfig.cmake needs neither -- CMake already treats an IMPORTED target's
# include dirs as SYSTEM, and a system library is already built PIC. SYSTEM matters
# because arbc_build_flags carries -Wall -Wextra -Wpedantic -Werror, which must not
# fire on third-party headers.
if(TARGET libzstd_static)
  set_property(TARGET libzstd_static PROPERTY POSITION_INDEPENDENT_CODE ON)
  get_target_property(_arbc_zstd_incs libzstd_static INTERFACE_INCLUDE_DIRECTORIES)
  if(_arbc_zstd_incs)
    set_target_properties(
      libzstd_static PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_arbc_zstd_incs}")
  endif()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
  add_subdirectory(src)
endif()

# The ONE vendored stb-class decode dependency, shared by both out-of-lib image kinds
# (doc 17:228-229; kinds/image.md Decision 6). Added BEFORE its consumers. It links no
# `arbc` target at all, and every consumer links it PRIVATE, so the decode symbols stay in
# the plugin tier and never reach `libarbc` / `arbc-testing` (doc 17 "the codec line").
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/imdec/CMakeLists.txt")
  add_subdirectory(plugins/imdec)
endif()

# The out-of-lib reference plugin (own deps, doc 17:171). Added after src/ so the
# umbrella `arbc` target it links exists; it is a shipped artifact, so it builds
# regardless of BUILD_TESTING (its tests live under tests/, gated there).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/imageseq/CMakeLists.txt")
  add_subdirectory(plugins/imageseq)
endif()

# The out-of-lib STILL image kind, `org.arbc.image` (doc 03:236) -- imageseq's sibling and
# the half of the pixel-persistence split that stores nothing in the document (doc 08
# Principle 3). Same shape and same reasons; its serialize codec, unlike its decoder, lives
# in `runtime` (doc 17 "the codec line is a decoder line").
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/image/CMakeLists.txt")
  add_subdirectory(plugins/image)
endif()

# The out-of-lib reference device backend (own deps; doc 17 "the codec line"
# generalized to device backends). Added after src/ so the umbrella `arbc` target
# (exposing DeviceSink) exists; a shipped artifact, so it builds regardless of
# BUILD_TESTING (its tests live under tests/, gated there).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/miniaudio/CMakeLists.txt")
  add_subdirectory(plugins/miniaudio)
endif()

# The contract conformance suite (arbc-testing) needs Catch2 for its assertion
# runtime and the component targets (declared in src/) for their public-header
# include dirs, so it is added after src/. Like the two plugin artifacts above it
# is a SHIPPED artifact (doc 17:14), so it builds regardless of BUILD_TESTING --
# a packager's release build silently dropping the crown jewel is the bug
# (testing_artifact D3). Its own tests live under tests/, gated there.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/testing/CMakeLists.txt")
  add_subdirectory(testing)
endif()

# The install/export seam for the two shipped artifacts. Last, because it exports
# both `arbc` and `arbc-testing` and so must see them declared.
include(ArbcInstall)
arbc_install()

# Minimal CPack metadata (packaging.install Decision D7): `cpack -G TGZ` rolls the
# installed tree into a tarball as a local convenience. Deliberately NOT a CI gate --
# no design promise rides on a packaging format, and the project's real distribution
# vehicle is the staged `cmake --install` the install.consumer test already exercises.
# include(CPack) at directory scope so the CPACK_* variables take effect; it only reads
# them and generates config files, adding no build/test work.
set(CPACK_PACKAGE_NAME "arbc")
set(CPACK_PACKAGE_VENDOR "arbitrarycomposer")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ")
include(CPack)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
