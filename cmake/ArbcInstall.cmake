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
  # How zstd reaches the consumer depends on BOTH how it was resolved (fetched vs
  # system) AND libarbc's linkage (static vs shared). arbc_serialize's objects (which
  # ARE in libarbc) call ZSTD_compress, so the dependency has to be expressed one of
  # three ways (shared_library_zstd_shared_link Decision D2):
  #
  #   - Fetched zstd (any linkage): built PIC and installed NOWHERE (we do not ship
  #     other people's libraries), so its objects fold into libarbc and the consumer
  #     is asked for nothing at all.
  #   - System zstd + STATIC libarbc: the archive carries its dependencies to the
  #     consumer's final link, so the generated config re-finds zstd and re-attaches
  #     it to the imported arbc::arbc.
  #   - System zstd + SHARED libarbc: libzstd is a private DT_NEEDED of libarbc.so
  #     (the component link is PRIVATE through the umbrella, cmake/ArbcComponent.cmake),
  #     so zstd never enters arbc::arbc's INTERFACE_LINK_LIBRARIES. The exported config
  #     asks for nothing: a find_dependency(zstd) would needlessly impose zstd on every
  #     embedder's link line (doc 10:32-35) and would dangle on a machine that has
  #     libzstd.so (runtime) but no zstdConfig.cmake (dev package).
  #
  # The fetched-vs-system discriminator is the same one the arbc_zstd shim resolves
  # once at the top level: a fetched zstd presents the plain `libzstd_static`, a
  # system one an imported `zstd::*`.
  get_target_property(zstd_link arbc_zstd INTERFACE_LINK_LIBRARIES)
  set(ARBC_ZSTD_FIND_DEPENDENCY "")
  set(ARBC_ZSTD_LINK_INTERFACE "")
  # The pkg-config Requires.private and the CPS `requires` express the SAME conditional
  # zstd dependency as the CMake config above -- one discriminator, three metadata forms
  # that never disagree (packaging.install Decision D2). Empty on the fetched-folded and
  # system-shared lanes; the zstd requirement only on system-zstd + STATIC libarbc.
  set(ARBC_PC_REQUIRES_PRIVATE "")
  set(ARBC_CPS_CORE_REQUIRES "")
  if(zstd_link STREQUAL "libzstd_static")
    target_sources(arbc PRIVATE "$<TARGET_OBJECTS:libzstd_static>")
  elseif(NOT BUILD_SHARED_LIBS)
    set(ARBC_ZSTD_FIND_DEPENDENCY "find_dependency(zstd 1.5)")
    set(ARBC_ZSTD_LINK_INTERFACE
        "set_property(TARGET arbc::arbc APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${zstd_link})")
    set(ARBC_PC_REQUIRES_PRIVATE "libzstd >= 1.5")
    set(ARBC_CPS_CORE_REQUIRES "\"zstd:libzstd\"")
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

  # --- pkg-config (doc 10:43-46, packaging.install) --------------------------
  # The flat convenience form for plain C / Makefile embedders of the CORE. The prefix
  # is relocatable off ${pcfiledir}, so it survives the staged `cmake --install --prefix`
  # (the CMAKE_INSTALL_PREFIX at configure time is not the install-time prefix). The `..`
  # hop count is computed here so a multiarch libdir resolves as well as a plain lib/lib64.
  file(RELATIVE_PATH ARBC_PC_RELDIR_TO_PREFIX
       "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
  configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/arbc.pc.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/arbc.pc" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/arbc.pc"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

  # --- CPS metadata (doc 10:43-46, packaging.install Decision D4) -------------
  # Mirrors the CMake package's OWN shape -- one package, a default `arbc` component and
  # an optional `testing` component -- which pkg-config's flat model cannot. Gated as
  # metadata "as it becomes consumable by tooling" (doc 10:45), so it is generated and
  # validated as well-formed JSON only, never against a `cps-config`-class reader. The
  # `arbc` component's `requires` carries the same conditional zstd and nothing else;
  # Catch2 lives only on the `testing` component, exactly as the CMake COMPONENTS gate.
  set(ARBC_CPS_PREFIX_TOKEN "@prefix@") # literal CPS prefix token, not a configure var
  if(BUILD_SHARED_LIBS)
    set(ARBC_CPS_CORE_TYPE "dylib")
    set(ARBC_CPS_CORE_FILENAME
        "${CMAKE_SHARED_LIBRARY_PREFIX}arbc${CMAKE_SHARED_LIBRARY_SUFFIX}")
  else()
    set(ARBC_CPS_CORE_TYPE "archive")
    set(ARBC_CPS_CORE_FILENAME
        "${CMAKE_STATIC_LIBRARY_PREFIX}arbc${CMAKE_STATIC_LIBRARY_SUFFIX}")
  endif()
  set(ARBC_CPS_TESTING_FILENAME
      "${CMAKE_STATIC_LIBRARY_PREFIX}arbc-testing${CMAKE_STATIC_LIBRARY_SUFFIX}")
  configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/arbc.cps.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/arbc.cps" @ONLY)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/arbc.cps"
          DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/cps")

  # --- VERIFY_INTERFACE_HEADER_SETS (doc 17:223-226, Decision D5) -------------
  # Every public header of both shipped artifacts must compile standalone. Setting the
  # property only CREATES the per-target verify targets (and the aggregate
  # `all_verify_interface_header_sets`); it costs nothing until built, so ordinary builds
  # are untouched and a single CI lane pays for the standalone-compile gate.
  set_target_properties(arbc arbc-testing PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)

  # --- plugin install layout (doc 17:28, Decision D6) ------------------------
  # The three shipped MODULE plugins install to a conventional, ARBC_PLUGIN_PATH-
  # discoverable subdir as loadable modules -- NO EXPORT, so they never enter arbcTargets
  # or the config's imported-target set and never become link targets that would drag a
  # codec/device dep onto an embedder. scan_plugin_path() loads this dir end-to-end.
  set(plugin_dest "${CMAKE_INSTALL_LIBDIR}/arbc/plugins")
  set(arbc_plugins "")
  foreach(plugin arbc-plugin-image arbc-plugin-imageseq arbc-plugin-miniaudio)
    if(TARGET ${plugin})
      list(APPEND arbc_plugins ${plugin})
    endif()
  endforeach()
  if(arbc_plugins)
    # On the shared lane the plugins DT_NEEDED libarbc.so, which CMake links by build-tree
    # path -- scheduling an install-time RELINK the standalone `cmake --install --prefix`
    # the install.consumer test uses cannot run (the same trap arbc_finalize_library fixes
    # for the umbrella). BUILD_WITH_INSTALL_RPATH builds them with their (empty) install
    # RPATH already, so no relink is scheduled; a scanning host has libarbc.so already
    # loaded, so the empty RPATH still resolves the plugin's DT_NEEDED. Inert for the
    # static lanes, so their plugins are byte-unchanged.
    if(BUILD_SHARED_LIBS)
      set_target_properties(${arbc_plugins} PROPERTIES BUILD_WITH_INSTALL_RPATH ON)
    endif()
    install(TARGETS ${arbc_plugins}
            LIBRARY DESTINATION "${plugin_dest}"
            RUNTIME DESTINATION "${plugin_dest}")
  endif()
endfunction()
