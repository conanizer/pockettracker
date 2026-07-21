cmake_minimum_required(VERSION 3.22.1)

project("pockettracker")

# The engine is C++17 (std::clamp, std::optional, structured bindings). Nothing declared that until
# now: the Android build simply inherited C++17 from the NDK clang's default, so a host compiler with
# an older default (MSVC → C++14) failed on <algorithm>'s std::clamp. State it, and require it.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Oboe is the ANDROID audio backend only (via Prefab). A desktop build provides its own
# shell (SDL/ALSA/JACK) against processLiveBlock() and never sees this.
#
# (Until convergence C2 this read `if(PT_ANDROID_SHELL)` — a variable that existed so the A5 canary
# could be an Android build WITHOUT the parts that need Gradle. C2 retired the canary, so the
# distinction collapsed back to plain ANDROID and the indirection went with it.)
if(ANDROID)
    find_package(oboe REQUIRED CONFIG)
endif()

# ─── Vendored Opus decode stack (libogg + libopus + opusfile) ──────────────────────────────────
# Native Opus decoding so .opus / Opus-in-Ogg samples don't depend on the device's MediaCodec, which
# is broken or absent for Opus on several target ROMs (LineageOS + GammaOS both failed). Decode-only
# in practice — the linker drops the unused encoder via --gc-sections. libogg/libopus build through
# their own CMake (static, no programs/tests, float). opusfile has no CMake of its own, so it's built
# here from just its file-decode sources: no http.c (no OpenSSL) and no config.h (HTTP compiles out).
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)

# CMake 4 REMOVED compatibility with cmake_minimum_required(VERSION < 3.5) — it is a hard configure
# error, not a warning — and the vendored codecs ask for less than that: libogg declares 2.8.12 and
# libopus 3.1. CMAKE_POLICY_VERSION_MINIMUM raises the floor for exactly those projects that ask for
# less, and touches nothing of ours (we declare 3.22.1), so it is scoped to the two lines below and
# unset immediately after. CMake < 3.31 has never heard of the variable and ignores it.
#
# Why this hid for so long: the Android build uses the NDK's bundled CMake 3.22.1, which has no such
# restriction, and neither does ubuntu-latest. It surfaced the moment tools/ started building the
# engine — the windows-latest and macos-latest runners both ship CMake 4 today, and both went red.
# Patching vendor/ would work equally well and be silently undone by the next vendor update.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(vendor/ogg)
add_subdirectory(vendor/opus)
unset(CMAKE_POLICY_VERSION_MINIMUM)
add_library(opusfile STATIC
        vendor/opusfile/src/opusfile.c
        vendor/opusfile/src/info.c
        vendor/opusfile/src/internal.c
        vendor/opusfile/src/stream.c
)
target_include_directories(opusfile
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/opusfile/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/opusfile/src
)
target_link_libraries(opusfile PUBLIC opus ogg)

