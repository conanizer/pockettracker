# Cross-compile the PocketTracker SDL shell for aarch64 — the PortMaster CFW target arch.
#
# Host: an amd64 Linux with the aarch64 cross toolchain (Ubuntu: `crossbuild-essential-arm64`).
#
#   cmake -S linux -B build/aarch64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=linux/toolchain-aarch64.cmake
#   cmake --build build/aarch64
#   file build/aarch64/pockettracker-sdl        # -> ELF 64-bit ... ARM aarch64
#
# SDL2: linux/CMakeLists.txt calls find_package(SDL2) and FetchContent-builds it STATIC when the host
# has none — which is what happens on a cross box. That yields a self-contained binary, ideal for CI
# build-validation and a qemu-user smoke-test (proven: it boots, decodes media, runs the frame loop and
# exits on SIGTERM under emulation).
#
# ⚠️ For a DEVICE-RUNNABLE artifact, two things change and neither is in this file:
#   1. glibc floor — build on PortMaster's Ubuntu 20.04 base, not a newer host, or the binary demands a
#      glibc newer than the CFWs carry (this validation host is 24.04 / glibc 2.39).
#   2. SDL2 — put an arm64 SDL2 on CMAKE_FIND_ROOT_PATH so find_package succeeds and the shell links it
#      DYNAMICALLY; the device ships its own patched libSDL2 (KMSDRM + ALSA), which is the one to use.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED PT_AARCH64_TRIPLE)
    set(PT_AARCH64_TRIPLE aarch64-linux-gnu)
endif()
set(CMAKE_C_COMPILER   ${PT_AARCH64_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${PT_AARCH64_TRIPLE}-g++)

# Search the toolchain's own sysroot for target libs/headers/packages, the host for build programs.
# PACKAGE=ONLY keeps find_package(SDL2) from picking up a host SDL2, so the FetchContent fallback fires.
set(CMAKE_FIND_ROOT_PATH /usr/${PT_AARCH64_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
