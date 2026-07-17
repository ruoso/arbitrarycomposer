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
#  - The umbrella and `arbc-testing` are EXPORTED (cmake/ArbcInstall.cmake), so
#    every usage requirement declared here is either install-safe or explicitly
#    confined to the build tree with `$<BUILD_INTERFACE:>`.

include(GNUInstallDirs)

# The umbrella-owned include ROOT that carries arbc/arbc_api.h (the visibility
# macro header, packaging.shared_library_build). Every component's public headers
# `#include <arbc/arbc_api.h>`, but a component object library links no umbrella
# target, so it does not inherit the umbrella's FILE_SET include dir -- it must
# carry this root on its own compile line (added PUBLIC in arbc_add_component, so
# it also flows to dependent components, the umbrella, and arbc-testing). Confined
# to the build interface: the header installs once, from the umbrella's FILE_SET.
get_filename_component(ARBC_API_INCLUDE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../src/api"
                       ABSOLUTE)

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
  # So every component TU can find <arbc/arbc_api.h> (see ARBC_API_INCLUDE_ROOT
  # above). BUILD_INTERFACE only -- the header is installed once by the umbrella.
  target_include_directories(${target}
                             PUBLIC "$<BUILD_INTERFACE:${ARBC_API_INCLUDE_ROOT}>")
  # The export-side define of ARBC_API (arbc/arbc_api.h, packaging.shared_library_build
  # Decision D2). PRIVATE and on EVERY object library, because libarbc's public symbols
  # are compiled here -- not in the umbrella's lone version.cpp -- so every TU that ends
  # up inside libarbc must take the export branch. A consumer/plugin never defines it and
  # takes the import branch. On ELF both branches are visibility("default").
  target_compile_definitions(${target} PRIVATE ARBC_BUILDING)
  foreach(dep IN LISTS ARG_DEPENDS)
    target_link_libraries(${target} PUBLIC "arbc_${dep}")
  endforeach()

  set_property(GLOBAL APPEND PROPERTY ARBC_COMPONENTS "${ARG_NAME}")
  set_property(GLOBAL APPEND PROPERTY ARBC_COMPONENT_DIRS
                                      "${CMAKE_CURRENT_SOURCE_DIR}")
  # Absolute, so arbc_finalize_library() can aggregate every component's public
  # headers onto ONE installable FILE_SET on the umbrella (the object libraries
  # are not exported, so their own FILE_SETs ship nothing).
  foreach(header IN LISTS ARG_PUBLIC_HEADERS)
    set_property(GLOBAL APPEND PROPERTY ARBC_COMPONENT_HEADERS
                                        "${CMAKE_CURRENT_SOURCE_DIR}/${header}")
  endforeach()
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
    message(FATAL_ERROR "arbc_add_testing_library: Catch2 not available")
  endif()

  add_library(${ARG_NAME} STATIC ${ARG_SOURCES})
  add_library(arbc::testing ALIAS ${ARG_NAME})
  # The name the INSTALLED package presents (cmake/ArbcInstall.cmake), chosen to
  # equal the build-tree ALIAS above: in-tree and out-of-tree consumers write the
  # identical `target_link_libraries(... arbc::testing)` line.
  set_target_properties(${ARG_NAME} PROPERTIES EXPORT_NAME testing)
  target_sources(
    ${ARG_NAME}
    PUBLIC FILE_SET HEADERS BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" FILES
           ${ARG_PUBLIC_HEADERS})
  target_compile_features(${ARG_NAME} PUBLIC cxx_std_20)
  # -Wpedantic -Werror is OUR discipline, not a plugin author's: arbc_build_flags
  # stays out of the exported interface (testing_artifact Constraint 7). PRIVATE
  # alone would not do it -- CMake keeps a static library's PRIVATE deps in the
  # interface as $<LINK_ONLY:...>, which install(EXPORT) then demands be exported.
  target_link_libraries(${ARG_NAME} PRIVATE "$<BUILD_INTERFACE:arbc_build_flags>")
  # Catch2 supplies the assertion runtime the suite drives from inside a
  # caller's TEST_CASE (doc 16 Decision 1); WithMain comes from the consumer.
  # A FetchContent-built Catch2 is a target of OUR build and cannot go in an
  # export set, so the installed interface names it from the generated package
  # config instead (arbcConfig.cmake.in, behind `COMPONENTS testing`) and the
  # build-tree link is confined here.
  target_link_libraries(${ARG_NAME} PUBLIC "$<BUILD_INTERFACE:Catch2::Catch2>")
  # Headers only -- see the note above on why objects must not be linked. In the
  # build tree that means the components' source dirs (absolute paths, hence the
  # BUILD_INTERFACE guard install(EXPORT) insists on); in an installed prefix the
  # same headers live under one include root, aggregated onto the umbrella.
  foreach(dep IN LISTS ARG_DEPENDS)
    target_include_directories(
      ${ARG_NAME} PUBLIC
      "$<BUILD_INTERFACE:$<TARGET_PROPERTY:arbc_${dep},INTERFACE_INCLUDE_DIRECTORIES>>")
  endforeach()
  target_include_directories(
    ${ARG_NAME} PUBLIC "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endfunction()

# arbc_finalize_library()
#
# Creates the single shipped `arbc` library from all registered components.
# Object libraries are linked PRIVATE (objects flow in, deduplicated by CMake)
# and every component's public headers are aggregated onto ONE FILE_SET on the
# umbrella -- which is both what gives consumers their include dirs in the build
# tree and what cmake/ArbcInstall.cmake ships to <prefix>/include/arbc/<component>/
# (doc 17:17). The object libraries themselves are build-time machinery and stay
# out of the exported interface (testing_artifact D2): by the time anything links
# libarbc their objects are already archived into it, so an installed consumer has
# nothing left to link against.
function(arbc_finalize_library)
  get_property(components GLOBAL PROPERTY ARBC_COMPONENTS)
  get_property(component_dirs GLOBAL PROPERTY ARBC_COMPONENT_DIRS)
  get_property(component_headers GLOBAL PROPERTY ARBC_COMPONENT_HEADERS)

  # The umbrella's own sources: version.cpp (packaging.version_api) and
  # builtin_kinds.cpp -- the built-in-kind Registry bootstrap doc 17:33/72
  # assigns to L6 (runtime.registry_bootstrap). The bootstrap TU's includes
  # (L4 kind headers, L5 builtin_kind_versions.hpp, the L3 Registry) are exactly
  # the umbrella's "runtime + all" closure, which is why it can live nowhere lower.
  add_library(arbc "${CMAKE_CURRENT_SOURCE_DIR}/version.cpp"
                   "${CMAKE_CURRENT_SOURCE_DIR}/builtin_kinds.cpp")
  add_library(arbc::arbc ALIAS arbc)
  target_compile_features(arbc PUBLIC cxx_std_20)
  target_link_libraries(arbc PRIVATE "$<BUILD_INTERFACE:arbc_build_flags>")
  # The umbrella's own TUs are inside libarbc too, so they take the export
  # branch of ARBC_API like every component object library (D2).
  target_compile_definitions(arbc PRIVATE ARBC_BUILDING)

  # SOVERSION/VERSION off the single-source-of-truth PROJECT_VERSION
  # (packaging.shared_library_build Decision D5). Ignored for the STATIC archive
  # every other lane builds; the soname the BUILD_SHARED_LIBS lane installs.
  # Pre-1.0 the project moves freely and makes NO ABI promise (doc 16:143-145), so
  # SOVERSION is the coarse major (0) -- it deliberately does not claim 0.1<->0.2
  # ABI compatibility; the strict soname-per-incompatible-release discipline
  # arrives with the 1.0 ABI-checking work item (doc 16:147), not here.
  set_target_properties(
    arbc
    PROPERTIES VERSION "${PROJECT_VERSION}"
               SOVERSION "${PROJECT_VERSION_MAJOR}")

  # Shared-libarbc install-relink fix (packaging.shared_library_zstd_shared_link,
  # budget item 4). When libarbc.so links a SYSTEM zstd it links libzstd.so by its
  # absolute path, which makes CMake schedule an install-time RELINK of libarbc.so.
  # That relink is a build-system rule (CMakeFiles/CMakeRelink.dir/...), NOT run by
  # the standalone `cmake --install <dir> --prefix <p>` the install.consumer test
  # (tests/consumer/run_staged_install.cmake) uses to stage -- so staging fails with
  # "cannot find .../CMakeRelink.dir/libarbc.so.*". BUILD_WITH_INSTALL_RPATH builds
  # libarbc.so already carrying its install RPATH, so no relink is scheduled and the
  # standalone install copies the built artifact directly; INSTALL_RPATH_USE_LINK_PATH
  # keeps a *non-system* zstd's directory on that RPATH (both build- and install-tree),
  # so a developer whose zstd lives outside the loader's default search still resolves
  # it at test time and after install. For a system zstd the directory is an implicit
  # link dir and drops out, leaving an empty RPATH that relies on the system loader --
  # the portable release shape. Set ONLY on the umbrella (never on test executables,
  # which need their build-tree RPATH to find libarbc.so.0); inert for the STATIC
  # archive, so the static lanes are byte-unchanged. Guarded on BUILD_SHARED_LIBS to
  # keep the static configuration identical.
  if(BUILD_SHARED_LIBS)
    set_target_properties(
      arbc
      PROPERTIES BUILD_WITH_INSTALL_RPATH ON
                 INSTALL_RPATH_USE_LINK_PATH ON)
  endif()

  # arbc/version.hpp (packaging.version_api, doc 10 § Versioning and the version API).
  # Generated into the BUILD tree from one template, so `project(... VERSION ...)` in the
  # top-level CMakeLists is the single source of truth and a bump is a one-line edit;
  # configure_file also registers the template as a configure dependency, so that bump
  # reconfigures. The header belongs to the UMBRELLA and to no component (doc 17:33 names
  # "version" an umbrella responsibility), so its base dir is the generated root and it
  # installs at <prefix>/include/arbc/version.hpp -- at the include root, not under an
  # arbc/<component>/ subdirectory.
  #
  # That placement is also what keeps "no component may include it" true BY CONSTRUCTION
  # rather than by lint: this BASE_DIRS entry exists only on the umbrella, and components
  # link only their declared arbc_<dep> object libraries and never the umbrella, so a
  # component that wrote `#include <arbc/version.hpp>` would simply fail to find the file.
  # An L4 kind reaching up to L6 is the cycle doc 17:52-55 forbids, and the build already
  # makes it impossible; scripts/check_levels.py needs no new rule (its INCLUDE_RE matches
  # arbc/<component>/..., which arbc/version.hpp deliberately is not).
  set(version_include_root "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set(version_header "${version_include_root}/arbc/version.hpp")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/arbc/version.hpp.in" "${version_header}"
                 @ONLY)

  # arbc/arbc_api.h -- the visibility macro header (packaging.shared_library_build).
  # Hand-written and checked in (NOT generated -- it derives from nothing, Decision
  # D1). Umbrella-owned and installed at the include ROOT (<prefix>/include/arbc/arbc_api.h),
  # exactly like version.hpp. It lives in the umbrella's OWN dedicated source root
  # (src/api/, shared with builtin_kinds.hpp below) so that root's single FILE_SET base
  # dir does not overlap any component's base dir -- a header under two base dirs is a
  # CMake FILE_SET error, which is why it cannot simply sit under src/ alongside the
  # components. The same by-construction rule keeps "no component may reach up to
  # include it" true (this base dir lives only on the umbrella), and its include
  # spelling (arbc/arbc_api.h, at the root, no arbc/<component>/ segment) is not
  # matched by scripts/check_levels.py's INCLUDE_RE.
  set(api_include_root "${CMAKE_CURRENT_SOURCE_DIR}/api")
  set(api_header "${api_include_root}/arbc/arbc_api.h")

  # arbc/builtin_kinds.hpp -- the registry-bootstrap header (runtime.registry_bootstrap,
  # doc 03 § Registry, doc 17:33/72), declaring register_builtin_kinds(Registry&).
  # Hand-written and umbrella-owned exactly like arbc_api.h, sharing its src/api root:
  # it installs at the include ROOT (<prefix>/include/arbc/builtin_kinds.hpp), its
  # include spelling carries no arbc/<component>/ segment, and the same by-construction
  # rule keeps components from reaching up to it -- the bootstrap is an L6 symbol, and
  # an L5- component naming it would be the upward edge doc 17:52-55 forbids.
  set(builtin_kinds_header "${api_include_root}/arbc/builtin_kinds.hpp")

  # One FILE_SET, one BASE_DIR per component (plus the generated root above): CMake
  # derives a $<BUILD_INTERFACE:> include dir from each base dir, so this replaces the
  # per-component target_include_directories the pre-install umbrella carried.
  target_sources(
    arbc
    PUBLIC FILE_SET
           HEADERS
           BASE_DIRS
           ${component_dirs}
           "${version_include_root}"
           "${api_include_root}"
           FILES
           ${component_headers}
           "${version_header}"
           "${api_header}"
           "${builtin_kinds_header}")
  foreach(name IN LISTS components)
    target_link_libraries(arbc PRIVATE "$<BUILD_INTERFACE:arbc_${name}>")
  endforeach()
  target_include_directories(arbc PUBLIC "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
endfunction()
