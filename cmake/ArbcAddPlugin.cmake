# arbc_add_plugin() -- the shipped one-line plugin builder (doc 10:47-49,
# packaging.plugin_helper).
#
# Against the installed package this is the COMPLETE target definition of a
# third-party plugin:
#
#   find_package(arbc CONFIG REQUIRED)
#   arbc_add_plugin(my-plugin SOURCES my_plugin.cpp)
#
# The file ships inside the package: it installs beside arbcConfig.cmake and is
# include()d from it unconditionally, so find_package(arbc CONFIG) alone makes the
# function callable -- no extra include() asked of the consumer, no optional
# component (plugin_helper D2). The SAME file is include()d in-tree (top-level
# CMakeLists), where the three shipped plugins dogfood it; both worlds work because
# the helper touches only `arbc::arbc`, the one identity valid in both -- the
# in-tree ALIAS and the installed imported target (plugin_helper D3).
#
# Deliberately NOT here (plugin_helper Constraint 3 -- these are the host project's
# decisions, or the runtime's, never the helper's):
#   - install / EXPORT / RPATH / output-directory / naming logic: plugins are
#     loadable modules, not link targets (install.md D6); where a host project puts
#     its module is ordinary CMake on the returned target.
#   - compile features: the exported arbc::arbc carries cxx_std_20 PUBLIC
#     (cmake/ArbcComponent.cmake), so setting one here would duplicate an exported
#     usage requirement.
#   - plugin ABI versioning: none exists in v1 (doc 10:92-98); the helper mints no
#     version defines and no negotiation stubs.
#   - runtime discovery: ARBC_PLUGIN_PATH is host/runtime policy (doc 10:50-53);
#     the helper is build-side only.

# arbc_add_plugin(<name> SOURCES <srcs...> [LINK_LIBRARIES <extra...>])
#
# Defines <name> as a MODULE library -- dlopen'd by a PluginHost, never linked
# against -- compiling <srcs...> and linking arbc::arbc PRIVATE. LINK_LIBRARIES is
# the seam for a plugin's own private dependencies (a codec, a device backend; the
# in-tree call sites pass their impl libraries and warning flags through it). The
# result is a normal CMake target: anything beyond the arbc link -- installing it,
# renaming it, extra compile options -- is ordinary CMake on <name>.
function(arbc_add_plugin name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;LINK_LIBRARIES")
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "arbc_add_plugin(${name}): unknown arguments: "
                        "${arg_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT arg_SOURCES)
    message(FATAL_ERROR "arbc_add_plugin(${name}): SOURCES is required")
  endif()

  # MODULE, not SHARED: the artifact exists to be loaded through the doc-03
  # extern "C" entry point; nothing ever links it (install.md D6).
  add_library(${name} MODULE ${arg_SOURCES})

  # PRIVATE, both halves: the module has no link interface to publish, and a
  # plugin's own dependencies must never leak toward its host (doc 17 "the codec
  # line"). arbc::arbc brings the public headers and cxx_std_20 as exported usage
  # requirements -- the helper adds nothing else.
  target_link_libraries(${name} PRIVATE arbc::arbc ${arg_LINK_LIBRARIES})
endfunction()
