# The out-of-tree consumer (quality.testing_artifact) -- a REAL foreign project.
#
# It has its own cmake_minimum_required/project(), its own Catch2 acquisition, and
# its own find_package(arbc CONFIG). It include()s nothing from arbitrarycomposer's
# cmake/, names neither its source tree nor its build tree, and is configured as a
# fresh CMake invocation against a STAGED INSTALL PREFIX (run_staged_install.cmake).
# Consuming the build tree instead would prove nothing about the install -- which is
# the exact false confidence this project exists to eliminate (testing_artifact D5).
cmake_minimum_required(VERSION 3.24)

project(
  arbc_consumer
  VERSION 0.1.0
  DESCRIPTION "Out-of-tree proof that arbc-testing is a shipped artifact"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ON  -> the plugin author's path: find_package(arbc COMPONENTS testing), link the
#        conformance suite, run it over a Content of our own.
# OFF -> the embedder's path, driven with -DCMAKE_DISABLE_FIND_PACKAGE_Catch2=ON so
#        Catch2 genuinely CANNOT be found. libarbc must still resolve.
option(ARBC_CONSUMER_WITH_TESTING "Consume the optional arbc `testing` component" ON)

include(CTest)

if(ARBC_CONSUMER_WITH_TESTING)
  # Our own Catch2, as a plugin author would have it: find-first with a pinned
  # FetchContent fallback. Acquired BEFORE find_package(arbc), so the `testing`
  # component's find_dependency(Catch2 3) resolves against it (CMake >= 3.24
  # redirects find_package to already-populated FetchContent content).
  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    FIND_PACKAGE_ARGS 3)
  FetchContent_MakeAvailable(Catch2)

  find_package(arbc CONFIG REQUIRED COMPONENTS testing)
else()
  find_package(arbc CONFIG REQUIRED)
  # The CMake half of 17-internal-components#libarbc-never-requires-arbc-testing:
  # the core package must not so much as DEFINE the suite when nobody asked for it.
  if(TARGET arbc::testing)
    message(
      FATAL_ERROR
        "find_package(arbc) without COMPONENTS testing defined arbc::testing; "
        "libarbc never requires arbc-testing (doc 17:14)")
  endif()
endif()

# The version the CMake PACKAGE advertises (packaging.version_api). find_package set it
# from the installed arbcConfigVersion.cmake, which write_basic_package_version_file
# generated from PROJECT_VERSION -- so it reaches core_only.cpp by a path that touches
# neither the installed header nor the installed archive, and comparing the three there
# is a real three-way agreement rather than a number checked against itself.
if(NOT arbc_VERSION)
  message(FATAL_ERROR "find_package(arbc CONFIG) set no arbc_VERSION; the installed "
                      "package advertises no version (doc 10 § Versioning)")
endif()

# Links arbc::arbc ALONE. In the Catch2-suppressed configuration it is the entire
# build, and it is what proves the core half of the package stands on its own.
add_executable(core_only core_only.cpp)
target_link_libraries(core_only PRIVATE arbc::arbc)
target_compile_definitions(core_only PRIVATE ARBC_PACKAGE_VERSION="${arbc_VERSION}")
add_test(NAME core_only COMMAND core_only)

if(ARBC_CONSUMER_WITH_TESTING)
  # arbc::testing precedes arbc::arbc on the link line: the suite deliberately
  # carries unresolved contract symbols that resolve from the umbrella archive
  # (static-archive link order), exactly as the in-tree drivers order them.
  add_executable(conformance_consumer conformance_consumer.cpp)
  target_link_libraries(conformance_consumer PRIVATE arbc::testing arbc::arbc
                                                     Catch2::Catch2WithMain)
  add_test(NAME conformance_consumer COMMAND conformance_consumer)
endif()

# --- packaging.install: the residual install-metadata surface ---------------------
# pkg-config, CPS, and the plugin install layout, all consumed from the SAME staged
# prefix as the CMake package above. Unconditional (both the WITH_TESTING and core-only
# configures), so the embedder's path exercises them too. GNUInstallDirs resolves the
# same lib/datarootdir the parent's install used (same machine), anchored at the staged
# prefix the parent handed us on CMAKE_PREFIX_PATH.
include(GNUInstallDirs)
list(GET CMAKE_PREFIX_PATH 0 arbc_staged_prefix)
set(arbc_pc_file "${arbc_staged_prefix}/${CMAKE_INSTALL_LIBDIR}/pkgconfig/arbc.pc")
set(arbc_cps_file "${arbc_staged_prefix}/${CMAKE_INSTALL_DATAROOTDIR}/cps/arbc.cps")
set(arbc_plugin_dir "${arbc_staged_prefix}/${CMAKE_INSTALL_LIBDIR}/arbc/plugins")

