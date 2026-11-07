# ─── Miyoo Mini / Mini+ (OnionOS) — the DEVICE toolchain ─────────────────────────────────────────
#
# ARM's 8.3-2019.03 arm-linux-gnueabihf with its glibc 2.28 sysroot — what every Miyoo Mini port is
# built with. ⚠️ Not in the repo and not downloaded here (279 MB, `shauninman/miyoomini-toolchain-buildroot`
# releases). Point `MIYOO_TOOLCHAIN` at the directory holding `bin/arm-linux-gnueabihf-gcc`:
#
#   cmake -S shell -B shell/build-miyoo -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=<abs>/shell/toolchain-miyoo.cmake \
#         -DMIYOO_TOOLCHAIN=$HOME/pt-miyoo/tc/miyoomini-toolchain \
#         -DSDL2_DIR=$HOME/pt-miyoo/sdk/lib/cmake/SDL2
#
# ⚠️ The CPU is named: a bare armhf target has no NEON, and vendored opus compiles NEON intrinsics
# whenever it sees ARM. The Mini's SSD202D is a Cortex-A7 with NEON.
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ⚠️ Without this a -DMIYOO_TOOLCHAIN does not reach the try_compile projects, which re-include this
# file with an empty cache, and the compiler check dies on the FATAL_ERROR below.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MIYOO_TOOLCHAIN)

if(NOT MIYOO_TOOLCHAIN)
    if(DEFINED ENV{MIYOO_TOOLCHAIN})
        set(MIYOO_TOOLCHAIN "$ENV{MIYOO_TOOLCHAIN}")
    else()
        message(FATAL_ERROR
            "MIYOO_TOOLCHAIN is not set. Pass -DMIYOO_TOOLCHAIN=<dir containing bin/arm-linux-gnueabihf-gcc>, "
            "unpacked from shauninman/miyoomini-toolchain-buildroot's release tarball.")
    endif()
endif()

set(MIYOO_PREFIX  "${MIYOO_TOOLCHAIN}/bin/arm-linux-gnueabihf-")
set(MIYOO_SYSROOT "${MIYOO_TOOLCHAIN}/arm-linux-gnueabihf/libc")

if(NOT EXISTS "${MIYOO_PREFIX}gcc")
    message(FATAL_ERROR "No compiler at ${MIYOO_PREFIX}gcc — MIYOO_TOOLCHAIN points at the wrong directory.")
endif()

set(CMAKE_C_COMPILER   "${MIYOO_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${MIYOO_PREFIX}g++")
set(CMAKE_STRIP        "${MIYOO_PREFIX}strip")

set(CMAKE_C_FLAGS_INIT   "-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")

# The compiler knows its sysroot already; this keeps CMake's find_* calls inside it.
set(CMAKE_SYSROOT "${MIYOO_SYSROOT}")

# ⚠️ The SDL2 SDK is not in the sysroot: the Mini's SDL2 is a fork with drivers for its chip, the
# device has no system copy, and we bundle it. `SDL2_DIR` names its package config (build-miyoo.sh).
set(CMAKE_FIND_ROOT_PATH "${MIYOO_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# ⚠️ The bundled libSDL2 needs the chip vendor's libmi_* libraries, which only the device has. This binary
# uses none of their symbols; the undefined-symbol check still runs against libSDL2 itself.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--allow-shlib-undefined")
