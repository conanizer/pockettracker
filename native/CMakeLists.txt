cmake_minimum_required(VERSION 3.22.1)

project("pockettracker")

# Stated, not inherited: MSVC defaults to C++14.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Android's audio backend. The desktop shells drive processLiveBlock() themselves.
if(ANDROID)
    find_package(oboe REQUIRED CONFIG)
endif()

# ─── Vendored Opus decode stack (libogg + libopus + opusfile) ──────────────────────────────────
# Decoded natively: MediaCodec lacks Opus on several target ROMs. Decode only — nothing references the
# encoder's objects, so the linker never takes them out of the archive. opusfile has no CMake of its
# own: its file-decode sources only, no http.c (no OpenSSL), no config.h.
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)

# ⚠️ CMake 4 refuses cmake_minimum_required(VERSION < 3.5), which libogg and libopus ask for. Raised for
# those two only; an older CMake ignores the variable. A patch to vendor/ would be lost at the next update.
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

# ─── Vendored FAAD2 (AAC decode) ───────────────────────────────────────────────────────────────
# minimp4 demuxes .m4a/.mp4/.mov…, FAAD2 decodes the AAC inside. Built from its sources here, not through
# its own CMake, so there is no generated config.h (F-Droid builds from source). Floating-point defaults:
# FAAD_FMT_FLOAT output is [-1,1]. See vendor/faad2/PT-VENDORING.md. CONFIGURE_DEPENDS picks up the files
# a revendor adds or removes.
file(GLOB FAAD2_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/faad2/libfaad/*.c)
add_library(faad2 STATIC ${FAAD2_SOURCES})
target_include_directories(faad2
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/vendor/faad2/include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/faad2/libfaad
)
# decoder.c #errors without PACKAGE_VERSION; keep it the vendored version (PT-VENDORING.md).
# ⚠️ Without HAVE_STDINT_H common.h typedefs its own uint32_t, which the NDK rejects as a redefinition.
target_compile_definitions(faad2 PRIVATE "PACKAGE_VERSION=\"2.11.2\"" HAVE_STDINT_H=1)
# Vendored: warnings off. Its own target, so it does not get the engine's -ffast-math either.
if(MSVC)
    target_compile_options(faad2 PRIVATE /w)
else()
    target_compile_options(faad2 PRIVATE -w)
endif()

# ─── Vendored SDL2, as the APK's shared library ────────────────────────────────────────────────
# F-Droid builds offline and from source, so Android carries SDL in-tree; the desktop shells take the
# system's, and this copy where there is none (shell/CMakeLists.txt). ⚠️ Two versions on purpose: the
# handhelds link the device's SDL against a 2.0.18 floor, Android bundles a current 2.x. vendor/SDL2
# is regenerated only by vendor/revendor-sdl2.sh, never edited by hand.
if(ANDROID)
    # ⚠️ SDL's C and Java halves must be one version — SDLActivity refuses to start otherwise. Checked
    # here, at configure time, instead of on a phone. C calls the third number PATCHLEVEL, Java MICRO_VERSION.
    set(SDL_VERSION_LOCK_H    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/SDL2/include/SDL_version.h")
    set(SDL_VERSION_LOCK_JAVA "${CMAKE_CURRENT_SOURCE_DIR}/vendor/SDL2/android/java/org/libsdl/app/SDLActivity.java")
    # ⚠️ Neither file is a target's source: without this, editing one would not re-run the check.
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
    # Two failed parses would compare equal: check both landed first.
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

    # Shared: SDLActivity.java loads libSDL2.so by name and calls JNI entry points inside it.
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)

    # SDL asks for CMake 3.0 — the codecs' CMake 4 workaround above. The NDK's CMake 3.22.1 does not
    # need it; a host CMake does.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(vendor/SDL2)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()

# ─── Engine sources ────────────────────────────────────────────────────────────────────────────
# The portable engine: no <oboe/*>, <android/*> or <jni.h> (logging goes through audio-defs.h).
# TSF_IMPLEMENTATION is defined in soundfont-voice.cpp only.
set(ENGINE_CORE_SOURCES
        songcore/schema-check.cpp
        # The file-open hook, the RAM probe and the load progress: .cpps so a process holds one copy of
        # each, not one per shared library (and <windows.h> stays out of the callers). See the headers.
        common/byte_source.cpp
        common/platform_memory.cpp
        common/load_progress.cpp
        soundfont-voice.cpp
        # A SoundFont's preset index, and a one-preset copy of it: file format only, no tsf.
        soundfont-trim.cpp
        sample-editor.cpp
        # One class, AudioEngine, over six files: the core, what the control thread sets, the sample
        # pool, the SoundFont bank, the table walk, and the block itself (engine-mix.cpp).
        audio-engine.cpp
        engine-params.cpp
        engine-samples.cpp
        engine-soundfont.cpp
        engine-tables.cpp
        engine-mix.cpp
        transient-detector.cpp
        # dr_mp3 and dr_flac are implemented inside audio-decoders.cpp. stb_vorbis and minimp4 are C files
        # of their own; minimp4 defines ERROR/MALLOC/FREE, kept out of every other file. FAAD2 is above.
        audio-decoders.cpp
        vendor/stb_vorbis/stb_vorbis.c
        vendor/minimp4/minimp4_impl.c
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
        # The Dragonfly Reverb algorithms (reverb ALGO 1..4) and the freeverb3 subset they run on.
        effects/primitives/dragonfly/dragonfly-reverb.cpp
        effects/primitives/dragonfly/freeverb/allpass.cpp
        effects/primitives/dragonfly/freeverb/biquad.cpp
        effects/primitives/dragonfly/freeverb/comb.cpp
        effects/primitives/dragonfly/freeverb/delay.cpp
        effects/primitives/dragonfly/freeverb/delayline.cpp
        effects/primitives/dragonfly/freeverb/earlyref.cpp
        effects/primitives/dragonfly/freeverb/efilter.cpp
        effects/primitives/dragonfly/freeverb/nrev.cpp
        effects/primitives/dragonfly/freeverb/nrevb.cpp
        effects/primitives/dragonfly/freeverb/progenitor.cpp
        effects/primitives/dragonfly/freeverb/progenitor2.cpp
        effects/primitives/dragonfly/freeverb/revbase.cpp
        effects/primitives/dragonfly/freeverb/slot.cpp
        effects/primitives/dragonfly/freeverb/utils.cpp
        effects/primitives/dragonfly/freeverb/zrev.cpp
        effects/primitives/dragonfly/freeverb/zrev2.cpp
        effects/modules/dust-chain.cpp
        effects/soundpipe/pareq.c
)

# Android's audio: the Oboe stream, which android-main.cpp (pockettracker-sdl below) constructs.
set(ANDROID_PLATFORM_SOURCES
        oboe-audio-engine.cpp
)

if(ANDROID)
    add_library(pockettracker SHARED ${ENGINE_CORE_SOURCES} ${ANDROID_PLATFORM_SOURCES})
else()
    # Desktop consumers link the core statically and bring their own shell.
    add_library(pockettracker STATIC ${ENGINE_CORE_SOURCES})
endif()

# ─── Compiler flags ────────────────────────────────────────────────────────────────────────────
# ⚠️ MSVC ignores an unknown `-f…` with a warning, so every GCC/Clang flag below has an MSVC arm.
# Warnings are on for our files; vendored sources are silenced per file, vendored headers by a push/pop
# at the include. ⚠️ PT_WERROR is off by default: F-Droid builds the APK itself, and a newer compiler's
# new warning must not fail it. CI turns it on.
option(PT_WERROR "Treat warnings in PocketTracker's own sources as errors" OFF)
set(PT_VENDORED_ENGINE_SOURCES
        effects/soundpipe/pareq.c
        kissfft/kiss_fft.c
        kissfft/kiss_fftr.c
        effects/primitives/daisysp/svf.cpp
        effects/primitives/daisysp/overdrive.cpp
        effects/primitives/daisysp/decimator.cpp
        effects/primitives/daisysp/limiter.cpp
        effects/primitives/daisysp/compressor.cpp
        effects/primitives/daisysp/crossfade.cpp
        effects/primitives/daisysp/balance.cpp
        effects/primitives/daisysp/reverbsc.cpp)
if(MSVC)
    # M_PI. PUBLIC: sampler-voice.h uses it in a header.
    target_compile_definitions(pockettracker PUBLIC _USE_MATH_DEFINES)
    # MSVC's default already; stated so a parent scope's /fp:fast cannot change the DSP's rounding.
    target_compile_options(pockettracker PRIVATE /fp:precise)

    # /W4 ≈ -Wall -Wextra -Wshadow. The CRT defines drop the "use fopen_s instead" advice.
    target_compile_options(pockettracker PRIVATE /W4 $<$<BOOL:${PT_WERROR}>:/WX>)
    target_compile_definitions(pockettracker PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    set_source_files_properties(
            ${PT_VENDORED_ENGINE_SOURCES}
            vendor/stb_vorbis/stb_vorbis.c
            vendor/minimp4/minimp4_impl.c
            PROPERTIES COMPILE_OPTIONS "/W0"
    )
else()
    # -ffast-math is codegen only; the denormal protection is setFlushToZeroForCurrentThread(), armed
    # per thread. ⚠️ An arch test, so the arm handhelds get it too — and so does an arm64 host (macOS
    # CI), whose DSP is then not bit-equal to x86-64's: an audio golden cannot be a byte compare across
    # toolchains.
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm")
        target_compile_options(pockettracker PRIVATE -ffast-math)
    endif ()

    # ⚠️ The decoders without it: -ffinite-math-only can corrupt a Vorbis decode. A file's flags come
    # after the target's, so -fno-fast-math wins.
    set_source_files_properties(audio-decoders.cpp PROPERTIES COMPILE_OPTIONS "-fno-fast-math")
    # The two vendored decoders: the same, and their warnings off.
    set_source_files_properties(
            vendor/stb_vorbis/stb_vorbis.c
            vendor/minimp4/minimp4_impl.c
            PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-w"
    )

    # No sequencer math is compiled into this library; songcore, pt-ui and pockettracker-sdl, which
    # compile it, pin IEEE rounding themselves.

    # -O2 in Debug: at -O0 the engine is too slow to run in real time on a handheld. MSVC keeps CMake's
    # defaults — there it builds host tools only.
    target_compile_options(pockettracker PRIVATE $<IF:$<CONFIG:Debug>,-O2,-O3>)

    target_compile_options(pockettracker PRIVATE -Wall -Wextra -Wshadow $<$<BOOL:${PT_WERROR}>:-Werror>)
    set_source_files_properties(${PT_VENDORED_ENGINE_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

# freeverb3: float samples, and headers named from the dragonfly/ root. Both scoped to these files.
# ⚠️ DISABLE_UNDENORMAL drops freeverb's per-filter denormal test (~60 compares a sample in HALL), so a
# thread that runs these files must arm flush-to-zero. That test also zeroed a NaN;
# DragonflyReverb::process keeps one out at the door instead.
file(GLOB PT_DRAGONFLY_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/effects/primitives/dragonfly/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/effects/primitives/dragonfly/freeverb/*.cpp)
set_source_files_properties(${PT_DRAGONFLY_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "LIBFV3_FLOAT;DISABLE_UNDENORMAL"
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/effects/primitives/dragonfly)
set_source_files_properties(${PT_DRAGONFLY_SOURCES} PROPERTIES COMPILE_OPTIONS
        $<IF:$<BOOL:${MSVC}>,/W0,-w>)   # vendored

# PUBLIC, so a consumer inherits the roots instead of restating them.
target_include_directories(pockettracker PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/effects/soundpipe
        ${CMAKE_CURRENT_SOURCE_DIR}/kissfft
)

# libm for tsf on Unix-likes; Windows has no m.lib, and asking for one fails the link.
target_link_libraries(pockettracker opusfile faad2)
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

# ─── songcore: the sequencer and the runtime that owns it ──────────────────────────────────────
# The sequencer's and the host's bodies; the rest is header-only. Static, so a tool that only
# sequences links only scheduler.cpp's object.
add_library(songcore STATIC
        songcore/scheduler.cpp
        songcore/host.cpp
        songcore/host_midi.cpp
        songcore/sample_editor_ops.cpp
)
# PUBLIC: host.h includes the engine's header.
target_link_libraries(songcore PUBLIC pockettracker)

# ⚠️ IEEE-exact: the traces compare the sequencer's float params bit for bit across x86 and aarch64,
# and aarch64 clang fma-contracts `a + b*c` by default.
if(MSVC)
    target_compile_options(songcore PRIVATE /fp:precise /W4 $<$<BOOL:${PT_WERROR}>:/WX>)
    target_compile_definitions(songcore PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
else()
    target_compile_options(songcore PRIVATE -fno-fast-math -ffp-contract=off)
    target_compile_options(songcore PRIVATE -Wall -Wextra -Wshadow $<$<BOOL:${PT_WERROR}>:-Werror>)
endif()

# ─── pt-ui: the portable UI ────────────────────────────────────────────────────────────────────
# The screens, canvas, font, theme and input, drawn into a 640×480 ARGB framebuffer. No SDL, POSIX,
# window or audio device: the shell owns those and scales the frame (ui/canvas.h), so a screen can
# also be drawn headless.
# ⚠️ AGP builds what it packages and what that links: pt-ui is built on Android because
# pockettracker-sdl links it. Never list targets in build.gradle.kts's externalNativeBuild — that list
# is a whitelist, and a .so left off it silently stops shipping.
add_library(pt-ui STATIC
        ui/canvas.cpp
        ui/clipboard.cpp
        ui/input_dispatcher.cpp
        ui/dispatch/buttons.cpp
        ui/dispatch/chords.cpp
        ui/dispatch/route.cpp
        ui/dispatch/layers/confirm.cpp
        ui/dispatch/layers/eq.cpp
        ui/dispatch/layers/fx_helper.cpp
        ui/dispatch/layers/map_picker.cpp
        ui/dispatch/layers/qwerty.cpp
        ui/dispatch/layers/render_dialog.cpp
        ui/dispatch/layers/send_editor.cpp
        ui/dispatch/layers/stretch_editor.cpp
        ui/dispatch/layers/theme.cpp
        ui/dispatch/project.cpp
        ui/dispatch/screens/file_browser.cpp
        ui/dispatch/screens/instrument.cpp
        ui/dispatch/screens/midi.cpp
        ui/dispatch/screens/mixer.cpp
        ui/dispatch/screens/project.cpp
        ui/dispatch/screens/sample_editor.cpp
        ui/dispatch/screens/scale_groove.cpp
        ui/dispatch/screens/sequencer.cpp
        ui/dispatch/sub_screens.cpp
        ui/help_topic.cpp
        ui/layout.cpp
        ui/autosave.cpp
        ui/value_hint.cpp
        ui/midi_ports.cpp
        ui/sample_edit_session.cpp
        ui/std_filesystem.cpp
        ui/settings_store.cpp
        ui/folder_config.cpp
        ui/input_config.cpp
        ui/project_actions.cpp
        ui/modules/chain_editor.cpp
        ui/modules/confirm_dialog.cpp
        ui/modules/effects_editor.cpp
        ui/modules/eq_editor.cpp
        ui/modules/file_browser.cpp
        ui/modules/fx_helper_overlay.cpp
        ui/modules/map_picker_overlay.cpp
        ui/modules/groove_editor.cpp
        ui/modules/help_overlay.cpp
        ui/modules/help_panel.cpp
        ui/modules/instrument_editor.cpp
        ui/modules/instrument_pool.cpp
        ui/modules/loading_strip.cpp
        ui/modules/midi_map_editor.cpp
        ui/modules/midi_settings.cpp
        ui/modules/mixer.cpp
        ui/modules/modulation.cpp
        ui/modules/navigation_map.cpp
        ui/modules/oscilloscope.cpp
        ui/modules/phrase_editor.cpp
        ui/modules/project_editor.cpp
        ui/modules/qwerty_keyboard.cpp
        ui/modules/render_dialog.cpp
        ui/modules/scale_editor.cpp
        ui/modules/sample_editor.cpp
        ui/modules/settings_editor.cpp
        ui/modules/song_editor.cpp
        ui/modules/table_editor.cpp
        ui/modules/theme_editor.cpp
)

# ⚠️ gcc 8 and older keep std::filesystem in a separate stdc++fs; a cross-toolchain can be that old.
# A version test, not a compile probe: the probe ignores CMAKE_CXX_STANDARD, compiles as C++14 on MSVC
# and would ask for a stdc++fs that does not exist there.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(pt-ui PUBLIC stdc++fs)
endif()

# PUBLIC: pt-ui's headers include songcore's, so a consumer gets the sequencer and the engine with it.
target_link_libraries(pt-ui PUBLIC songcore)

# std_filesystem.cpp asks Windows where `Documents` is (shell32, ole32). MSVC links both by default;
# MinGW does not.
if(WIN32)
    target_link_libraries(pt-ui PUBLIC shell32 ole32)
endif()

# ⚠️ IEEE-exact, as songcore: it includes songcore's headers, and the EQ editor steps FREQ until the
# formatted readout changes, so rounding decides the value stored in the project.
if(MSVC)
    target_compile_options(pt-ui PRIVATE /fp:precise)
    target_compile_options(pt-ui PRIVATE /W4 $<$<BOOL:${PT_WERROR}>:/WX>)   # see the engine's note
    target_compile_definitions(pt-ui PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
else()
    target_compile_options(pt-ui PRIVATE -fno-fast-math -ffp-contract=off)
    target_compile_options(pt-ui PRIVATE -Wall -Wextra -Wshadow $<$<BOOL:${PT_WERROR}>:-Werror>)
endif()

# ─── The Android app ───────────────────────────────────────────────────────────────────────────
# ⚠️ THIS LIBRARY IS THE APP, on every build type: the manifest's one activity is an SDLActivity.
# libpockettracker.so is the engine alone, a real DT_NEEDED of this one, and legitimately older after a
# build that touched only the shell.
if(ANDROID)
    set(PT_SHELL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../shell")

    # app.cpp is the shell the desktop runs too; android-main.cpp is shell/main.cpp's twin.
    add_library(pockettracker-sdl SHARED
            "${PT_SHELL_DIR}/app.cpp"
            "${PT_SHELL_DIR}/android/android-main.cpp"
            "${PT_SHELL_DIR}/android/saf-filesystem.cpp"   # the SAF ui::FileSystem, and byte_source.h's
                                                           # three hooks for pt:// paths
            "${PT_SHELL_DIR}/android/midi-android-devices.cpp" # the device list both directions share
            "${PT_SHELL_DIR}/android/midi-out-android.cpp" # MIDI out over JNI up-calls
            "${PT_SHELL_DIR}/android/midi-in-android.cpp"  # ⚠️ POLLED once a frame, where winmm and ALSA
                                                           # push — see its header, and MidiInBase::pump
            "${PT_SHELL_DIR}/midi/midi-out-base.cpp"
            "${PT_SHELL_DIR}/midi/midi-in-base.cpp"
            "${PT_SHELL_DIR}/midi/midi-sender.cpp"         # ⚠️ SDL_CreateThread, so SDL attaches it to
                                                           # the JVM — midi-out-android.cpp makes JNI
                                                           # calls FROM this thread
            "${PT_SHELL_DIR}/console/midi-in-console.cpp"
            "${PT_SHELL_DIR}/console/midi-jitter.cpp"
            "${PT_SHELL_DIR}/skin/image.cpp"
            "${PT_SHELL_DIR}/skin/assets.cpp"
            "${PT_SHELL_DIR}/skin/skin.cpp"
            "${PT_SHELL_DIR}/skin/overlay.cpp"
            "${PT_SHELL_DIR}/skin/portrait2.cpp"
            "${PT_SHELL_DIR}/skin/font_raster.cpp"
            "${PT_SHELL_DIR}/skin/font.cpp"
            "${PT_SHELL_DIR}/sdl/sdl-input.cpp"
            "${PT_SHELL_DIR}/sdl/sdl-touch.cpp"
            "${PT_SHELL_DIR}/sdl/sdl-video.cpp"
    )

    # Shell headers by their path from shell/; the engine's and pt-ui's roots come PUBLIC with the link.
    target_include_directories(pockettracker-sdl PRIVATE "${PT_SHELL_DIR}")

    # The boot banner's version, from the parse the desktop and the Windows .rc use. On Android the log
    # is the only place it shows.
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/pt_version.cmake")
    target_compile_definitions(pockettracker-sdl PRIVATE "PT_VERSION_STRING=\"${PT_VERSION}\"")

    # ⚠️ IEEE-exact, as songcore: app.cpp includes songcore/host.h, so the sequencer is compiled here,
    # beside the engine's aarch64 -ffast-math. Per target, never add_compile_options(): that would
    # override the engine's own choice.
    target_compile_options(pockettracker-sdl PRIVATE -fno-fast-math -ffp-contract=off)

    # pt-ui brings songcore and the engine. SDL2 is the vendored shared library SDLActivity loads by
    # name — one copy in the APK. `log` is android-main.cpp's logging before the console is up.
    target_link_libraries(pockettracker-sdl PRIVATE pt-ui SDL2 log android)
endif()