# pkg-config (acceptance: modversion == PROJECT_VERSION, --cflags/--libs compile+link a
# core-only TU). Guarded on pkg-config being present, the way a real Makefile embedder's
# environment would be. PkgConfig::ARBC_PC carries the .pc's include+link flags; the probe
# gets a BUILD_RPATH to the staged libdir so a shared libarbc resolves at run time (the
# imported arbc::arbc targets get that RPATH for free, a raw -L/-l line does not).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  set(ENV{PKG_CONFIG_PATH} "${arbc_staged_prefix}/${CMAKE_INSTALL_LIBDIR}/pkgconfig")
  pkg_check_modules(ARBC_PC QUIET IMPORTED_TARGET arbc)
  if(NOT TARGET PkgConfig::ARBC_PC)
    message(FATAL_ERROR "pkg-config is present but did not find the installed arbc.pc under "
                        "${arbc_staged_prefix} (PKG_CONFIG_PATH=$ENV{PKG_CONFIG_PATH})")
  endif()
  if(NOT ARBC_PC_VERSION VERSION_EQUAL arbc_VERSION)
    message(FATAL_ERROR "arbc.pc --modversion ${ARBC_PC_VERSION} disagrees with the package "
                        "version ${arbc_VERSION}")
  endif()
  add_executable(pkgconfig_probe pkgconfig_probe.cpp)
  target_link_libraries(pkgconfig_probe PRIVATE PkgConfig::ARBC_PC)
  target_compile_features(pkgconfig_probe PRIVATE cxx_std_20)
  set_target_properties(pkgconfig_probe PROPERTIES BUILD_RPATH "${ARBC_PC_LIBRARY_DIRS}")
  add_test(NAME pkgconfig_probe COMMAND pkgconfig_probe)
else()
  message(STATUS "pkg-config not found; skipping the arbc.pc consumer probe")
endif()

# CPS: well-formed JSON of the expected shape (name, version, the default `arbc`
# component's type mirroring libarbc's linkage, and the optional `testing` component).
# Validated at configure time with string(JSON); the metadata is gated as JSON-only, not
# against a cps-config reader (packaging.install Decision D4).
if(NOT EXISTS "${arbc_cps_file}")
  message(FATAL_ERROR "installed CPS metadata is missing: ${arbc_cps_file}")
endif()
file(READ "${arbc_cps_file}" arbc_cps_json)
string(JSON arbc_cps_name GET "${arbc_cps_json}" name)
if(NOT arbc_cps_name STREQUAL "arbc")
  message(FATAL_ERROR "arbc.cps name is '${arbc_cps_name}', expected 'arbc'")
endif()
string(JSON arbc_cps_version GET "${arbc_cps_json}" version)
if(NOT arbc_cps_version VERSION_EQUAL arbc_VERSION)
  message(FATAL_ERROR "arbc.cps version ${arbc_cps_version} != package version ${arbc_VERSION}")
endif()
get_target_property(arbc_libtype arbc::arbc TYPE)
if(arbc_libtype STREQUAL "SHARED_LIBRARY")
  set(arbc_cps_expected_type "dylib")
else()
  set(arbc_cps_expected_type "archive")
endif()
string(JSON arbc_cps_core_type GET "${arbc_cps_json}" components arbc type)
if(NOT arbc_cps_core_type STREQUAL arbc_cps_expected_type)
  message(FATAL_ERROR "arbc.cps arbc component type is '${arbc_cps_core_type}', expected "
                      "'${arbc_cps_expected_type}' for this libarbc linkage")
endif()
string(JSON arbc_cps_testing ERROR_VARIABLE arbc_cps_testing_err
       GET "${arbc_cps_json}" components testing type)
if(arbc_cps_testing_err)
  message(FATAL_ERROR "arbc.cps has no optional testing component: ${arbc_cps_testing_err}")
endif()
# The arbc component's `requires`, handed to the imposition claim (core_metadata.cpp).
string(JSON arbc_cps_core_requires ERROR_VARIABLE arbc_cps_req_err
       GET "${arbc_cps_json}" components arbc requires)
if(arbc_cps_req_err)
  set(arbc_cps_core_requires "")
endif()
set(arbc_cps_requires_file "${CMAKE_CURRENT_BINARY_DIR}/arbc_cps_arbc_requires.txt")
file(WRITE "${arbc_cps_requires_file}" "${arbc_cps_core_requires}")

# The core-imposition claim: the installed .pc and .cps CORE metadata name nothing beyond
# the conditional zstd -- no Catch2, codec, GPU SDK, or GUI toolkit (doc 10:32-35).
add_executable(core_metadata core_metadata.cpp)
target_compile_features(core_metadata PRIVATE cxx_std_20)
target_compile_definitions(
  core_metadata PRIVATE "ARBC_INSTALLED_PC=\"${arbc_pc_file}\""
                        "ARBC_CPS_ARBC_REQUIRES_FILE=\"${arbc_cps_requires_file}\"")
add_test(NAME core_metadata COMMAND core_metadata)

# Plugin install layout: an ARBC_PLUGIN_PATH host scans the installed plugin dir and loads
# exactly the shipped kind-registering plugins (behavioral counter, doc 17:28).
add_executable(plugin_scan plugin_scan.cpp)
target_link_libraries(plugin_scan PRIVATE arbc::arbc)
target_compile_features(plugin_scan PRIVATE cxx_std_20)
add_test(NAME plugin_scan COMMAND plugin_scan)
set_property(TEST plugin_scan PROPERTY ENVIRONMENT "ARBC_PLUGIN_PATH=${arbc_plugin_dir}")
