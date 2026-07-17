# Container image for the local MSVC lanes (orchestrator/driver.py). ci.yml's
# msvc-debug/msvc-shared legs are `runs-on: windows-latest`, which `act` cannot
# replay on Linux -- so instead of GitHub Actions those legs run here: real MSVC
# cl.exe/link.exe under Wine (msvc-wine), driven by cmake/wine-msvc.toolchain.cmake.
#
# Like .github/act/runner.Dockerfile bakes act's extra toolchain into its image,
# this bakes the whole Windows toolchain in: Wine, plus the actual MSVC + Windows
# SDK downloaded by msvc-wine. The image is large (~5-6 GB) and slow to build the
# first time (a ~2 GB Microsoft download), but docker layer caching makes rebuilds
# a no-op until this file changes. Build it with:
#   docker build -t arbitrarycomposer/msvc-wine:latest \
#     -f .github/act/msvc-wine.Dockerfile .github/act
#
# --accept-license below records acceptance of the Microsoft Visual Studio license
# by whoever builds the image (the same flag msvc-wine documents for CI use).
FROM debian:trixie-slim

# wine runs cl.exe/link.exe; msitools+cabextract unpack the MSVC .msi/.cab payloads;
# cmake+ninja drive the build; python3+git+curl are msvc-wine's downloader deps.
# i386 is enabled because the wine package pulls its 32-bit support that way (the
# build itself is x64-only).
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      wine msitools cabextract cmake ninja-build python3 git curl ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Bake in the real MSVC toolchain. --major 17 is VS 2022 (matches the windows-latest
# runner baseline); x64 host + x64 target only, to keep the download lean. install.sh
# finalizes the wrappers, lowercases the SDK headers, boots the Wine prefix (/root/.wine)
# and compiles msvctricks -- i.e. proves cl.exe runs under Wine at image-build time.
ARG MSVC_MAJOR=17
ENV WINEDEBUG=-all
ENV WINEDLLOVERRIDES="mscoree,mshtml="
RUN git clone --depth 1 https://github.com/mstorsjo/msvc-wine.git /opt/msvc-wine \
 && python3 /opt/msvc-wine/vsdownload.py --accept-license --major "${MSVC_MAJOR}" \
      --host-arch x64 --architecture x64 --dest /opt/msvc \
 && /opt/msvc-wine/install.sh /opt/msvc \
 && rm -rf /opt/msvc-wine/.git

# Finalize the Wine prefix. install.sh boots it async (`wineboot &>/dev/null`, no
# wineserver wait), which can commit a half-initialized prefix into the image --
# one with no default %TEMP%, so at run time cl.exe fails with
# "D8037: cannot create temporary il file". An explicit, *waited* wineboot -u
# completes the profile so cl can write its temp files.
RUN WINEDEBUG=-all wine wineboot -u && WINEDEBUG=-all wineserver -w

# cmake/wine-msvc.toolchain.cmake reads this to find the cl/link/lib wrappers.
ENV ARBC_MSVC_WINE_BIN=/opt/msvc/bin/x64

# The repo is bind-mounted at runtime and owned by the host uid, so git inside the
# container would otherwise reject it as dubious-ownership.
RUN git config --system --add safe.directory '*'
