# A copy-me third-party arbc plugin project (packaging.plugin_helper).
#
# This directory is the executable documentation of doc 10:47-49: against an
# INSTALLED arbc package, the complete build of a loadable plugin is find_package +
# one arbc_add_plugin() call -- the two lines below. It is a standalone project,
# never add_subdirectory'd into arbitrarycomposer's build; CI compiles it against a
# real staged install prefix and loads the produced module through the production
# PluginHost (tests/consumer/plugin_template_load.cpp inside the install.consumer
# CTest), which is what keeps the helper honest against the installed package.
#
# To start your own plugin: copy this directory, rename the target, point SOURCES
# at your registration TU, and hand your arbc install prefix to CMake via
# -DCMAKE_PREFIX_PATH=<prefix>. The helper returns a normal CMake target: install
# destination, output naming, RPATH, and any private dependencies of your own
# (LINK_LIBRARIES) are your project's ordinary CMake decisions.
cmake_minimum_required(VERSION 3.24)

project(
  arbc_plugin_template
  VERSION 0.1.0
  DESCRIPTION "Template for a third-party arbc plugin"
  LANGUAGES CXX)

find_package(arbc CONFIG REQUIRED)

arbc_add_plugin(template-plugin SOURCES template_plugin.cpp)
