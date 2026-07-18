cmake_minimum_required(VERSION 3.22.1)

# C as well as C++: ptrender links the engine (add_subdirectory(../native) below), and the vendored
# decoder stack under it — stb_vorbis, kissfft, pareq, libopus, libogg — is C.
project("pockettracker-tools" LANGUAGES C CXX)

# ─── The songcore conformance harness — host-only, never part of the Android build ─────────────
#
# One tool per layer of the songcore port (linux-port-plan.md §4.3), each comparing C++ output
# against a golden recorded from the REAL Kotlin implementation. The JVM tests under
# app/src/test/.../trace/ WRITE /testdata; these READ it back:
#
#   ptroundtrip   S2   .ptp/.pti I/O            the eight golden projects re-serialize byte-for-byte
#   ptresolve     S3   timing / FX / traversal  288 unit cases       (units/s3-units.txt)
#   ptplay        S4   the scheduler spine      all 36 event traces  (traces/)
#   ptvoice       S5   the below-seam consumer  77 engine-call cases (units/s5-consumer.txt)
#   ptrender      S6b  the render, end to end   real audio, through the real engine — and, since
#                      Phase 3 S4, that LIVE PLAYBACK AND THE RENDER PRODUCE THE SAME AUDIO (below)
#   ptrandom      S7   the random FX            12 distributions     (units/s7-random.txt)
#   ptnondet      S7   which projects may be byte-compared at all   (+ a standalone .ptp diagnostic)
#   ptshot   Phase3 S1 the UI, as pixels        any screen → a PNG, headless (no golden yet — see below)
#   ptinput  Phase3 S3 the input layer          21687 cases          (units/p3-input.txt)
#   ptdispatch Ph3 S5  the input DISPATCHER     686 assertions — NO golden; see below and its header
#
# ⚠️ ptdispatch is NOT a conformance tool, and the line matters. Every tool above it byte-compares
# against a golden recorded from the Kotlin code it replaces — they encode what Kotlin DOES. ptdispatch
# is hand-written assertions: it encodes what the author believed Kotlin does, having read it. It exists
# because ptinput measures the MODULES and is structurally blind to the JOIN between them (revert S5's
# START fix and ptinput stays green while ptdispatch goes red), and because the dispatcher has now
# carried a bug into two consecutive sessions. It does not replace the Kotlin-recorded golden S3
# describes; that is still a session of its own.
#
# ⚠️ ptrender's `live == render` check exists because of a hole every other tool in this list is
# STRUCTURALLY blind to. The engine holds state on its own behalf — the mixer, the master bus, reverb,
# delay, the 128-slot EQ bank, every instrument's drive/filter/loop. None of it is an event (ptplay
# cannot see it), none of it is issued by a note (ptvoice cannot see it), and a render pushes it (so
# rendering, which is all ptrender used to do, went through the one path that was correct). The SDL
# shell therefore exported a project correctly and PLAYED it on the engine's factory defaults for four
# sessions — 84% of the bytes differed — with nothing anywhere going red.
#
# ptrandom is the odd one out, and deliberately so: CHA/RND/RNL/ARP-RANDOM draw from a clock-seeded
# RNG, so the Kotlin sequencer has no repeatable event stream to record and there is no byte-exact
# golden to be had. It compares the SUPPORT and the DRAW COUNT exactly (which is where every bug in a
# bounded random draw actually lives) and the SHAPE statistically. That gap is exactly how songcore
# shipped a stub RNG through four green sessions.
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
# Windows. For the four header-only tools Debug is a speed difference and never a correctness one:
# the goldens match bit-for-bit under both -Od and -O2, which is what pt_ieee_exact() guarantees.
#
# Four of the five tools are header-only consumers of native/songcore (relative #includes, no link
# deps) and build in about a second. ptrender is the exception: it renders real audio through the
# real engine, so it builds native/ — the engine plus opus/ogg/opusfile — and dominates the time.

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
#
# PER-TARGET, not add_compile_options(). A directory-scope flag would be inherited by everything
# under add_subdirectory(../native) — the engine and the whole vendored decoder stack — and quietly
# countermand the engine's own -ffast-math policy. The tools' own TUs need it (they #include the
# songcore headers, and aarch64 clang contracts a+b*c into an fma by DEFAULT, -ffast-math or not);
# the engine must be left to decide for itself.
function(pt_ieee_exact target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /fp:precise)   # MSVC contracts only under /fp:fast — pin it regardless
    else()
        target_compile_options(${target} PRIVATE -fno-fast-math -ffp-contract=off)
    endif()
