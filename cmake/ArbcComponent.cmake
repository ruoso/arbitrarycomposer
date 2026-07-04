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
