# The two Linux build containers: the desktop tarball and the PortMaster zip.
#
#     docker build --target linux      -t pockettracker-linux -f shell/Dockerfile shell/
#     docker run --rm -v "$PWD:/src" pockettracker-linux bash /src/shell/build-linux.sh
#
#     docker build --target portmaster -t pockettracker-build -f shell/Dockerfile shell/
#     docker run --rm -v "$PWD:/src" pockettracker-build bash /src/shell/build-portmaster.sh
#
# ⚠️ `ubuntu:20.04` is the glibc floor. glibc symbols are versioned, so a binary built on a newer host
# demands that host's glibc and dies on an older distribution or CFW (`version GLIBC_2.34 not found`)
# before printing a line — while the build stays green. 2.31 is at or below every distribution a
# tester is plausibly on and every Tier-1 CFW. Checked on the artifact:
#
#     readelf -V <binary> | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1     # must be <= 2.31
#
# ⚠️ SDL2 is built here only to link against, never shipped: the distribution or the CFW provides
# libSDL2-2.0.so.0, the copy that knows its display and audio. Hence no backends at all.
# ⚠️ 2.0.18, the first with SDL_GetTicks64(): linking against the lowest SDL the app supports turns a
# newer API into a link error here, not a crash on someone's machine. Raise it only together with
# shell/portmaster/README.md.
#
# CMake comes from Kitware: 20.04 ships 3.16, the tree needs 3.22+.
FROM ubuntu:20.04 AS base

ENV DEBIAN_FRONTEND=noninteractive

ARG CMAKE_VERSION=3.28.3
ARG SDL2_TAG=release-2.0.18
# build-portmaster.sh prints it beside the artifact checks.
ENV PT_SDL2_TAG=${SDL2_TAG}

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ninja-build \
        git \
        pkg-config \
        wget \
        ca-certificates \
        file \
    && wget -qO- "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
       | tar xz -C /opt \
    && ln -s "/opt/cmake-${CMAKE_VERSION}-linux-x86_64/bin/cmake" /usr/local/bin/cmake \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch "${SDL2_TAG}" https://github.com/libsdl-org/SDL.git /opt/sdl2-src

WORKDIR /src

# ─── The desktop: SDL into /usr/local, where find_package looks ───────────────────────────────
FROM base AS linux

RUN cmake -S /opt/sdl2-src -B /tmp/sdl2-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF \
    && cmake --build /tmp/sdl2-build \
    && cmake --install /tmp/sdl2-build \
    && rm -rf /tmp/sdl2-build

# ─── The handheld: aarch64, SDL into the cross sysroot (toolchain-aarch64.cmake's find root) ─────
FROM base AS portmaster

RUN apt-get update && apt-get install -y --no-install-recommends \
        crossbuild-essential-arm64 \
        zip \
        unzip \
    && rm -rf /var/lib/apt/lists/*

RUN cmake -S /opt/sdl2-src -B /tmp/sdl2-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
        -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DCMAKE_INSTALL_PREFIX=/usr/aarch64-linux-gnu \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF \
    && cmake --build /tmp/sdl2-build \
    && cmake --install /tmp/sdl2-build \
    && rm -rf /tmp/sdl2-build
