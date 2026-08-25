cmake_minimum_required(VERSION 3.22.1)

# C as well as C++: this links the engine (add_subdirectory(../native) below), and the vendored
# decoder stack under it — stb_vorbis, kissfft, pareq, libopus, libogg — is C.
project("pockettracker-sdl" LANGUAGES C CXX)

# ─── The SDL shell — Linux-port plan Phase 2 ───────────────────────────────────────────────────
#
# The first build of PocketTracker containing no Kotlin. It links the SAME engine the APK ships and
# reaches it through the SAME class Android's JNI layer marshals (songcore::SongcoreHost), so there
# is no second implementation of anything that decides how a song sounds. See main.cpp.
#
#   cmake -S shell -B shell/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build shell/build --config Release
#   shell/build/pockettracker-sdl                 # no argument: a blank document
#
# Pass CMAKE_BUILD_TYPE explicitly, as above. The usual "default to Release if it is empty" guard is
# not portable: MSVC's platform module pre-seeds CMAKE_BUILD_TYPE to Debug (via CMAKE_BUILD_TYPE_INIT)
# while GCC/Clang leave it unset, so the guard silently never fires on Windows — and here that is not
# merely slow: a Debug engine may not keep up with a real-time audio callback, which an offline
# consumer of the same engine never has to. (native/CMakeLists.txt forces -O2 even in Debug on
# GCC/Clang for exactly this reason.)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Windows resource stamps the version into the exe (Explorer > Properties > Details),
# build-windows.ps1 names the zip after it, and app.cpp prints it in the boot banner. The parse is
# shared with the Android build rather than repeated here.
include("${CMAKE_CURRENT_SOURCE_DIR}/../native/cmake/pt_version.cmake")

# ─── A self-contained exe: the MSVC runtime goes IN ────────────────────────────────────────────
#
# ⚠️ Set BEFORE add_subdirectory and FetchContent below, or the engine and SDL compile against a
# different CRT than this target and the link mixes two heaps.
#
# Without this the shell links MSVCP140.dll / VCRUNTIME140.dll / VCRUNTIME140_1.dll, and a machine
# without Visual Studio on it does not have those: a stranger who unzips the A3 package and
# double-clicks gets a Windows error dialog naming a DLL, not a tracker. That is the ENTIRE failure
# the package exists to prevent, and it is invisible on any box that builds the thing — which is
# every box that has ever run this shell.
#
# The UCRT (api-ms-win-crt-*) is a different matter and needs nothing: it is part of Windows itself
# from 10 onwards, and 10 is the floor here anyway.
#
# ⚠️ This works on SDL because SDL2's own CMakeLists sets CMAKE_POLICY_DEFAULT_CMP0091 NEW at its
# very first line (before its cmake_minimum_required, which would otherwise reset the policy to a
# 3.0-era default and make it ignore this variable). Its SDL_FORCE_STATIC_VCRT option is therefore
# NOT needed and must not be set: it only assigns this same variable `if(NOT DEFINED)`, so setting
# both would be one instruction with two spellings.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ─── Dead-code stripping — the handheld's biggest free win ─────────────────────────────────────
#
# gcc and clang neither section-split nor GC by default, so without these every unreferenced function
# in the engine, pt-ui, SDL2 and the whole vendored decoder stack is resident on a 512 MB device.
#
# ⚠️⚠️ DIRECTORY SCOPE IS THE POINT, and it is why this sits ABOVE the two add_subdirectory calls
# below rather than beside the per-target flags further down. --gc-sections discards unreferenced
# SECTIONS; without -ffunction-sections/-fdata-sections on the code being GC'd, a whole .o is one
# section and there is nothing to discard. Measured on gcc 13.3/x86-64 Release, same tree, one flag
# flip apart: per-target on `pockettracker-sdl` alone is 2,933,600 → 2,860,176 B (−2.5 %, the shell's
# own TUs only); at directory scope it is 2,933,600 → 2,569,704 B (−12.4 %). The libraries are where
# the dead code is. ⚠️ The percentage is not portable to the PortMaster aarch64 toolchain; the
# direction is structural and holds everywhere.
#
# ⚠️ This is the ONE exception to the "PER-TARGET, never add_compile_options()" rule stated at the
# IEEE-exactness block below, and the reason the rule does not cover it: that rule protects the
# ENGINE's own -ffast-math policy from being countermanded by a directory-scope FLOATING-POINT flag.
# Section splitting is numerically inert — it decides where the compiler puts each function and datum,
# never what it computes — so it can be inherited by targets entitled to their own fp policy.
# Anything affecting arithmetic still belongs on a target and nowhere else.
#
# MSVC needs nothing (/OPT:REF is its Release default). Apple's ld64 spells it -dead_strip and is
# untested here, so it is excluded rather than guessed at.
if(NOT MSVC AND NOT APPLE)
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
endif()

