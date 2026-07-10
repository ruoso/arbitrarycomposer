# Runner image for local `act` replays of .github/workflows/ci.yml — used by
# the orchestrator driver's verification chain (orchestrator/driver.py) and
# available for manual runs (`act -j lint`, see repo-root .actrc).
#
# act's medium image ships only gcc/python/git; this layers on the rest of
# the toolchain ci.yml expects from the GitHub-hosted ubuntu-latest runner.
# clang comes from apt.llvm.org because the rtsan lane needs
# `-fsanitize=realtime` (LLVM >= 20) and noble's default clang is 18.
FROM catthehacker/ubuntu:act-latest

RUN . /etc/os-release \
 && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
      -o /etc/apt/keyrings/llvm.asc \
 && echo "deb [signed-by=/etc/apt/keyrings/llvm.asc] http://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-20 main" \
      > /etc/apt/sources.list.d/llvm.list \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      cmake make ccache clang-20 clang-18 \
      libclang-rt-20-dev libclang-rt-18-dev \
 && ln -s /usr/bin/clang-20 /usr/local/bin/clang \
 && ln -s /usr/bin/clang++-20 /usr/local/bin/clang++ \
 && rm -rf /var/lib/apt/lists/*

# ci.yml's lint/coverage jobs `pip install` these per run; pre-installing
# makes those steps no-op resolves instead of downloads. The env var keeps
# pip usable on noble's PEP-668 "externally managed" python, matching the
# GitHub-hosted runner's behavior.
ENV PIP_BREAK_SYSTEM_PACKAGES=1
RUN pip install --no-cache-dir clang-format==19.1.7 gcovr diff-cover