# ─── Vendored SDL2 — the ANDROID build only (convergence plan C1) ──────────────────────────────
#
# Android is the one target that has to carry SDL in-tree, and it is F-Droid that decides that: their
# builder compiles offline, from source, and rejects prebuilt binaries. So neither of the two ways
# the shell gets SDL is available here — shell/CMakeLists.txt takes the SYSTEM libSDL2 when there is
# one (mandatory on the PortMaster CFWs, whose SDL carries that hardware's display and audio patches)
# and FetchContent's it otherwise, and FetchContent needs a network at configure time.
#
# ⚠️ Two SDL versions in one repo, deliberately, and it is not an inconsistency: the handhelds link
# the DEVICE's SDL against a 2.0.18 compatibility floor, while Android BUNDLES its own and should
# carry a current 2.x. Different linkage models, different version policies. native/vendor/SDL2 is
# 2.30.9 and is documented in native/vendor/SDL2/PT-VENDORING.md; it is regenerated only by
# native/vendor/revendor-sdl2.sh and must never be hand-edited.
#
if(ANDROID)
    # ─── The C/Java version lock, asserted at configure time ───────────────────────────────────
    #
    # SDL's Android support is half C and half Java, and the two halves are version-locked:
    # SDLActivity.java hardcodes the version triple and, in its static initialiser, compares it
    # against the C library's nativeGetVersion(), refusing to start on a mismatch with
    # "SDL C/Java version mismatch". Upstream added that check because mixing the halves is a
    # classic SDL-on-Android failure, and it presents as an inscrutable crash at init.
    #
    # ⚠️ But upstream's check fires on a DEVICE, at runtime, in front of a user. This one fires here,
    # at configure time, on the two files themselves — and it is derived from the DATA rather than
    # resting on whoever next bumps SDL remembering to copy both halves. (revendor-sdl2.sh copies
    # them out of ONE clone, so the drift this catches is a hand-edit, which is exactly the case a
    # habit would not catch.) The names differ across the seam on purpose — upstream calls the third
    # component PATCHLEVEL in C and MICRO_VERSION in Java — so this cannot be a naive text compare.
    set(SDL_VERSION_LOCK_H    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/SDL2/include/SDL_version.h")
    set(SDL_VERSION_LOCK_JAVA "${CMAKE_CURRENT_SOURCE_DIR}/vendor/SDL2/android/java/org/libsdl/app/SDLActivity.java")
    # ⚠️ Without this the guard is checked only when something ELSE happens to force a reconfigure.
    # CMake has no reason to depend on a .java file, and SDL_version.h is not in any target's source
    # list either — so editing the exact two files this check exists to police would leave a cached
    # configure in place and the check would simply not run. A guard that does not fire on the change
    # it guards against is not a guard. (Found by trying to drive this red and noticing it stayed
    # green: the negative control caught the control.)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${SDL_VERSION_LOCK_H}" "${SDL_VERSION_LOCK_JAVA}")
    file(READ "${SDL_VERSION_LOCK_H}"    SDL_VERSION_H_TEXT)
    file(READ "${SDL_VERSION_LOCK_JAVA}" SDL_VERSION_JAVA_TEXT)
    string(REGEX MATCH "#define SDL_MAJOR_VERSION[ \t]+([0-9]+)" _m "${SDL_VERSION_H_TEXT}")
    set(SDL_C_MAJOR "${CMAKE_MATCH_1}")
    string(REGEX MATCH "#define SDL_MINOR_VERSION[ \t]+([0-9]+)" _m "${SDL_VERSION_H_TEXT}")
    set(SDL_C_MINOR "${CMAKE_MATCH_1}")
    string(REGEX MATCH "#define SDL_PATCHLEVEL[ \t]+([0-9]+)"    _m "${SDL_VERSION_H_TEXT}")
    set(SDL_C_PATCH "${CMAKE_MATCH_1}")
    string(REGEX MATCH "SDL_MAJOR_VERSION[ \t]*=[ \t]*([0-9]+)" _m "${SDL_VERSION_JAVA_TEXT}")
    set(SDL_J_MAJOR "${CMAKE_MATCH_1}")
    string(REGEX MATCH "SDL_MINOR_VERSION[ \t]*=[ \t]*([0-9]+)" _m "${SDL_VERSION_JAVA_TEXT}")
    set(SDL_J_MINOR "${CMAKE_MATCH_1}")
    string(REGEX MATCH "SDL_MICRO_VERSION[ \t]*=[ \t]*([0-9]+)" _m "${SDL_VERSION_JAVA_TEXT}")
    set(SDL_J_PATCH "${CMAKE_MATCH_1}")
    set(SDL_C_VERSION    "${SDL_C_MAJOR}.${SDL_C_MINOR}.${SDL_C_PATCH}")
    set(SDL_JAVA_VERSION "${SDL_J_MAJOR}.${SDL_J_MINOR}.${SDL_J_PATCH}")
    # An empty capture would make two blank strings compare EQUAL and the guard pass vacuously —
    # the shape this project keeps paying for — so assert the parse landed before comparing it.
    if(NOT SDL_C_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$" OR
       NOT SDL_JAVA_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR
                "SDL_VERSION_LOCK: could not parse a version out of the vendored SDL2 "
                "(C='${SDL_C_VERSION}' Java='${SDL_JAVA_VERSION}'). If upstream changed how it "
                "declares these, fix the regexes here - do not delete the check.")
    endif()
    if(NOT SDL_C_VERSION STREQUAL SDL_JAVA_VERSION)
        message(FATAL_ERROR
                "SDL_VERSION_LOCK: the vendored SDL2's C and Java halves disagree - "
                "include/SDL_version.h says ${SDL_C_VERSION}, SDLActivity.java says "
                "${SDL_JAVA_VERSION}. SDLActivity refuses to start on a mismatch, so this would be "
                "a crash at app launch. Re-run native/vendor/revendor-sdl2.sh <tag>; never edit "
                "one half by hand.")
    endif()
    message(STATUS "SDL2 ${SDL_C_VERSION} (vendored; C and Java halves agree)")

    # Shared, not static: SDLActivity.java does System.loadLibrary("SDL2") and then calls JNI
    # entry points that live inside it, so libSDL2.so has to exist as its own object in the APK.
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)

    # The same CMake-4 workaround the vendored codecs need, and for the same reason: SDL declares
    # cmake_minimum_required(VERSION 3.0.0...3.5), which CMake 4 rejects outright. The APK builds
    # with the NDK's bundled CMake 3.22.1 (pinned in app/build.gradle.kts) so this cannot bite
    # today - it is here so that configuring native/ with a modern host CMake does not become the
    # way somebody discovers it. See the long note above add_subdirectory(vendor/ogg).
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(vendor/SDL2)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()

