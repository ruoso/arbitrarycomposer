# Driver for the `install.consumer` CTest test (quality.testing_artifact, D5).
#
# Stages an install, builds examples/plugin-template against it (the one-line
# arbc_add_plugin() proof, packaging.plugin_helper), builds AND RUNS the two
# host-embedding examples (examples/host-offline, examples/host-interactive --
# packaging.examples, doc 16:88-90), then configures/builds/runs tests/consumer/
# against the same prefix TWICE:
#
#   1. the plugin author's path -- find_package(arbc CONFIG REQUIRED COMPONENTS testing),
#      link arbc::arbc + arbc::testing, run arbc::contract_tests over a foreign Content;
#   2. the embedder's path -- find_package(arbc CONFIG REQUIRED) with
#      -DCMAKE_DISABLE_FIND_PACKAGE_Catch2=ON, so the "libarbc never requires
#      arbc-testing" half is a real negative rather than an accident of the CI image.
#
# A CTest test rather than a CI-only shell step, so scripts/gate and per-push CI run
# the same check off one registration (doc 16:97-100): a check that only exists in
# ci.yml is one you find out you broke after pushing.

cmake_minimum_required(VERSION 3.24)

foreach(required IN ITEMS ARBC_BUILD_DIR ARBC_CONSUMER_SRC ARBC_STAGE_DIR
                          ARBC_CONSUMER_BUILD_DIR ARBC_CORE_ONLY_BUILD_DIR
                          ARBC_CTEST_COMMAND ARBC_TEMPLATE_SRC ARBC_TEMPLATE_BUILD_DIR
                          ARBC_MODULE_SUFFIX ARBC_HOST_OFFLINE_SRC ARBC_HOST_OFFLINE_BUILD_DIR
                          ARBC_HOST_INTERACTIVE_SRC ARBC_HOST_INTERACTIVE_BUILD_DIR)
  if(NOT ${required})
    message(FATAL_ERROR "run_staged_install: -D${required}=... is required")
  endif()
endforeach()

# A fresh prefix every run: a public header that silently stopped being installed
# must surface as a missing-include build failure, not as a leftover from last time.
file(REMOVE_RECURSE "${ARBC_STAGE_DIR}")

message(STATUS "install.consumer: staging install into ${ARBC_STAGE_DIR}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${ARBC_BUILD_DIR}" --prefix
                        "${ARBC_STAGE_DIR}" COMMAND_ERROR_IS_FATAL ANY)

# The consumer links a STATIC libarbc, so it has to be compiled and linked with the
# same instrumentation that archive carries -- otherwise the asan/tsan/rtsan/coverage
# lanes fail at link on the sanitizer runtime's symbols. That is the only thing the
# parent build imposes; the consumer's find_package, its Catch2, and its targets are
# entirely its own (testing_artifact Constraint 8).
set(common_args
    -G "${ARBC_GENERATOR}"
    "-DCMAKE_BUILD_TYPE=${ARBC_BUILD_TYPE}"
    "-DCMAKE_CXX_COMPILER=${ARBC_CXX_COMPILER}"
    "-DCMAKE_CXX_FLAGS=${ARBC_CXX_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=${ARBC_LINKER_FLAGS}"
    "-DCMAKE_PREFIX_PATH=${ARBC_STAGE_DIR}")
if(ARBC_MAKE_PROGRAM)
  list(APPEND common_args "-DCMAKE_MAKE_PROGRAM=${ARBC_MAKE_PROGRAM}")
endif()
# Point the consumer's OWN Catch2 FetchContent at the copy the parent already
# downloaded. That is a stock CMake cache variable -- the same thing a CI source
# cache would set -- not a coupling to arbitrarycomposer, and it keeps a test off the
# network. When the parent resolved a system Catch2 instead, this is empty and the
# consumer's find-first finds that same one.
if(ARBC_CATCH2_SOURCE_DIR)
  list(APPEND common_args "-DFETCHCONTENT_SOURCE_DIR_CATCH2=${ARBC_CATCH2_SOURCE_DIR}")
endif()

