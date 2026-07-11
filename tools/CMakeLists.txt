cmake_minimum_required(VERSION 3.22.1)

project("pockettracker-tools" LANGUAGES CXX)

# ─── The songcore conformance harness — host-only, never part of the Android build ─────────────
#
# One tool per layer of the songcore port (linux-port-plan.md §4.3), each byte-comparing C++
# output against a golden recorded from the REAL Kotlin implementation. The JVM tests under
# app/src/test/.../trace/ WRITE /testdata; these READ it back:
#
#   ptroundtrip   S2   .ptp/.pti I/O            the six golden projects re-serialize byte-for-byte
#   ptresolve     S3   timing / FX / traversal  286 unit cases       (units/s3-units.txt)
#   ptplay        S4   the scheduler spine      all 32 event traces  (traces/)
#   ptvoice       S5   the below-seam consumer  77 engine-call cases (units/s5-consumer.txt)
#
# Why this project exists: `./gradlew test` already runs the JVM goldens, which proves the KOTLIN
# sequencer still matches them. Nothing proved the C++ one did — the tools were built by hand, on
# one box, with a scratch build.bat and an MSVC prompt. These targets are that proof, wired to
# ctest so CI runs it on every push.
#
#     cmake -S tools -B tools/build -DCMAKE_BUILD_TYPE=Release
#     cmake --build tools/build --config Release
#     ctest --test-dir tools/build --output-on-failure -C Release
#
# Pass the build type explicitly, as above: the usual "default to Release if CMAKE_BUILD_TYPE is
# empty" guard is not portable, because MSVC's platform module pre-seeds it to Debug (via
# CMAKE_BUILD_TYPE_INIT) while GCC/Clang leave it unset — so the guard silently never fires on
# Windows. Debug is only ever a speed difference here, never a correctness one: the goldens match
# bit-for-bit under both -Od and -O2, which is what /fp:precise below is there to guarantee.
#
# The tools are header-only consumers of native/songcore (relative #includes, no link deps), so
# there is nothing to configure and no engine to build. ptrender — real audio, through the real
# engine — is the one that will need native/'s desktop static lib, and lands with S6b.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── IEEE-exactness: a correctness requirement, not an optimisation choice ─────────────────────
#
# The scheduler's float params (velGain/volGain/pan, PSL/PBN/vibrato rates) are compared against
# the Kotlin sequencer as raw binary32 BITS (event-schema §5/§6). Reassociation — and above all
# contraction of `a + b*c` into an fma, which rounds once where the JVM rounds twice — changes
# those bits, and the traces stop matching. GCC defaults to -ffp-contract=fast and WILL contract,
# so without this the Linux CI runner could go red on a tree that is green on the Windows dev box.
# This is the same guarantee native/CMakeLists.txt gives songcore-jni.cpp; it has to be restated
# here because these translation units are built by a different project entirely.
if(MSVC)
    add_compile_options(/fp:precise)   # MSVC contracts only under /fp:fast — pin it regardless
else()
    add_compile_options(-fno-fast-math -ffp-contract=off)
endif()

# ─── Targets + tests ───────────────────────────────────────────────────────────────────────────

enable_testing()

set(PT_TESTDATA "${CMAKE_CURRENT_SOURCE_DIR}/../testdata")

foreach(tool ptroundtrip ptresolve ptplay ptvoice)
    add_executable(${tool} ${tool}/main.cpp)
endforeach()

# Each tool takes its goldens as argv[1] and exits non-zero on the first byte that differs. The
# paths are absolute, so ctest does not care which directory it is run from.
add_test(NAME s2-project-io    COMMAND ptroundtrip "${PT_TESTDATA}")
add_test(NAME s3-pure-units    COMMAND ptresolve   "${PT_TESTDATA}")
add_test(NAME s4-golden-traces COMMAND ptplay      "${PT_TESTDATA}")
add_test(NAME s5-consumer      COMMAND ptvoice     "${PT_TESTDATA}/units/s5-consumer.txt")
