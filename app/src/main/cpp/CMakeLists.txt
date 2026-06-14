cmake_minimum_required(VERSION 3.22.1)

project("pockettracker")

# Find Oboe package (via Prefab)
find_package(oboe REQUIRED CONFIG)

# Add our native library
# Phase 0 file split: native-audio.cpp has been split into focused modules.
# TSF_IMPLEMENTATION is defined in soundfont-voice.cpp only.
add_library(pockettracker SHARED
        soundfont-voice.cpp
        sample-editor.cpp
        audio-engine.cpp
        jni-bridge.cpp
        transient-detector.cpp
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

# Link with Oboe and libm (required by tsf for math functions)
target_link_libraries(pockettracker
        oboe::oboe
        android
        log
        m
)