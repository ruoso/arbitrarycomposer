# The interactive arbc embedding (packaging.examples): a HostViewport bound to
# a Document, an InteractiveRenderer frame loop into one persistent
# caller-owned surface, pan/zoom as camera-transform edits -- driven by a
# deterministic scripted gesture tape so it runs headlessly in CI. The tape is
# the single swap point for a real toolkit's event loop (see README.md).
#
# A standalone foreign project, never add_subdirectory'd into
# arbitrarycomposer's build: it sees arbc only through find_package against an
# installed prefix (-DCMAKE_PREFIX_PATH=<prefix>). CI configures, builds, AND
# RUNS it against a real staged install inside the install.consumer CTest (doc
# 16:88-90), then validates the final-frame PNG it writes byte-exactly
# (tests/consumer/host_example_artifacts.cpp).
#
# The entire dependency surface is arbc::arbc -- deliberately no SDL, no GLFW,
# no windowing toolkit (doc 00:76-79: windowing and input belong to the host;
# doc 10:34-35: embedding the core imposes no GUI toolkit).
cmake_minimum_required(VERSION 3.24)

project(
  arbc_host_interactive
  VERSION 0.1.0
  DESCRIPTION "Interactive arbc embedding: scripted pan/zoom over a HostViewport"
  LANGUAGES CXX)

find_package(arbc CONFIG REQUIRED)

add_executable(host_interactive main.cpp)
target_link_libraries(host_interactive PRIVATE arbc::arbc)
target_compile_features(host_interactive PRIVATE cxx_std_20)
