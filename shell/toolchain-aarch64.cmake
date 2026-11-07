# Cross-compile the PocketTracker SDL shell for aarch64 — the PortMaster CFW target arch.
#
# Host: an amd64 Linux with the aarch64 cross toolchain (Ubuntu: `crossbuild-essential-arm64`).
#
#   cmake -S shell -B build/aarch64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=shell/toolchain-aarch64.cmake
#   cmake --build build/aarch64
#   file build/aarch64/pockettracker-sdl        # -> ELF 64-bit ... ARM aarch64
#
# With no arm64 SDL2 on the find root, SDL is fetched and linked statically: a self-contained binary for
# CI and qemu-user, not for a device.
# ⚠️ A device build (build-portmaster.sh) differs in two ways, neither in this file: it builds on
# PortMaster's Ubuntu 20.04 base for the glibc floor, and links the device's own SDL2 dynamically.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED PT_AARCH64_TRIPLE)
    set(PT_AARCH64_TRIPLE aarch64-linux-gnu)
endif()
set(CMAKE_C_COMPILER   ${PT_AARCH64_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${PT_AARCH64_TRIPLE}-g++)

# Target libraries, headers and packages from the toolchain's sysroot, programs from the host.
# PACKAGE=ONLY keeps find_package(SDL2) off the host's SDL2.
set(CMAKE_FIND_ROOT_PATH /usr/${PT_AARCH64_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
