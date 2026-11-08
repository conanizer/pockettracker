cmake_minimum_required(VERSION 3.22.1)

# C too: the engine's vendored decoders (stb_vorbis, kissfft, pareq, libopus, libogg) are C.
project("pockettracker-sdl" LANGUAGES C CXX)

# ─── The desktop and handheld app ──────────────────────────────────────────────────────────────
# The same engine and UI the APK ships, behind an SDL shell.
#
#   cmake -S shell -B shell/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build shell/build --config Release
#   shell/build/pockettracker-sdl                 # no argument: a blank document
#
# ⚠️ Pass CMAKE_BUILD_TYPE. A "default to Release" guard never fires on Windows (MSVC pre-seeds Debug),
# and an MSVC Debug engine may not keep up with the audio callback.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The version: the Windows resource stamps it into the exe, build-windows.ps1 names the zip after it,
# app.cpp prints it at boot. One parse, shared with the Android build.
include("${CMAKE_CURRENT_SOURCE_DIR}/../native/cmake/pt_version.cmake")
# The Linux package script names the tarball after it.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.txt" "${PT_VERSION}\n")

# ─── A self-contained exe: the MSVC runtime goes IN ────────────────────────────────────────────
# Without it the exe needs VCRUNTIME140.dll and friends, which a machine without Visual Studio lacks —
# and no machine that builds it shows the failure. The UCRT is part of Windows 10, the floor.
# ⚠️ Set BEFORE the add_subdirectory calls below, or the engine and SDL get another CRT and the
# link mixes two heaps. SDL honours the variable itself; its SDL_FORCE_STATIC_VCRT sets the same one.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ─── Dead-code stripping ───────────────────────────────────────────────────────────────────────
# gcc and clang keep every unreferenced function by default — on a 512 MB handheld.
# ⚠️ Directory scope, above the add_subdirectory calls: the dead code is in the libraries, and
# --gc-sections can only drop what -ffunction-sections split out. The one flag set at directory scope,
# because it cannot change a result; anything touching arithmetic goes on a target.
# MSVC: /OPT:REF is its Release default. Apple's -dead_strip is untested, so left out.
if(NOT MSVC AND NOT APPLE)
    add_compile_options(-ffunction-sections -fdata-sections)
    add_link_options(-Wl,--gc-sections)
endif()

# ─── The engine, as a static library ───────────────────────────────────────────────────────────
# The APK's portable core without the Oboe stream; its include roots come PUBLIC with it.
add_subdirectory(../native native)

# ─── SDL2 ──────────────────────────────────────────────────────────────────────────────────────
# The system's SDL2 first — required on a handheld: the PortMaster CFWs ship their own, patched for
# their display, audio and input. Where there is none (a Windows box), the APK's vendored copy, linked
# statically.
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    message(STATUS "SDL2 not found on the system — building native/vendor/SDL2 into the exe")
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
    set(SDL_TEST   OFF CACHE BOOL "" FORCE)
    # ⚠️ The vendored copy has no wayland-protocols/, and SDL's Wayland backend fails to compile
    # without it. A Linux box that wants Wayland installs the system SDL2.
    set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
    # Its PipeWire backend fails on gcc 14+ against current PipeWire headers; PulseAudio and ALSA
    # still reach a PipeWire server.
    set(SDL_PIPEWIRE OFF CACHE BOOL "" FORCE)
    # SDL asks for CMake 3.0, which a CMake 4 refuses without this.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(../native/vendor/SDL2 sdl2)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()

