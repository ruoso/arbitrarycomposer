# The install/export seam for the two shipped artifacts of doc 17:9-18 --
# `libarbc` and the contract conformance suite `arbc-testing` (quality.testing_artifact).
#
# One CMake package, `arbc`. `arbc::arbc` comes by default; `arbc::testing` --
# the crown jewel of doc 16:31-44, "shipped as public API" -- comes only behind
# `find_package(arbc CONFIG REQUIRED COMPONENTS testing)`. That split is the whole
# design (testing_artifact D4): the suite's assertion runtime is Catch2, and an
# embedder of the core must never be made to find Catch2 to use libarbc. The two
# artifacts land in two export FILES for exactly that reason -- one export set per
# file is CMake's rule, and a single file would define `arbc::testing`
# unconditionally, which is the thing doc 17:14 says must not happen.
#
# `arbc-testing` deliberately carries UNRESOLVED contract symbols
# (cmake/ArbcComponent.cmake: its DEPENDS are consumed for headers only), so it is
# meaningless without an installed libarbc to resolve them against -- which is why
# the umbrella install cannot be deferred out of this task and lands here in its
# minimal form. pkg-config, CPS metadata, VERIFY_INTERFACE_HEADER_SETS, CPack, and
# the plugin artifacts' install layout stay with `packaging.install`; the
# BUILD_SHARED_LIBS variant (SOVERSION, ARBC_API) stays with
# `packaging.shared_library_build`.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# arbc_install()
#
# Called ONCE from the top-level CMakeLists, after testing/ has been added.
function(arbc_install)
  set(cmake_dest "${CMAKE_INSTALL_LIBDIR}/cmake/arbc")

  # --- zstd, exactly as CMakeLists.txt's zstd block dictates -----------------
  # A STATIC libarbc carries its dependencies to its consumer's final link, and
  # arbc_serialize's objects (which ARE in libarbc.a) call ZSTD_compress. So the
  # requirement has to be expressed one way or the other:
  #
  #   - System zstd: it is installed on the machine and findable, so the generated
  #     config re-finds it and re-attaches it to the imported arbc::arbc.
  #   - Pinned FetchContent zstd: it is installed NOWHERE (we do not ship other
  #     people's libraries), so its objects are folded into libarbc.a and the
  #     consumer is asked for nothing at all.
  #
  # The discriminator is the same one the arbc_zstd shim resolves once at the top
  # level: a fetched zstd presents the plain `libzstd_static`, a system one an
  # imported `zstd::*`.
  get_target_property(zstd_link arbc_zstd INTERFACE_LINK_LIBRARIES)
  set(ARBC_ZSTD_FIND_DEPENDENCY "")
  set(ARBC_ZSTD_LINK_INTERFACE "")
  if(zstd_link STREQUAL "libzstd_static")
    target_sources(arbc PRIVATE "$<TARGET_OBJECTS:libzstd_static>")
  else()
    set(ARBC_ZSTD_FIND_DEPENDENCY "find_dependency(zstd 1.5)")
    set(ARBC_ZSTD_LINK_INTERFACE
        "set_property(TARGET arbc::arbc APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${zstd_link})")
  endif()

  # --- the two artifacts -----------------------------------------------------
  # `arbc` carries the aggregated FILE_SET of every component's public headers
  # (arbc_finalize_library), so one install(TARGETS) ships the whole public
  # surface at <prefix>/include/arbc/<component>/... (doc 17:17).
  install(
    TARGETS arbc
    EXPORT arbcTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

  install(
    TARGETS arbc-testing
    EXPORT arbcTestingTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

  install(EXPORT arbcTargets NAMESPACE arbc:: FILE arbcTargets.cmake
          DESTINATION "${cmake_dest}")
  install(EXPORT arbcTestingTargets NAMESPACE arbc:: FILE arbcTestingTargets.cmake
          DESTINATION "${cmake_dest}")

  # --- package metadata ------------------------------------------------------
  configure_package_config_file(
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/arbcConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/arbcConfig.cmake"
    INSTALL_DESTINATION "${cmake_dest}")

  # SameMajorVersion off project(arbitrarycomposer VERSION ...). The C++ version
  # API is a separate leaf (packaging.version_api); this reads PROJECT_VERSION and
  # needs no symbol.
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/arbcConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/arbcConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/arbcConfigVersion.cmake"
          DESTINATION "${cmake_dest}")
endfunction()
