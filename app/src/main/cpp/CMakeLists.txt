cmake_minimum_required(VERSION 3.22.1)

project("pockettracker")

# Find Oboe package (via Prefab)
find_package(oboe REQUIRED CONFIG)

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
add_subdirectory(vendor/ogg)
add_subdirectory(vendor/opus)
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

# Add our native library
# The engine is split across focused modules (native-audio.cpp is a thin stub redirect).
# TSF_IMPLEMENTATION is defined in soundfont-voice.cpp only.
add_library(pockettracker SHARED
        soundfont-voice.cpp
        sample-editor.cpp
        audio-engine.cpp
        oboe-audio-engine.cpp
        jni-bridge.cpp
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

# Flush-to-zero on ARM: prevents denormal floats from causing 10-100x slowdowns
# in feedback loops (reverb, delay, EQ). Required when using any effects.
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

# 7.3: explicit optimization level. The NDK leaves Debug at -O0, which makes the real-time audio
# engine too slow to test reliably on the Miyoo (the variant we test most). Force -O2 in Debug so
# debug builds run the engine at near-release speed; Release keeps -O3. (We debug via logcat, not
# native breakpoints, so the loss of -O0 debuggability is a non-issue here.)
target_compile_options(pockettracker PRIVATE $<IF:$<CONFIG:Debug>,-O2,-O3>)

# Include paths: root (for audio-defs.h, sampler-voice.h, etc.),
#   effects/soundpipe (so pareq.c finds soundpipe.h), kissfft (for kiss_fft.h)
target_include_directories(pockettracker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/effects/soundpipe
        ${CMAKE_CURRENT_SOURCE_DIR}/kissfft
)

# Link with Oboe and libm (required by tsf for math functions). opusfile pulls in libopus + libogg
# (PUBLIC-linked) for native Opus decoding, and propagates the opusfile.h include path.
target_link_libraries(pockettracker
        oboe::oboe
        opusfile
        android
        log
        m
)