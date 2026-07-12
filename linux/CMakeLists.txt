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
#   cmake -S linux -B linux/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build linux/build --config Release
#   linux/build/pockettracker-sdl testdata/g7-audio.ptp testdata
#
# Pass CMAKE_BUILD_TYPE explicitly, as above. The usual "default to Release if it is empty" guard is
# not portable: MSVC's platform module pre-seeds CMAKE_BUILD_TYPE to Debug (via CMAKE_BUILD_TYPE_INIT)
# while GCC/Clang leave it unset, so the guard silently never fires on Windows — and here, unlike the
# conformance tools, that is not merely slow: a Debug engine may not keep up with a real-time audio
# callback. (native/CMakeLists.txt forces -O2 even in Debug on GCC/Clang for exactly this reason.)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

add_executable(pockettracker-sdl
        main.cpp
        sdl-audio-engine.cpp
)

# ─── IEEE-exactness: a CORRECTNESS requirement, not an optimisation choice ─────────────────────
#
# This is copied from tools/CMakeLists.txt on purpose, and it is the least obvious line in this file.
# main.cpp #includes songcore/host.h, so the SEQUENCER is compiled into this target's translation
# units. Its float params (velGain/volGain/pan, PSL/PBN/vibrato rates) are the ones the conformance
# traces compare as raw binary32 BITS (event-schema §5/§6) — and aarch64 clang contracts `a + b*c`
# into an fma BY DEFAULT, -ffast-math or not, which rounds once where the JVM rounds twice.
#
# Without this, the handheld build would quietly sequence differently from the APK the goldens were
# recorded against, on the exact architecture we ship to, and nothing in this directory would say so.
#
# PER-TARGET, never add_compile_options(): a directory-scope flag would be inherited by everything
# under add_subdirectory(../native) — the engine and the whole vendored decoder stack — and would
# countermand the ENGINE's own -ffast-math policy, which it is entitled to make for itself.
if(MSVC)
    target_compile_options(pockettracker-sdl PRIVATE /fp:precise)
else()
    target_compile_options(pockettracker-sdl PRIVATE -fno-fast-math -ffp-contract=off)
endif()

target_link_libraries(pockettracker-sdl PRIVATE pockettracker)

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
