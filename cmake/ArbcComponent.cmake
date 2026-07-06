# Component machinery for the levelized object-library architecture
# (design doc 17).
#
# Rules encoded here:
#  - Every component is an OBJECT library named arbc_<name> with alias
#    arbc::<name>. Public headers are FILE_SET HEADERS members living under
#    <component dir>/arbc/<name>/ (single tree, no separate include/).
#  - Component DEPENDS edges are declared here and validated against the
#    doc 17 table by scripts/check_levels.py.
#  - Nothing links object libraries except the umbrella `arbc` target;
#    tests and examples link `arbc`. This avoids duplicate-object pitfalls
#    with transitive object-library linking.

# arbc_add_component(NAME <name>
#                    SOURCES <src...>
#                    PUBLIC_HEADERS <hdr...>
#                    [DEPENDS <component...>])
function(arbc_add_component)
  cmake_parse_arguments(ARG "" "NAME" "SOURCES;PUBLIC_HEADERS;DEPENDS" ${ARGN})
  if(NOT ARG_NAME)
    message(FATAL_ERROR "arbc_add_component: NAME is required")
  endif()
  set(target "arbc_${ARG_NAME}")

  add_library(${target} OBJECT ${ARG_SOURCES})
  add_library(arbc::${ARG_NAME} ALIAS ${target})
  target_sources(
    ${target}
    PUBLIC FILE_SET HEADERS BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" FILES
           ${ARG_PUBLIC_HEADERS})
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_link_libraries(${target} PRIVATE arbc_build_flags)
  foreach(dep IN LISTS ARG_DEPENDS)
    target_link_libraries(${target} PUBLIC "arbc_${dep}")
  endforeach()

  set_property(GLOBAL APPEND PROPERTY ARBC_COMPONENTS "${ARG_NAME}")
  set_property(GLOBAL APPEND PROPERTY ARBC_COMPONENT_DIRS
                                      "${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()

# arbc_component_test(COMPONENT <name> SOURCES <src...>)
#
# Component unit tests live in <component dir>/t/ and link the umbrella
# `arbc` (which carries every component's objects exactly once).
function(arbc_component_test)
  cmake_parse_arguments(ARG "" "COMPONENT" "SOURCES" ${ARGN})
  if(NOT BUILD_TESTING)
    return()
  endif()
  set(target "arbc_${ARG_COMPONENT}_t")
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE arbc Catch2::Catch2WithMain
                                          arbc_build_flags)
  catch_discover_tests(${target})
endfunction()

# arbc_component_bench(COMPONENT <name> SOURCES <src...>)
#
# Peer to arbc_component_test: component benchmarks live in <component dir>/bench/
# and link the umbrella `arbc` plus Google Benchmark. Built ONLY when
# ARBC_BENCHMARKS is ON (the `bench` preset), so dev/asan/coverage builds neither
# fetch Google Benchmark nor compile these -- benchmarks trend, they do not gate
# (doc 16:225-226). The bodies still carry diff coverage via the bench-smoke
# CTest, which drives the shared workload header in the normal test build.
function(arbc_component_bench)
  cmake_parse_arguments(ARG "" "COMPONENT" "SOURCES" ${ARGN})
  if(NOT ARBC_BENCHMARKS)
    return()
  endif()
  set(target "arbc_${ARG_COMPONENT}_bench")
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE arbc benchmark::benchmark arbc_build_flags)
endfunction()

# arbc_add_testing_library(NAME <name>
#                          SOURCES <src...>
#                          PUBLIC_HEADERS <hdr...>
#                          [DEPENDS <component...>])
#
# A standalone STATIC library shipped SEPARATELY from the umbrella `arbc`
# (doc 17:14): the contract conformance suite `arbc-testing`, the first
# non-OBJECT build target in the tree. Unlike a component it is NOT folded into
# `arbc` and is never linked by `libarbc`; downstream test binaries and plugin
# authors link it alongside `arbc` and call `arbc::contract_tests(factory)`.
#
# Crucially the DEPENDS components are consumed for their PUBLIC HEADERS ONLY --
# their include directories are propagated, but their OBJECT files are NOT
# pulled into this archive. Doing so would duplicate every contract symbol
# against the copy the umbrella `arbc` already carries and break the downstream
# link. The suite's unresolved contract symbols (Content vtable, RenderCompletion
# methods, ...) resolve at the consumer's final link against `arbc`.
function(arbc_add_testing_library)
  cmake_parse_arguments(ARG "" "NAME" "SOURCES;PUBLIC_HEADERS;DEPENDS" ${ARGN})
  if(NOT ARG_NAME)
    message(FATAL_ERROR "arbc_add_testing_library: NAME is required")
  endif()
  if(NOT TARGET Catch2::Catch2)
    message(FATAL_ERROR "arbc_add_testing_library: Catch2 not available (BUILD_TESTING off?)")
  endif()

  add_library(${ARG_NAME} STATIC ${ARG_SOURCES})
  add_library(arbc::testing ALIAS ${ARG_NAME})
  target_sources(
    ${ARG_NAME}
    PUBLIC FILE_SET HEADERS BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" FILES
           ${ARG_PUBLIC_HEADERS})
  target_compile_features(${ARG_NAME} PUBLIC cxx_std_20)
  target_link_libraries(${ARG_NAME} PRIVATE arbc_build_flags)
  # Catch2 supplies the assertion runtime the suite drives from inside a
  # caller's TEST_CASE (doc 16 Decision 1); WithMain comes from the consumer.
  target_link_libraries(${ARG_NAME} PUBLIC Catch2::Catch2)
  # Headers only -- see the note above on why objects must not be linked.
  foreach(dep IN LISTS ARG_DEPENDS)
    target_include_directories(
      ${ARG_NAME} PUBLIC
      "$<TARGET_PROPERTY:arbc_${dep},INTERFACE_INCLUDE_DIRECTORIES>")
  endforeach()
endfunction()

# arbc_finalize_library()
#
# Creates the single shipped `arbc` library from all registered components.
# Object libraries are linked PRIVATE (objects flow in, deduplicated by
# CMake); public include directories are re-exported per component so
# consumers of `arbc` see every public header set. Header install
# aggregation onto an installed FILE_SET is deferred until install/packaging
# lands (doc 17).
function(arbc_finalize_library)
  get_property(components GLOBAL PROPERTY ARBC_COMPONENTS)
  get_property(component_dirs GLOBAL PROPERTY ARBC_COMPONENT_DIRS)

  add_library(arbc "${CMAKE_CURRENT_SOURCE_DIR}/version.cpp")
  add_library(arbc::arbc ALIAS arbc)
  target_compile_features(arbc PUBLIC cxx_std_20)
  target_link_libraries(arbc PRIVATE arbc_build_flags)
  foreach(name dir IN ZIP_LISTS components component_dirs)
    target_link_libraries(arbc PRIVATE "arbc_${name}")
    target_include_directories(arbc PUBLIC "$<BUILD_INTERFACE:${dir}>")
  endforeach()
endfunction()
