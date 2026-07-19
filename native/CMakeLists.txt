cmake_minimum_required(VERSION 3.22.1)

project("pockettracker")

# The engine is C++17 (std::clamp, std::optional, structured bindings). Nothing declared that until
# now: the Android build simply inherited C++17 from the NDK clang's default, so a host compiler with
# an older default (MSVC → C++14) failed on <algorithm>'s std::clamp. State it, and require it.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Oboe is the ANDROID audio backend only (via Prefab). A desktop build provides its own
# shell (SDL/ALSA/JACK) against processLiveBlock() and never sees this.
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
# ⚠️ NOT in the Android build. Android draws these screens in Compose today, and will keep doing so
# until the convergence decision point (linux-port-plan §12.8, taken after the port ships). Gating it
# out means the port cannot grow the APK by a byte or break it in any way while it is being written —
# and when convergence comes, this `if` is what changes, not where the code lives.
if(NOT ANDROID)
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
    if(MSVC)
        target_compile_options(pt-ui PRIVATE /fp:precise)
    else()
        target_compile_options(pt-ui PRIVATE -fno-fast-math -ffp-contract=off)
    endif()
endif()
