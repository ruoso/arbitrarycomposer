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

# Links arbc::arbc ALONE. In the Catch2-suppressed configuration it is the entire
# build, and it is what proves the core half of the package stands on its own.
add_executable(core_only core_only.cpp)
target_link_libraries(core_only PRIVATE arbc::arbc)
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
