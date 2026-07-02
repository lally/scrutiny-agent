# scrutiny-agent portable Linux build image.
#
# Produces a `scrutiny-agent` for the buildx target platform
# (linux/amd64 or linux/arm64) whose runtime dependencies are just
# glibc >= 2.31 and system libsqlite3 (ubiquitous on every distro; the
# C++ runtime is statically linked). Build context is the repository
# root; the whole dependency graph (gitmanip, GitReviewCore, and every
# Conan dependency) builds from source inside the image with one
# toolchain, so there is no prebuilt-archive ABI reconciliation.
#
# TOOLCHAIN: gitmanip requires C++23. debian:11's stock gcc-10 cannot
# compile C++23, and its libstdc++-10 has no C++23 library. To keep
# the debian:11 / glibc 2.31 portability floor (runs on essentially
# every still-supported distro) we install clang-17 + libc++-17 from
# apt.llvm.org and statically link the C++ runtime (libc++ / libc++abi
# / libgcc). glibc stays dynamic; nothing else does. The Conan profile
# is pinned to clang/17/libc++/cppstd=23 so every dependency shares
# one ABI.
#
# debian:11's apt cmake is 3.18 (predates solid C++23 dialect
# support), so a modern CMake + Ninja come from PyPI manylinux wheels
# (glibc-compatible; the 2.31 floor is kept).

FROM debian:11-slim AS build

ARG AGENT_VERSION=0.1.0
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential git pkg-config perl python3 python3-pip \
        ca-certificates wget gnupg lsb-release software-properties-common \
        libsqlite3-dev \
    && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
    && chmod +x /tmp/llvm.sh && /tmp/llvm.sh 17 \
    && apt-get install -y --no-install-recommends \
        clang-17 lld-17 libc++-17-dev libc++abi-17-dev \
    && ln -sf /usr/bin/clang-17 /usr/bin/clang \
    && ln -sf /usr/bin/clang++-17 /usr/bin/clang++ \
    && ln -sf /usr/bin/ld.lld-17 /usr/bin/ld.lld \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir "conan>=2.0,<3" "cmake>=3.27" ninja \
    && cmake --version && ninja --version

ENV CC=clang-17 CXX=clang++-17

# Conan host profile: clang 17, libc++, C++23 for every from-source
# dependency (ABI-consistent with gitmanip/GitReviewCore/the agent).
RUN conan profile detect --force \
 && PF="$(conan profile path default)" \
 && sed -i 's/^compiler=.*/compiler=clang/' "$PF" \
 && sed -i 's/^compiler\.version=.*/compiler.version=17/' "$PF" \
 && sed -i 's/^compiler\.libcxx=.*/compiler.libcxx=libc++/' "$PF" \
 && ( grep -q '^compiler\.cppstd=' "$PF" \
      && sed -i 's/^compiler\.cppstd=.*/compiler.cppstd=23/' "$PF" \
      || sed -i '/^\[settings\]/a compiler.cppstd=23' "$PF" ) \
 && printf '\n[conf]\ntools.build:compiler_executables={"c":"clang-17","cpp":"clang++-17"}\n' >> "$PF" \
 && echo "----- conan profile -----" && cat "$PF"

# Static C++ runtime so the only runtime deps are glibc + libsqlite3:
# compile against libc++ headers; at link, drop the auto C++ stdlib
# (-nostdlib++) and pull the STATIC archives + static libgcc, via lld.
ENV SCRUTINY_CXX_STATIC="-nostdlib++ -static-libgcc -fuse-ld=lld -l:libc++.a -l:libc++abi.a -lpthread -ldl -lm"

COPY . /src

# `--build="*"` (build EVERYTHING from source, incl. tool_requires like
# b2): Conan Center's prebuilt binaries are built on newer glibc and
# will NOT run on this debian:11 (glibc 2.31) base. Compiling the whole
# graph here is the only way to honor the glibc-2.31 portability floor
# for every transitive binary. Slower, but this is a release artifact.
RUN conan install /src --output-folder=/build --build="*" \
        -s build_type=Release \
    && cmake -S /src -B /build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/build/conan_toolchain.cmake \
        -DSCRUTINY_BUILD_TESTS=OFF \
        -DSCRUTINY_BUILD_TOOLS=OFF \
        -DSCRUTINY_AGENT_VERSION="${AGENT_VERSION}" \
        -DCMAKE_EXE_LINKER_FLAGS="${SCRUTINY_CXX_STATIC}" \
    && cmake --build /build --target scrutiny-agent -j"$(nproc)"

# Minimal export stage: just the binary, picked up via buildx
# --output type=local.
FROM scratch AS export
COPY --from=build /build/agent/scrutiny-agent /scrutiny-agent