# ─── The engine, as a host static lib ──────────────────────────────────────────────────────────
# native/CMakeLists.txt's non-ANDROID branch gives us `pockettracker`: the same portable core the
# APK ships, minus the Oboe/JNI shell (its ANDROID_PLATFORM_SOURCES, which this directory replaces).
# Its include roots are PUBLIC, so "audio-engine.h" and "songcore/host.h" just resolve — nothing is
# restated here and nothing can drift. tools/ptrender already links this exact target.
add_subdirectory(../native native)

# ─── SDL2 ──────────────────────────────────────────────────────────────────────────────────────
# System SDL2 first — and on a handheld that is a requirement, not a preference: the PortMaster CFWs
# (muOS, ROCKNIX, ArkOS, Knulli, AmberELEC…) ship their own libSDL2 and a port is expected to link
# the one the OS provides, so it inherits that device's audio/video backends and input quirks.
#
# FetchContent is the fallback, and it is what makes a Windows dev box need zero setup: SDL2 is not
# installed there, and vendoring ~50 MB of it into the repo — the way ogg/opus/nlohmann are vendored —
# would cost every clone for a dependency no shipping target actually takes from us.
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    message(STATUS "SDL2 not found on the system — fetching it (first configure only)")
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL2
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG        release-2.30.9
            GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(SDL2)
endif()

