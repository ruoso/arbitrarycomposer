# Driver for the `install.consumer` CTest test (quality.testing_artifact, D5).
#
# Stages an install, then configures/builds/runs tests/consumer/ against it TWICE:
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
                          ARBC_CTEST_COMMAND)
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
