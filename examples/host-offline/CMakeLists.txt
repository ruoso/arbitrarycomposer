# The minimal arbc embedding (packaging.examples): render one exact frame of a
# small document to a PNG, the way a batch/export embedder would.
#
# A standalone foreign project, never add_subdirectory'd into arbitrarycomposer's
# build: it sees arbc only through find_package against an installed prefix
# (-DCMAKE_PREFIX_PATH=<prefix>), which is what keeps it an honest embedding
# recipe. CI configures, builds, AND RUNS it against a real staged install
# inside the install.consumer CTest (doc 16:88-90), then validates the PNG it
# writes byte-exactly (tests/consumer/host_example_artifacts.cpp).
#
# The entire dependency surface is arbc::arbc -- no codec, no GPU SDK, no GUI
# toolkit (doc 10:34-35). The PNG writer is example-local code shared with
# host-interactive by relative include (../common/png_writer.hpp).
#
# The [readme-quickstart:*] anchors below mark the region README.md's
# quickstart lifts byte-identically; tests/readme_quickstart.t.cpp enforces
# the sync (claim 16-sdlc-and-quality#readme-quickstart-is-the-shipped-example).
cmake_minimum_required(VERSION 3.24)

project(
  arbc_host_offline
  VERSION 0.1.0
  DESCRIPTION "Minimal arbc embedding: one exact offline frame to PNG"
  LANGUAGES CXX)

# [readme-quickstart:consume]
find_package(arbc CONFIG REQUIRED)

add_executable(host_offline main.cpp)
target_link_libraries(host_offline PRIVATE arbc::arbc)
target_compile_features(host_offline PRIVATE cxx_std_20)
# [/readme-quickstart:consume]
