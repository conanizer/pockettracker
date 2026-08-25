# ─── Miyoo Mini / Mini+ (OnionOS) — the DEVICE toolchain ─────────────────────────────────────────
#
# The toolchain that produces a SHIPPABLE armhf ELF, as opposed to `toolchain-armhf.cmake`, which is
# Ubuntu's generic cross compiler and exists only to answer "is this codebase 32-bit clean?" under
# qemu. The two are not interchangeable and the difference is the SYSROOT:
#
#   toolchain-armhf.cmake   compiles 32-bit, then finds its headers in the HOST's /usr/include and
#                           links against a modern glibc. Fine for a portability smoke test — the
#                           headers are read by the 32-bit compiler, so the type sizes are the
#                           target's — and exactly how a build picks up a header the device has not
#                           got.
#   this file               ARM's 8.3-2019.03 arm-linux-gnueabihf with its own glibc 2.28 sysroot,
#                           which is the toolchain every Miyoo Mini port is built with and the
#                           reason the glibc ceiling asserted in build-miyoo.sh is a READING rather
#                           than a guess.
#
# ⚠️ IT IS NOT IN THE REPO AND IS NOT DOWNLOADED BY THIS FILE — 279 MB, from
# `shauninman/miyoomini-toolchain-buildroot` releases. Point `MIYOO_TOOLCHAIN` at the directory that
# has `bin/arm-linux-gnueabihf-gcc` under it:
#
#   cmake -S shell -B shell/build-miyoo -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=<abs>/shell/toolchain-miyoo.cmake \
#         -DMIYOO_TOOLCHAIN=$HOME/pt-miyoo/tc/miyoomini-toolchain \
#         -DSDL2_DIR=$HOME/pt-miyoo/sdk/lib/cmake/SDL2
#
# ⚠️ THE CPU IS NAMED, and it is not decoration. A bare armhf target is a VFPv3-D16 baseline with no
# NEON, and vendored opus compiles its NEON intrinsics unconditionally once it sees ARM — 270
# "inlining failed in call to always_inline vmlaq_f32: target specific option mismatch" errors, in
# somebody else's code. The Mini is a SigmaStar SSD202D: a Cortex-A7 with NEON, so this is the real
# target rather than a workaround.
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ⚠️ WITHOUT THIS THE -D ON THE COMMAND LINE IS INVISIBLE HERE. CMake re-includes this file inside
# every try_compile project it generates, and those start with an empty cache — so the compiler
# check dies on the FATAL_ERROR below while the real configure line was perfectly correct. Naming
# the variable is what carries it across.
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

# The compiler already knows this sysroot (it was configured --with-sysroot), so this is for CMake's
# benefit: find_package/find_library must look there and nowhere else.
set(CMAKE_SYSROOT "${MIYOO_SYSROOT}")

# ⚠️ THE SDL2 SDK IS ON THE FIND ROOT, NOT IN THE SYSROOT. The device's SDL2 is a fork with drivers
# for this silicon (`mmiyoo`), it is not part of the toolchain, and — unlike every other handheld we
# ship to — there is no system copy on the device either, so we bundle it. `SDL2_DIR` points at the
# tiny package config beside the prebuilt .so; see build-miyoo.sh.
set(CMAKE_FIND_ROOT_PATH "${MIYOO_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# ⚠️ The bundled libSDL2 has the SigmaStar vendor libraries (libmi_gfx, libmi_ao, libmi_sys…) as
# DT_NEEDED, and those live on the DEVICE — nothing here has them. Without this the link fails on
# "libmi_gfx.so, needed by libSDL2-2.0.so.0, not found" even though this binary references not one
# symbol from them. It is the right relaxation and not a papering-over: the same undefined-symbol
# check that would have caught a real mistake still runs against libSDL2 itself.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--allow-shlib-undefined")