# ─── The Windows resource: the icon and the version stamp (A3) ─────────────────────────────────
# Generated rather than committed, because it carries the icon's path and the version parsed above.
# The icon file ITSELF is committed (shell/windows/PocketTracker.ico) and is regenerated from
# docs/images/logo-plain.png by shell/windows/make-icon.ps1 — which no build runs.
#
# One embedded icon is all the shell needs: SDL takes the first RT_GROUP_ICON as the window icon, so
# this is also why there is no SDL_SetWindowIcon anywhere. See the header of pockettracker.rc.in.
if(WIN32)
    # Not in project(LANGUAGES) at the top, because WIN32 is only defined once project() has run.
    # Ninja in particular will not compile a .rc without this; the VS generators are more forgiving,
    # which is exactly how a build breaks on the runner and not on the dev box.
    enable_language(RC)
    set(PT_ICON     "${CMAKE_CURRENT_SOURCE_DIR}/windows/PocketTracker.ico")
    set(PT_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/windows/pockettracker.manifest")
    configure_file(windows/pockettracker.rc.in "${CMAKE_CURRENT_BINARY_DIR}/pockettracker.rc" @ONLY)
    set(PT_WIN_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/pockettracker.rc")
endif()

# app.cpp is the SHARED shell — the boot sequence, the frame loop and the teardown, identical on every
# platform (convergence C0.2). main.cpp is the desktop/handheld residue beside it: argv, the signal
# handler, SDL_Init/SDL_Quit, and the choice of audio backend, root and caps. Convergence C1 adds an
# Android entry point ALONGSIDE main.cpp and links the same app.cpp.
add_executable(pockettracker-sdl
        app.cpp
        main.cpp
        image.cpp          # PNG decode (convergence D2): the one stb_image implementation TU
        assets.cpp         # the asset seam (convergence D7): SDL_RWFromFile, APK vs beside-the-exe
        skin.cpp           # the touch-skin textures (convergence D1): decode once, own per renderer
        overlay.cpp        # the CRT screen overlay (convergence D6): one texture, composited over the frame
        portrait2.cpp      # the PORTRAIT2 device-skin renderer (convergence D): composite the bands
        font_raster.cpp    # glyph rasterization (convergence D): the one stb_truetype implementation TU
        font.cpp           # the button-label font (convergence D): Helvetica glyphs + D-pad triangles → textures
        midi-out-base.cpp  # the platform-FREE half of a MIDI backend (MIDI plan B2b): list, TEST, trace
        midi-out-winmm.cpp # the WINDOWS songcore::IMidiOut (MIDI plan B2);  empty TU elsewhere
        midi-out-alsa.cpp  # the LINUX   songcore::IMidiOut (MIDI plan B2b); empty TU elsewhere
        alsa-rawmidi.cpp   # the ONE libasound loader + rawmidi device walk (E5); empty TU elsewhere
        midi-in-base.cpp   # the platform-FREE half of a MIDI INPUT backend (E2): list, sink, counters
        midi-in-winmm.cpp  # the WINDOWS songcore::IMidiIn  (MIDI plan E2);  empty TU elsewhere
        midi-in-alsa.cpp   # the LINUX   songcore::IMidiIn  (MIDI plan E5);  empty TU elsewhere
        midi-sender.cpp    # the just-in-time MIDI sender thread + its jitter instrument (MIDI plan B3)
        sdl-audio-engine.cpp
        sdl-input.cpp
        sdl-touch.cpp
        sdl-video.cpp
        ${PT_WIN_RESOURCES}
)

# app.cpp's boot banner. A bug report arrives as a log file and a sentence, and without this the
# version is only in the download's filename — which a reporter has usually thrown away.
target_compile_definitions(pockettracker-sdl PRIVATE "PT_VERSION_STRING=\"${PT_VERSION}\"")

# winmm — `midiOutShortMsg` and friends (midi-out-winmm.cpp). In every Windows install; no package,
# no COM, no manifest capability.
if(WIN32)
    target_link_libraries(pockettracker-sdl PRIVATE winmm)

    # ⚠️ NO CONSOLE WINDOW. A console-subsystem exe opens a black window beside the app for the whole
    # session, and on Windows that window IS the only place the program's output ever went. The
    # subsystem moves to WINDOWS and the output moves to <app root>/pockettracker-log.txt — see
    # open_session_log() in main.cpp, which is unconditional here because there is nothing left to
    # print to.
    #
    # ⚠️ /ENTRY:mainCRTStartup with it. A WINDOWS-subsystem link looks for WinMain, and this shell
    # deliberately does not link SDL2main (it calls SDL_SetMainReady itself), so without the entry
    # point named the link fails on an unresolved WinMain rather than on anything that names a
    # console.
    if(MSVC)
        target_link_options(pockettracker-sdl PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    else()
        target_link_options(pockettracker-sdl PRIVATE -mwindows)
    endif()
endif()

# ⚠️ The Linux MIDI backend links NO libasound — it dlopen()s it (midi-out-alsa.cpp explains why: the
# PortMaster container has no libasound-dev, and a link-time dependency turns a CFW without the
# library into an app that will not start). So the only thing needed here is the dynamic loader, and
# that is `${CMAKE_DL_LIBS}` rather than a bare -ldl: dlopen moved into libc proper at glibc 2.34, and
# this port's floor is 2.31 (ubuntu:20.04, see Dockerfile.portmaster), where it is still a separate
# library. CMake resolves it to `dl` or to nothing, whichever the toolchain wants.
#
# ⚠️ Threads::Threads since E5: the ALSA INPUT backend owns a reader thread (`std::thread`, because
# ALSA rawmidi has no callback — midi-in-alsa.cpp says why it is not an SDL thread), and on
# gcc/libstdc++ `std::thread` needs -pthread at COMPILE time as well as at link. A bare -lpthread is
# not the same flag and leaves a binary that aborts the moment a thread is created.
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(Threads REQUIRED)
    target_link_libraries(pockettracker-sdl PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()

# ─── IEEE-exactness: a CORRECTNESS requirement, not an optimisation choice ─────────────────────
#
# This is copied from tools/CMakeLists.txt on purpose, and it is the least obvious setting in this
# file. app.cpp #includes songcore/host.h, so the SEQUENCER is compiled into this target's translation
# units. Its float params (velGain/volGain/pan, PSL/PBN/vibrato rates) are the ones the conformance
# traces compare as raw binary32 BITS (event-schema §5/§6) — and aarch64 clang contracts `a + b*c`
# into an fma BY DEFAULT, -ffast-math or not, which rounds once where the JVM rounds twice.
#
# ⚠️ PER-TARGET is what keeps this correct across the C0.2 split: the flags below apply to every TU in
# `pockettracker-sdl`, so moving the host include from main.cpp to app.cpp did not move the guarantee.
# A future Android entry point linking app.cpp must restate it in ITS build (the APK's own
# native/CMakeLists.txt already does, for songcore-jni.cpp, for exactly this reason).
#
# Without this, the handheld build would quietly sequence differently from the APK the goldens were
# recorded against, on the exact architecture we ship to, and nothing in this directory would say so.
#
# PER-TARGET, never add_compile_options(): a directory-scope flag would be inherited by everything
# under add_subdirectory(../native) — the engine and the whole vendored decoder stack — and would
# countermand the ENGINE's own -ffast-math policy, which it is entitled to make for itself. That is a
# rule about ARITHMETIC, not about scope for its own sake: the section-splitting flags above are set
# at directory scope deliberately, because they cannot change a result. See the note there.
if(MSVC)
    target_compile_options(pockettracker-sdl PRIVATE /fp:precise)
    # ⚠️ The manifest embedded by the .rc above is the ONLY one. MSVC's linker synthesises its own
    # by default (the bare asInvoker one it has always produced), and two manifests in one image is
    # a link error at best and the wrong one winning at worst — losing activeCodePage=UTF-8, whose
    # absence is silent until a user has a non-ASCII character in a path.
    target_link_options(pockettracker-sdl PRIVATE /MANIFEST:NO)
else()
    target_compile_options(pockettracker-sdl PRIVATE -fno-fast-math -ffp-contract=off)
endif()


# pt-ui is the portable UI — the screen modules, the software canvas, the 5×5 font, the cursor-context
# system (native/ui/). It links `pockettracker` itself, so this one line brings the engine, songcore
# and the UI in together. It contains no SDL and no POSIX: the seam is exactly at this line, which is
# what lets tools/ptshot draw a screen with no window at all.
target_link_libraries(pockettracker-sdl PRIVATE pt-ui)

# SDL2 exports different target names depending on how it arrived (system config package vs
# FetchContent, shared vs static), and an older find module exports plain variables instead.
if(TARGET SDL2::SDL2)
    target_link_libraries(pockettracker-sdl PRIVATE SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(pockettracker-sdl PRIVATE SDL2::SDL2-static)
else()
    target_include_directories(pockettracker-sdl PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(pockettracker-sdl PRIVATE ${SDL2_LIBRARIES})
endif()