# --- packaging.plugin_helper: the third-party plugin template ---------------------
# examples/plugin-template is a standalone foreign project whose ONLY target-defining
# lines are find_package(arbc CONFIG REQUIRED) + one arbc_add_plugin() call.
# Configuring and building it against the staged prefix -- on every lane, including
# the shared ones where a MODULE's link against a shared libarbc can actually go
# wrong -- is what keeps the shipped helper honest against the INSTALLED package
# (doc 10:47-49). The consumer's plugin_template_load then loads the produced module
# through the production PluginHost.
message(STATUS "install.consumer: third-party plugin template (arbc_add_plugin)")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${ARBC_TEMPLATE_SRC}" -B "${ARBC_TEMPLATE_BUILD_DIR}"
          ${common_args} COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${ARBC_TEMPLATE_BUILD_DIR}"
                        COMMAND_ERROR_IS_FATAL ANY)

# The built module's path, handed into both consumer configures the way plugin paths
# reach in-tree tests (a -D compile-definition seam). The platform's module
# prefix/suffix come from the parent configure -- script mode does not know them --
# and the prefix may be legitimately empty (Windows), so only the suffix is in the
# required list above.
set(arbc_template_module
    "${ARBC_TEMPLATE_BUILD_DIR}/${ARBC_MODULE_PREFIX}template-plugin${ARBC_MODULE_SUFFIX}")
if(NOT EXISTS "${arbc_template_module}")
  message(FATAL_ERROR "install.consumer: the template build produced no module at "
                      "${arbc_template_module}")
endif()
list(APPEND common_args "-DARBC_TEMPLATE_MODULE=${arbc_template_module}")

# --- packaging.examples: the two host-embedding examples ---------------------------
# Each is a standalone foreign project (find_package(arbc CONFIG REQUIRED), never
# add_subdirectory'd) configured and built against the staged prefix, then RUN --
# doc 16:88-90's tier is "compiles and runs in CI", and a non-zero exit fails this
# test. The PNG each writes is handed into both consumer configures below, where
# host_example_artifacts.cpp validates it byte-exactly. On the Windows shared lane
# the executables import arbc.dll from the staged bin/, which the outer test's
# ENVIRONMENT_MODIFICATION already prepended to PATH (msvc refinement D4); that env
# propagates through this driver into these child processes.
foreach(example IN ITEMS OFFLINE INTERACTIVE)
  string(TOLOWER "${example}" example_lower)
  set(example_src "${ARBC_HOST_${example}_SRC}")
  set(example_build "${ARBC_HOST_${example}_BUILD_DIR}")
  message(STATUS "install.consumer: host example (host-${example_lower})")
  execute_process(COMMAND "${CMAKE_COMMAND}" -S "${example_src}" -B "${example_build}"
                          ${common_args} COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${example_build}"
                          COMMAND_ERROR_IS_FATAL ANY)
  # The executable suffix comes from the parent configure (script mode does not know
  # it) and is legitimately empty everywhere but Windows.
  set(example_exe "${example_build}/host_${example_lower}${ARBC_EXE_SUFFIX}")
  if(NOT EXISTS "${example_exe}")
    message(FATAL_ERROR "install.consumer: the host-${example_lower} build produced no "
                        "executable at ${example_exe}")
  endif()
  execute_process(COMMAND "${example_exe}" "${example_build}/out.png"
                          COMMAND_ERROR_IS_FATAL ANY)
  list(APPEND common_args "-DARBC_HOST_${example}_PNG=${example_build}/out.png")
endforeach()

message(STATUS "install.consumer: plugin-author path (COMPONENTS testing)")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${ARBC_CONSUMER_SRC}" -B "${ARBC_CONSUMER_BUILD_DIR}"
          ${common_args} -DARBC_CONSUMER_WITH_TESTING=ON COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${ARBC_CONSUMER_BUILD_DIR}"
                        COMMAND_ERROR_IS_FATAL ANY)
execute_process(
  COMMAND "${ARBC_CTEST_COMMAND}" --test-dir "${ARBC_CONSUMER_BUILD_DIR}"
          --output-on-failure COMMAND_ERROR_IS_FATAL ANY)

message(STATUS "install.consumer: embedder path (no Catch2, no testing component)")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${ARBC_CONSUMER_SRC}" -B "${ARBC_CORE_ONLY_BUILD_DIR}"
          ${common_args} -DARBC_CONSUMER_WITH_TESTING=OFF
          -DCMAKE_DISABLE_FIND_PACKAGE_Catch2=ON COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${ARBC_CORE_ONLY_BUILD_DIR}"
                        COMMAND_ERROR_IS_FATAL ANY)
execute_process(
  COMMAND "${ARBC_CTEST_COMMAND}" --test-dir "${ARBC_CORE_ONLY_BUILD_DIR}"
          --output-on-failure COMMAND_ERROR_IS_FATAL ANY)
