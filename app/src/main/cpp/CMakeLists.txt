cmake_minimum_required(VERSION 3.22.1)

project("pockettracker")

# Find Oboe package (via Prefab)
find_package(oboe REQUIRED CONFIG)

# Add our native library
# Phase 0 file split: native-audio.cpp has been split into focused modules.
# TSF_IMPLEMENTATION is defined in soundfont-voice.cpp only.
add_library(pockettracker SHARED
        soundfont-voice.cpp
        audio-engine.cpp
        jni-bridge.cpp
        effects/primitives/daisysp/svf.cpp
        effects/primitives/daisysp/overdrive.cpp
        effects/primitives/daisysp/decimator.cpp
        effects/primitives/daisysp/limiter.cpp
        effects/primitives/daisysp/compressor.cpp
        effects/primitives/daisysp/crossfade.cpp
        effects/primitives/daisysp/balance.cpp
)

# Include current directory so tsf.h can be found
target_include_directories(pockettracker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Link with Oboe and libm (required by tsf.h for math functions)
target_link_libraries(pockettracker
        oboe::oboe
        android
        log
        m
)