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

# Benchmarks (design doc 15/16 tier 9) are OFF by default so no wall-clock cost
# enters the per-push gate: dev/asan/coverage builds neither fetch Google
# Benchmark nor compile the bench targets. The `bench` preset (Release) turns
# this on; the benchmark bodies still get diff coverage via the bench-smoke
# CTest in the normal build. CI tracked-history wiring is quality.benchmark_history.
option(ARBC_BENCHMARKS "Fetch Google Benchmark and build the benchmark targets" OFF)

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

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
  add_subdirectory(src)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
