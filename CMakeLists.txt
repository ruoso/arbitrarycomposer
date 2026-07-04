cmake_minimum_required(VERSION 3.24)

project(
  arbitrarycomposer
  VERSION 0.1.0
  DESCRIPTION "2D scene composer with pluggable layer kinds"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Components are object libraries composed into the single shared/static
# libarbc (design doc 17); objects must be PIC for the shared build.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ArbcComponent)

add_library(arbc_build_flags INTERFACE)
target_compile_options(
  arbc_build_flags
  INTERFACE $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive->
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>)

set(ARBC_SANITIZERS
    ""
    CACHE STRING "Comma-separated sanitizer list (e.g. address,undefined)")
if(ARBC_SANITIZERS)
  add_compile_options(-fsanitize=${ARBC_SANITIZERS} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${ARBC_SANITIZERS})
endif()

option(ARBC_COVERAGE "Instrument for coverage" OFF)
if(ARBC_COVERAGE)
  add_compile_options(--coverage)
  add_link_options(--coverage)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
  add_subdirectory(src)
endif()

include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    FIND_PACKAGE_ARGS 3)
  FetchContent_MakeAvailable(Catch2)
  if(catch2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
  elseif(Catch2_DIR)
    list(APPEND CMAKE_MODULE_PATH "${Catch2_DIR}")
  endif()
  include(Catch)
  add_subdirectory(tests)
endif()