endfunction()

# ─── Targets + tests ───────────────────────────────────────────────────────────────────────────

enable_testing()

set(PT_TESTDATA "${CMAKE_CURRENT_SOURCE_DIR}/../testdata")

foreach(tool ptroundtrip ptresolve ptplay ptvoice ptrandom ptnondet)
    add_executable(${tool} ${tool}/main.cpp)
    pt_ieee_exact(${tool})
endforeach()

# The engine, as a host static lib. native/CMakeLists.txt's non-ANDROID branch: the same portable
# core the APK ships, minus the Oboe/JNI shell. This is the first thing in the repo to build it as
# part of another project — and the reason the CI matrix now compiles the engine on gcc and clang
# too, not just the NDK's clang.
add_subdirectory(../native native)

add_executable(ptrender ptrender/main.cpp)
pt_ieee_exact(ptrender)
target_link_libraries(ptrender PRIVATE pockettracker)

# ptshot — a screen, rendered to a PNG with no window (Phase 3). It links pt-ui, the portable UI, and
# that link is itself the point: pt-ui knows nothing about SDL or a display, so a screen can be drawn
# in CI on a headless runner. It has no ctest yet and that is deliberate — the golden for a pixel diff
# is an Android screenshot, which is a device capture, not something CI can produce. Building it here
# still earns its keep: it compiles the UI on all three toolchains, so the port cannot rot on gcc or
# clang/arm64 while it is being written on MSVC.
add_executable(ptshot ptshot/main.cpp)
pt_ieee_exact(ptshot)
target_link_libraries(ptshot PRIVATE pt-ui)

# ptinput — the input layer, byte-compared against the Kotlin one (Phase 3 S3). Also links pt-ui,
# because what it measures is the modules' own cursor_context()/handle_input() and not a reimplementation
# of them: a harness that re-derived the answer would prove only that the harness agrees with itself.
# Unlike ptshot it DOES have a golden — the input layer's output is text, not pixels, so the JVM can
# record it (P3InputGoldenTest) and CI can compare it.
add_executable(ptinput ptinput/main.cpp)
pt_ieee_exact(ptinput)
target_link_libraries(ptinput PRIVATE pt-ui)

# ptdispatch — the input DISPATCHER (Phase 3 S5). The one tool here that is NOT a conformance tool:
# it takes no golden, because what it checks is the JOIN between the modules, cursor_move and
# navigation, and Kotlin's side of that join is AppInputDispatcher — entangled with ~60 Compose state
# refs and not recordable without a session of its own. Hand-written assertions, transcribed from the
# Kotlin source; see its header for why that is a weaker claim than the other nine make, and for the
# two dispatcher bugs (S3's START, S4's cursor refresh) that ptinput was structurally blind to.
add_executable(ptdispatch ptdispatch/main.cpp)
pt_ieee_exact(ptdispatch)
target_link_libraries(ptdispatch PRIVATE pt-ui)

# ptrender writes WAVs, and the fingerprint goldens live beside the projects they describe.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/render-out")

# Each tool takes its goldens as argv[1] and exits non-zero on the first byte that differs. The
# paths are absolute, so ctest does not care which directory it is run from.
add_test(NAME s2-project-io    COMMAND ptroundtrip "${PT_TESTDATA}")
add_test(NAME s3-pure-units    COMMAND ptresolve   "${PT_TESTDATA}")
add_test(NAME s4-golden-traces COMMAND ptplay      "${PT_TESTDATA}")
add_test(NAME s5-consumer      COMMAND ptvoice     "${PT_TESTDATA}/units/s5-consumer.txt")
add_test(NAME s6-render        COMMAND ptrender    "${PT_TESTDATA}" "${CMAKE_CURRENT_BINARY_DIR}/render-out")
add_test(NAME s7-random        COMMAND ptrandom    "${PT_TESTDATA}")
# Guards a dependency that is otherwise invisible: ptrender asserts two renders of g7-audio are
# BYTE-IDENTICAL, which only holds while g7 contains nothing clock-seeded. Put a CHA in it and s6-render
# fails as "the two renders differ" — true, useless, and hours from the cause. This fails first, by name.
add_test(NAME s7-determinism   COMMAND ptnondet    "${PT_TESTDATA}")
add_test(NAME p3-input         COMMAND ptinput     "${PT_TESTDATA}/units/p3-input.txt")
# No golden argument, because it has none — it asserts, it does not compare. See ptdispatch's header.
add_test(NAME p3-dispatch      COMMAND ptdispatch)
