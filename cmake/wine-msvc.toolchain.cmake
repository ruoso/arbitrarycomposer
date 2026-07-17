# CMake toolchain: build the Windows/MSVC lanes with real MSVC cl.exe/link.exe
# under Wine on Linux (msvc-wine: https://github.com/mstorsjo/msvc-wine). This is
# what lets the orchestrator's local CI run the msvc-debug/msvc-shared legs that
# `act` cannot (there is no local Windows container). See scripts/ci-msvc-wine.
#
# The msvc-wine install lives OUTSIDE the tree (it is a multi-GB download of the
# actual MSVC toolchain); point ARBC_MSVC_WINE_BIN at its `bin/x64` directory.
# scripts/ci-msvc-wine sets this and the PATH/env; a manual invocation is:
#
#   ARBC_MSVC_WINE_BIN=/path/to/msvc/bin/x64 \
#   PATH=/path/to/msvc/bin/x64:$PATH \
#   cmake -G Ninja -S . -B build/msvc-wine-dev \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/wine-msvc.toolchain.cmake \
#     -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON

if(DEFINED ENV{ARBC_MSVC_WINE_BIN})
  set(MSVC_WINE_BIN "$ENV{ARBC_MSVC_WINE_BIN}")
else()
  message(FATAL_ERROR
    "wine-msvc.toolchain.cmake: set ARBC_MSVC_WINE_BIN to the msvc-wine bin/x64 "
    "directory (the one holding the cl/link/lib wrapper scripts).")
endif()

set(WINE_EXECUTABLE "$ENV{ARBC_MSVC_WINE}")
if(NOT WINE_EXECUTABLE)
  set(WINE_EXECUTABLE "wine")
endif()

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# Wine has no working mspdbsrv.exe, so /Zi (separate .pdb) fails with
# "C1902: Program database manager mismatch". Force /Z7 (debug info embedded
# in the .obj) by overriding the debug flag strings directly — the CMP0141
# debug-info-format knob does not reach CMake's internal compiler-probe compile.
set(CMAKE_CXX_FLAGS_DEBUG "/Z7 /Ob0 /Od /RTC1" CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_DEBUG   "/Z7 /Ob0 /Od /RTC1" CACHE STRING "" FORCE)

# Point CMake at the Wine wrapper scripts (not the .exe) so INCLUDE/LIB env is set.
set(CMAKE_C_COMPILER   "${MSVC_WINE_BIN}/cl")
set(CMAKE_CXX_COMPILER "${MSVC_WINE_BIN}/cl")
set(CMAKE_RC_COMPILER  "${MSVC_WINE_BIN}/rc")
set(CMAKE_LINKER       "${MSVC_WINE_BIN}/link")
set(CMAKE_AR           "${MSVC_WINE_BIN}/lib")
set(CMAKE_MT           "${MSVC_WINE_BIN}/mt")

# NB: do NOT force CMAKE_CXX_COMPILER_ID here — CMake detects "MSVC 19.44" from the
# cl wrapper on its own, and forcing it skips the probe that sets MSVC_VERSION.

# Run the resulting PE test binaries through Wine so ctest can drive them.
set(CMAKE_CROSSCOMPILING_EMULATOR "${WINE_EXECUTABLE}")

# Cross-compiling: only search the toolchain tree, never the Linux host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