# ─── Engine sources: portable core vs platform shell ───────────────────────────────────────────
# ENGINE_CORE_SOURCES is the whole portable engine — no <oboe/*>, <android/*>, or <jni.h>
# anywhere in it (logging goes through the audio-defs.h shim). A desktop/Linux build reuses
# this list verbatim and supplies its own shell in place of ANDROID_PLATFORM_SOURCES.
# TSF_IMPLEMENTATION is defined in soundfont-voice.cpp only; native-audio.cpp is a stub.
set(ENGINE_CORE_SOURCES
        songcore/schema-check.cpp
        soundfont-voice.cpp
        sample-editor.cpp
        audio-engine.cpp
        transient-detector.cpp
        # Native compressed-audio decoders. dr_mp3/dr_flac impl live inside audio-decoders.cpp;
        # stb_vorbis ships as a .c that must be its own translation unit (compiled as C).
        audio-decoders.cpp
        vendor/stb_vorbis/stb_vorbis.c
        kissfft/kiss_fft.c
        kissfft/kiss_fftr.c
        effects/primitives/daisysp/svf.cpp
        effects/primitives/daisysp/overdrive.cpp
        effects/primitives/daisysp/decimator.cpp
        effects/primitives/daisysp/limiter.cpp
        effects/primitives/daisysp/compressor.cpp
        effects/primitives/daisysp/crossfade.cpp
        effects/primitives/daisysp/balance.cpp
        effects/primitives/daisysp/reverbsc.cpp
        effects/modules/dust-chain.cpp
        effects/soundpipe/pareq.c
)

# Android shell: the Oboe stream glue + the JNI facade (+ songcore's JNI shell — the Linux port
# swaps exactly these files and keeps everything above).
set(ANDROID_PLATFORM_SOURCES
        oboe-audio-engine.cpp
        jni-bridge.cpp
        songcore-jni.cpp
)

if(ANDROID)
    add_library(pockettracker SHARED ${ENGINE_CORE_SOURCES} ${ANDROID_PLATFORM_SOURCES})
else()
    # Desktop consumers link the core as a static lib and provide their own shell/main.
    add_library(pockettracker STATIC ${ENGINE_CORE_SOURCES})
endif()

# ─── Compiler flags ────────────────────────────────────────────────────────────────────────────
# Every flag below is GCC/Clang spelling, and the engine is now built by MSVC too (the host static
# lib that tools/ptrender links — see the desktop branch above). cl.exe does not error on an unknown
# `-f…`, it warns D9002 and IGNORES it, so an unguarded list would look fine and quietly do nothing.
# Hence the explicit MSVC arm on each one.
if(MSVC)
    # M_PI is not in MSVC's <cmath> without this — the engine uses it in the LFOs, the SOLA window
    # and the FFT. (GCC/Clang expose it by default.) PUBLIC because sampler-voice.h uses M_PI in a
    # header, so every desktop consumer that includes the engine needs the define too.
    target_compile_definitions(pockettracker PUBLIC _USE_MATH_DEFINES)
    # MSVC's default is already /fp:precise (no contraction, no reassociation) — state it anyway, so
    # a future /fp:fast in a parent scope cannot silently change the DSP's rounding.
    target_compile_options(pockettracker PRIVATE /fp:precise)
