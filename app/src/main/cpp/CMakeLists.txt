cmake_minimum_required(VERSION 3.22.1)

project("pockettracker")

# Find Oboe package (via Prefab)
find_package(oboe REQUIRED CONFIG)

# Add our native library
add_library(pockettracker SHARED
        native-audio.cpp
)

# Link with Oboe
target_link_libraries(pockettracker
        oboe::oboe
        android
        log
)