# ─── The Windows resource: the icon, the manifest, the version stamp ───────────────────────────
# Generated, since it carries the icon's path and the version. The .ico is committed; make-icon.ps1
# regenerates it, and no build runs that. SDL takes the first icon as the window's on Windows.
if(WIN32)
    # ⚠️ Ninja will not compile a .rc without it (the VS generators will). Here, not in project():
    # WIN32 is defined only once project() has run.
    enable_language(RC)
    set(PT_ICON     "${CMAKE_CURRENT_SOURCE_DIR}/windows/PocketTracker.ico")
    set(PT_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/windows/pockettracker.manifest")
    configure_file(windows/pockettracker.rc.in "${CMAKE_CURRENT_BINARY_DIR}/pockettracker.rc" @ONLY)
    set(PT_WIN_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/pockettracker.rc")
endif()

# app.cpp is the shell every platform runs: boot, frame loop, teardown. main.cpp is the desktop's
# entry around it (argv, the signal handler, SDL_Init, the audio backend); Android's is
# android/android-main.cpp, built by native/CMakeLists.txt.
add_executable(pockettracker-sdl
        app.cpp
        main.cpp
        skin/image.cpp                  # the one stb_image implementation TU
        skin/assets.cpp                 # SDL_RWFromFile: APK assets vs files beside the exe
        skin/skin.cpp                   # the touch-skin textures: decode once, own per renderer
        skin/overlay.cpp                # the CRT screen overlay, composited over the frame
        skin/portrait2.cpp              # the PORTRAIT2 device skin: composite the bands
        skin/font_raster.cpp            # the one stb_truetype implementation TU
        skin/font.cpp                   # the button-label font, rasterised into textures
        midi/midi-out-base.cpp          # the platform-free half of a MIDI output: list, TEST, trace
        midi/midi-in-base.cpp           # the platform-free half of a MIDI input: list, sink, counters
        midi/midi-sender.cpp            # the just-in-time MIDI sender thread
        console/midi-in-console.cpp     # the measuring instruments, off unless an env var asks
        console/midi-jitter.cpp
        linux/alsa-rawmidi.cpp          # the one libasound loader + device walk; empty TU elsewhere
        linux/alsa-route.cpp            # what stands between ALSA's default and the chip; empty elsewhere
        linux/midi-out-alsa.cpp         # empty TU off Linux, as are the rest of linux/ and windows/
        linux/midi-in-alsa.cpp
        windows/midi-out-winmm.cpp
        windows/midi-in-winmm.cpp
        windows/asio-audio-engine.cpp   # the ASIO output (SETTINGS > AUDIO OUT)
        windows/audio-outputs-win.cpp   # the switch between the system output and ASIO
        sdl/sdl-audio-engine.cpp
        sdl/sdl-input.cpp
        sdl/sdl-touch.cpp
        sdl/sdl-video.cpp
        ${PT_WIN_RESOURCES}
)

# Shell headers are included by their path from here ("midi/midi-out-base.h").
target_include_directories(pockettracker-sdl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# app.cpp's boot banner: a bug report's log then names the version.
target_compile_definitions(pockettracker-sdl PRIVATE "PT_VERSION_STRING=\"${PT_VERSION}\"")

# A handheld build may take the sound chip for itself (sdl-audio-engine.cpp): nothing else plays
# there. A desktop build never does — an exclusive open would silence every other program.
option(PT_HANDHELD "Build for a handheld (PortMaster): the audio may open the chip exclusively" OFF)
if(PT_HANDHELD)
    target_compile_definitions(pockettracker-sdl PRIVATE PT_HANDHELD=1)
endif()

# winmm: Windows MIDI, in every install.
if(WIN32)
    target_link_libraries(pockettracker-sdl PRIVATE winmm)

    # The Steinberg ASIO SDK's host side (native/vendor/asio, GPLv3 option). Vendor code: its own
    # target, so our warning flags never reach it.
    set(PT_ASIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../native/vendor/asio")
    add_library(pt-asio STATIC
            "${PT_ASIO_DIR}/common/asio.cpp"
            "${PT_ASIO_DIR}/host/asiodrivers.cpp"
            "${PT_ASIO_DIR}/host/pc/asiolist.cpp")
    target_include_directories(pt-asio PUBLIC
            "${PT_ASIO_DIR}/common" "${PT_ASIO_DIR}/host" "${PT_ASIO_DIR}/host/pc")
    target_link_libraries(pt-asio PUBLIC ole32 advapi32)
    if(MSVC)
        target_compile_options(pt-asio PRIVATE /W0)
    else()
        target_compile_options(pt-asio PRIVATE -w)
    endif()
    target_link_libraries(pockettracker-sdl PRIVATE pt-asio)

    # No console window: the output goes to pockettracker-log.txt instead (open_session_log, main.cpp).
    # ⚠️ /ENTRY:mainCRTStartup with it: SDL2main is not linked (main.cpp calls SDL_SetMainReady), and a
    # WINDOWS-subsystem link otherwise fails looking for WinMain.
    if(MSVC)
        target_link_options(pockettracker-sdl PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    else()
        target_link_options(pockettracker-sdl PRIVATE -mwindows)
    endif()
endif()

# ⚠️ libasound is dlopen()ed, never linked: a CFW without it must still start (midi-out-alsa.cpp).
# ${CMAKE_DL_LIBS}, not -ldl: dlopen is in libc from glibc 2.34, and the floor is 2.31.
# ⚠️ Threads::Threads for the ALSA input's std::thread: -pthread is needed at compile time too, and a
# bare -lpthread leaves a binary that aborts on the first thread.
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(Threads REQUIRED)
    target_link_libraries(pockettracker-sdl PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()

# ⚠️ IEEE-exact: app.cpp includes songcore's headers, so the sequencer is compiled here, and the traces
# compare its float params bit for bit across x86 and aarch64 (whose clang fma-contracts by default).
# Per target, never add_compile_options(): that would reach the engine and override its -ffast-math.
# The Android build restates it for its own app.cpp target.
if(MSVC)
    target_compile_options(pockettracker-sdl PRIVATE /fp:precise)
    # ⚠️ The .rc's manifest must be the only one: the linker's own would drop activeCodePage=UTF-8,
    # which fails silently until a path holds a non-ASCII character.
    target_link_options(pockettracker-sdl PRIVATE /MANIFEST:NO)
else()
    target_compile_options(pockettracker-sdl PRIVATE -fno-fast-math -ffp-contract=off)
endif()

# The portable UI, which brings songcore and the engine with it. No SDL and no POSIX below this line.
target_link_libraries(pockettracker-sdl PRIVATE pt-ui)

# SDL2's target name depends on how it arrived; an older find module sets plain variables instead.
if(TARGET SDL2::SDL2)
    target_link_libraries(pockettracker-sdl PRIVATE SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(pockettracker-sdl PRIVATE SDL2::SDL2-static)
else()
    target_include_directories(pockettracker-sdl PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(pockettracker-sdl PRIVATE ${SDL2_LIBRARIES})
endif()