else()
    # -ffast-math here is a pure codegen optimisation (reassociation, no NaN/Inf special-casing)
    # for the DSP-heavy TUs. It is NOT the denormal protection: that is
    # setFlushToZeroForCurrentThread() in audio-engine.cpp, which sets the FP-environment
    # FTZ/DAZ bits explicitly on every audio thread (a compile-only -ffast-math would not).
    #
    # ⚠️ The arch test, not an ANDROID test, on purpose — the Linux handhelds are arm and want the
    # same speed. But it now also fires on an arm64 *host* (a macOS CI runner), so the DSP there is
    # NOT bit-comparable with the x86-64 runners. Harmless for the trace goldens (songcore's own TU is
    # IEEE-pinned below, and the engine never touches them) and harmless for ptrender's determinism
    # check (same binary, twice) — but it is exactly why an audio golden cannot be a byte-exact
    # cross-toolchain compare. See the S6b notes in docs/internal/order-of-work.md.
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm")
        target_compile_options(pockettracker PRIVATE -ffast-math)
    endif ()

    # The compressed-audio decoders are float-heavy (stb_vorbis's IMDCT especially). -ffast-math implies
    # -ffinite-math-only, which can corrupt Vorbis decode/setup — so build the decoder TUs WITHOUT it
    # (dr_mp3/dr_flac don't need it either; the perf delta on a one-shot file decode is negligible). The
    # per-source flag is appended after the target's -ffast-math, so -fno-fast-math wins for these files.
    set_source_files_properties(
            audio-decoders.cpp
            vendor/stb_vorbis/stb_vorbis.c
            PROPERTIES COMPILE_OPTIONS "-fno-fast-math"
    )

    # songcore's translation unit is REQUIRED to be IEEE-exact — this is not an optimisation choice.
    # The sequencer's float params (velGain, volGain, pan, PSL/PBN/vibrato rates) are compared against the
    # Kotlin sequencer as raw binary32 BITS (event-schema §5/§6), so any reassociation, and above all fma
    # contraction of `a + b*c` — which aarch64 clang performs BY DEFAULT, -ffast-math or not — changes the
    # rounding and the conformance trace stops matching. Hence both flags, and hence songcore lives in its
    # own TU: pulling its headers into any other source would put that source's math under -ffast-math and
    # silently break the guarantee. (songcore-jni.cpp is Android-only, so this needs no MSVC arm; the host
    # tools restate the same guarantee for their own TUs in tools/CMakeLists.txt.)
    set_source_files_properties(
            songcore-jni.cpp
            PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off"
    )

    # 7.3: explicit optimization level. The NDK leaves Debug at -O0, which makes the real-time audio
    # engine too slow to test reliably on the Miyoo (the variant we test most). Force -O2 in Debug so
    # debug builds run the engine at near-release speed; Release keeps -O3. (We debug via logcat, not
    # native breakpoints, so the loss of -O0 debuggability is a non-issue here.) MSVC keeps CMake's
    # own /Od-Debug, /O2-Release defaults: no real-time constraint applies to a host tool.
    target_compile_options(pockettracker PRIVATE $<IF:$<CONFIG:Debug>,-O2,-O3>)
endif()

# Include paths: root (for audio-defs.h, sampler-voice.h, etc.),
#   effects/soundpipe (so pareq.c finds soundpipe.h), kissfft (for kiss_fft.h).
# PUBLIC, not PRIVATE: a desktop consumer (tools/ptrender, the future SDL shell) links this target and
# must inherit the same include roots — otherwise it would have to restate them and could drift.
target_include_directories(pockettracker PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/effects/soundpipe
        ${CMAKE_CURRENT_SOURCE_DIR}/kissfft
)

