# ─── 32-bit ARM (arm-linux-gnueabihf) ────────────────────────────────────────────────────────────
#
# The Miyoo Mini's architecture, and the question this file exists to answer FIRST: is this codebase
# 32-bit clean at all? Everything device-shaped — the SDL2 fork, the panel, the input mapping, the
# 128 MB ceiling — is unmeasured by construction; this only proves the PORTABLE half has no 32-bit
# problem. That is worth having on its own, because it is also the armhf question for PortMaster.
#
# ⚠️ THIS IS NOT THE DEVICE'S TOOLCHAIN. It is Ubuntu's generic armhf cross-compiler against a modern
# glibc, chosen because it needs nothing downloaded and runs the existing ctest suite under
# qemu-user. The real device toolchain is a buildroot/uclibc one (github.com/MiyooMini, and
# shauninman's union-miyoomini-toolchain) and belongs to the phase that produces a shippable ELF.
#
#   cmake -S tools -B tools/build-armhf \
#         -DCMAKE_TOOLCHAIN_FILE=<abs>/shell/toolchain-armhf.cmake -DCMAKE_BUILD_TYPE=Release
#   ctest --test-dir tools/build-armhf
#
# ⚠️ `-L` is not optional. qemu-user resolves the interpreter and every shared library against the
# HOST root unless it is told where the armhf sysroot is, and an amd64 libc found by an armhf binary
# fails in a way that reads like a code bug.
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ⚠️ THE CPU IS NAMED, and it is not decoration. Ubuntu armhf targets a VFPv3-D16 baseline with
# no NEON, and vendored opus compiles its NEON intrinsics unconditionally once it sees an ARM target -
# 270 "inlining failed in call to always_inline vmlaq_f32: target specific option mismatch" errors, in
# somebody else. The Mini is a SigmaStar SSD202D: a Cortex-A7 with NEON, so this is the real target
# rather than a workaround, and pointing the compiler at it makes the vendor tree build as intended.
set(CMAKE_C_FLAGS_INIT   "-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")

set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BEFORE)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# What makes `ctest` work without a device: every test executable is run through the emulator.
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-arm-static -L /usr/arm-linux-gnueabihf)
