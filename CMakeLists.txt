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

add_library(arbc_build_flags INTERFACE)
target_compile_options(
  arbc_build_flags
  INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive->
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>)

set(ARBC_SANITIZERS
    ""
    CACHE STRING "Comma-separated sanitizer list (e.g. address,undefined)")
if(ARBC_SANITIZERS)
  add_compile_options(-fsanitize=${ARBC_SANITIZERS} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${ARBC_SANITIZERS})
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
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ARBC_SANITIZERS AND NOT ARBC_COVERAGE)
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
if(BUILD_TESTING)
  # Test dependencies come before src/ so component unit tests can use them.
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
endif()

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

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
  add_subdirectory(src)
endif()

# The out-of-lib reference plugin (own deps, doc 17:171). Added after src/ so the
# umbrella `arbc` target it links exists; it is a shipped artifact, so it builds
# regardless of BUILD_TESTING (its tests live under tests/, gated there).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/plugins/imageseq/CMakeLists.txt")
  add_subdirectory(plugins/imageseq)
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
# include dirs, so it is added after src/ and only under BUILD_TESTING.
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/testing/CMakeLists.txt")
  add_subdirectory(testing)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