# opusfile pulls in libopus + libogg (PUBLIC-linked) for native Opus decoding, and propagates the
# opusfile.h include path. Oboe/android/log are Android-shell dependencies only — the core never
# touches them. libm is required by tsf for math functions on Unix-likes; on Windows the CRT already
# carries it and there is no `m.lib` to find, so asking for it fails the link.
target_link_libraries(pockettracker opusfile)
if(NOT WIN32)
    target_link_libraries(pockettracker m)
endif()
if(ANDROID)
    target_link_libraries(pockettracker
            oboe::oboe
            android
            log
    )
endif()

# ─── pt-ui: the portable UI (Linux-port plan Phase 3) ──────────────────────────────────────────
#
# The screen modules, the software canvas, the 5×5 font, the theme and the cursor-context system —
# the C++ port of the Kotlin `ui/**` + `input/**` layer. It draws into a 640×480 ARGB framebuffer and
# knows nothing about SDL, POSIX, a window or an audio device: the SHELL owns those and scales the
# finished frame (ui/canvas.h). That is what makes `tools/ptshot` able to render a screen headlessly,
# and it is the §12.8 obligation — "write the SDL UI as portable C++ with no Linux-isms, so that
# 'Android on SDL' is a shell swap, not a rewrite".
#
# ⭐ UNCONDITIONAL since convergence phase C2 (2026-07-20). For the whole of the Linux port this
# target was gated `if(NOT ANDROID)`, so that a UI still being written could not grow the APK by a
# byte or break it in any way. That `if` was the one line standing between two UIs and one, and
# deleting it is the convergence.
#
# ✅ **C3 GAVE THIS ARCHIVE A LINKER** — `pockettracker-sdl` at the bottom of this file. Android now
# COMPILES pt-ui *and* SHIPS it, inside that .so. The C2-era warning below is kept because it is
# still true of the mechanism and is the reason the CI check exists, but the specific claim it opens
# with ("nothing links this archive") is no longer the state of the tree.
#
# ⚠️ The shipped app still draws its screens in Compose: the SDL activity is debug-only through
# phases C and D, because the moment the app becomes an SDLActivity, touch stops working and touch is
# Phase D. Two activities, one APK, deliberately — see the pockettracker-sdl block.
#
# ⚠️⚠️ BUT `assembleDebug` DOES NOT BUILD THIS TARGET, AND DELETING THE GATE DID NOT MAKE IT.
# Measured, not assumed: with the gate gone, `:app:externalNativeBuildDebug` printed BUILD SUCCESSFUL
# and "5 actionable tasks: 5 executed" in 29 seconds having compiled pt-ui ZERO times. AGP fills the
# `{LIST_OF_TARGETS_TO_BUILD}` placeholder in its ninja command with the targets it can PACKAGE — the
# .so files — plus whatever ninja pulls in transitively; a static archive nothing links is never
# named. The independent witness is `SDL2main`: libogg/libopus/libopusfile.a are all built (the
# packaged libpockettracker.so links them) while libSDL2main.a has sat unbuilt since C1 for exactly
# the same reason. So un-gating alone is a change that is real in the MODEL (pt-ui appears in
# android_gradle_build.json with a declared libpt-ui.a output) and vacuous in the BUILD.
#
# The coverage therefore comes from CI naming the target explicitly, per ABI, against AGP's own
# configure — .github/workflows/build.yml, "Compile pt-ui with the shipping NDK configuration".
# ⚠️ Do NOT "fix" this by listing targets in app/build.gradle.kts's externalNativeBuild.cmake.targets:
# that field is a WHITELIST, so it would work today and silently stop packaging the first .so a
# future session forgets to add to it. Correctness that rests on every future site remembering is
# already broken — the same rule this project keeps relearning. ✅ C3 removed the question the honest
# way instead: pt-ui now has a real linker (`pockettracker-sdl`), so AGP builds it because it packages
# the thing that needs it — no list to keep in sync.
add_library(pt-ui STATIC
        ui/canvas.cpp
        ui/clipboard.cpp
        ui/input_dispatcher.cpp
        ui/layout.cpp
        ui/lifecycle.cpp
        ui/std_filesystem.cpp
        ui/settings_store.cpp
        ui/project_actions.cpp
        ui/modules/chain_editor.cpp
        ui/modules/confirm_dialog.cpp
        ui/modules/effects_editor.cpp
        ui/modules/eq_editor.cpp
        ui/modules/file_browser.cpp
        ui/modules/fx_helper_overlay.cpp
        ui/modules/groove_editor.cpp
        ui/modules/instrument_editor.cpp
        ui/modules/instrument_pool.cpp
        ui/modules/mixer.cpp
        ui/modules/modulation.cpp
        ui/modules/navigation_map.cpp
        ui/modules/oscilloscope.cpp
        ui/modules/phrase_editor.cpp
        ui/modules/project_editor.cpp
        ui/modules/qwerty_keyboard.cpp
        ui/modules/sample_editor.cpp
        ui/modules/settings_editor.cpp
        ui/modules/song_editor.cpp
        ui/modules/table_editor.cpp
        ui/modules/theme_editor.cpp
)

