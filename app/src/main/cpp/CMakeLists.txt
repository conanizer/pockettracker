cmake_minimum_required(VERSION 3.22.1)

project("pockettracker")

# Find Oboe package (via Prefab)
find_package(oboe REQUIRED CONFIG)

# Add our native library
add_library(pockettracker SHARED
        native-audio.cpp
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