# ⚠️ std::filesystem needs a separate link library on gcc 8 and older — `stdc++fs`. It was folded
# into libstdc++ proper in gcc 9. The CI runners are far newer (gcc 13, clang 18, MSVC), but a
# PortMaster cross-toolchain can easily be gcc 8, and the failure mode is a wall of undefined
# references at LINK time on a box the port author does not have.
#
# A compiler-version test, NOT a check_cxx_source_compiles probe: the probe does not inherit
# CMAKE_CXX_STANDARD, so it compiles as C++14 on MSVC, fails to find <filesystem>, and concludes
# that MSVC needs `stdc++fs` — a library that does not exist there. (Measured, not guessed: that is
# exactly what the first version of this block did.)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(pt-ui PUBLIC stdc++fs)
endif()

# PUBLIC: pt-ui's headers include songcore's (model.h, effects.h), so anything linking pt-ui
# inherits the engine's include roots too — the same reasoning as pockettracker's own PUBLIC roots.
target_link_libraries(pt-ui PUBLIC pockettracker)

# ui/std_filesystem.cpp asks Windows where `Documents` is — SHGetKnownFolderPath (shell32) and the
# CoTaskMemFree that releases its result (ole32). Stated explicitly rather than inherited: CMake puts
# both in MSVC's default CMAKE_CXX_STANDARD_LIBRARIES, so this line changes nothing there, but a
# MinGW or clang-mingw build links neither by default and would fail with two undefined references
# in a file whose Windows branch nothing else in the tree exercises.
if(WIN32)
    target_link_libraries(pt-ui PUBLIC shell32 ole32)
endif()

# IEEE-exactness, for the same reason songcore-jni.cpp carries it: this target includes songcore
# headers, and the rule stated above is that any TU which does must not have its math reassociated
# or fma-contracted.
#
# ⚠️ S1..S7 carried this on the argument that "the UI does no float math worth the name today, but
# the rule is what keeps that true after someone adds a meter". S8 IS THAT SOMEONE, and it turned
# out to be load-bearing rather than prudent: the EQ editor's FREQ stepping advances until the
# formatted READOUT changes (ui/modules/eq_editor.cpp), so `20 * 1000^(v/255)` and a "%.1f" of it
# decide the value that ends up in the project — and that value is byte-compared against Kotlin's by
# tools/ptinput, over all 256 of them. Reassociated math there is not a slightly different picture;
# it is a red golden.
#
# ⚠️ On Android this is not merely inherited discipline: the APK build sets -ffast-math for
# aarch64 on the `pockettracker` target above, and pt-ui is the other target that includes songcore
# headers. Without these two flags the converged UI would compute its EQ readouts under different
# rounding than the tools that certify them.
if(MSVC)
    target_compile_options(pt-ui PRIVATE /fp:precise)
else()
    target_compile_options(pt-ui PRIVATE -fno-fast-math -ffp-contract=off)
endif()

# ─── The Android SDL app: pt-ui's first real linker (convergence C3) ───────────────────────────
#
# ⭐ **THIS TARGET IS WHAT MAKES C2 REAL.** C2 removed the `if(NOT ANDROID)` gate above and pt-ui
# became an unconditional target — and nothing changed in the build, because AGP only names the
# targets it can PACKAGE plus their transitive dependencies, and a static archive nothing links is
# never compiled. The fix was never to whitelist targets (that is correctness resting on every
# future session remembering, which this project has written down as broken by construction); it was
# to give the archive a consumer that AGP has to build because it packages the result. That is this
# `.so`, and pt-ui arrives inside it as an ordinary link-time dependency.
#
# ⚠️ **TWO APPS IN ONE APK, DELIBERATELY, FOR THE LENGTH OF PHASES C AND D.** `libpockettracker.so`
# above keeps the Compose app running through the JNI facade, and this library is the SDL app beside
# it. The plan requires it: the moment the app becomes an SDLActivity, Compose is gone and touch does
# not work — and touch is Phase D. Release ships the Compose activity until Phase D is device-proven;
# this one is debug-only (see the manifest). Phase E deletes the first and this becomes the app.
#
# The two never coexist in one process: they are separate activities, each constructing its own
# engine and its own OboeAudioEngine. Nothing here is a singleton.
if(ANDROID)
    set(PT_SHELL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shell")

    # app.cpp is the SHARED shell — the same boot sequence, frame loop and teardown the desktop runs,
    # character for character (C0.2). sdl-input/sdl-video are shared too. android-main.cpp is the only
    # file here that is Android's, and it is the twin of shell/main.cpp; set them side by side and the
    # difference is the whole of the port.
    add_library(pockettracker-sdl SHARED
            "${PT_SHELL_DIR}/app.cpp"
            "${PT_SHELL_DIR}/android-main.cpp"
            "${PT_SHELL_DIR}/image.cpp"        # PNG decode (convergence D2): the one stb_image impl TU
            "${PT_SHELL_DIR}/assets.cpp"       # the asset seam (convergence D7): SDL_RWFromFile → APK assets
            "${PT_SHELL_DIR}/skin.cpp"         # the touch-skin textures (convergence D1)
            "${PT_SHELL_DIR}/sdl-input.cpp"
            "${PT_SHELL_DIR}/sdl-touch.cpp"
            "${PT_SHELL_DIR}/sdl-video.cpp"
    )

    # app.h, sdl-input.h, sdl-video.h. The engine's and pt-ui's roots arrive PUBLIC through the link
    # below, so this is the only path that has to be stated.
    target_include_directories(pockettracker-sdl PRIVATE "${PT_SHELL_DIR}")

    # ─── IEEE-exactness: a CORRECTNESS requirement, and one this file was WARNED to restate ───────
    #
    # ⚠️⚠️ shell/CMakeLists.txt says, in as many words: "A future Android entry point linking app.cpp
    # must restate it in ITS build." This is that entry point and this is that restatement.
    #
    # app.cpp #includes songcore/host.h, so the SEQUENCER is compiled into this target. Its float
    # params (velGain/volGain/pan, PSL/PBN/vibrato rates) are what the conformance traces compare as
    # raw binary32 BITS (event-schema §5/§6) — and aarch64 clang contracts `a + b*c` into an fma BY
    # DEFAULT, -ffast-math or not, which rounds once where the JVM rounds twice. The `pockettracker`
    # target above sets -ffast-math for aarch64, and without these two flags this target would inherit
    # nothing to stop it: the converged Android app would sequence differently from the APK every
    # golden was recorded against, on the exact architecture we ship to, and no test in the tree would
    # say so. It would present as a song that plays subtly wrong only on a phone.
    #
    # PER-TARGET, never add_compile_options(): a directory-scope flag would countermand the ENGINE's
    # own -ffast-math policy, which it is entitled to make for itself.
    target_compile_options(pockettracker-sdl PRIVATE -fno-fast-math -ffp-contract=off)

    # pt-ui brings the engine and songcore with it (it links `pockettracker` PUBLIC), so this one line
    # is the whole tracker. SDL2 is the vendored shared library from C1 — the same libSDL2.so
    # SDLActivity.java loads by name, so there is exactly one copy in the APK and the C/Java version
    # lock above governs it. `log` is android-main.cpp's own __android_log_print for the boot path,
    # before the shell's console has anywhere to go.
    target_link_libraries(pockettracker-sdl PRIVATE pt-ui SDL2 log android)
endif()
