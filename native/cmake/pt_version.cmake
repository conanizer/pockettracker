# ─── The version, PARSED from the one place that already had it ────────────────────────────────
#
# Three builds want the version string and none of them may become a SECOND place a release has to
# remember to bump — that is the shape of bug this project keeps paying for. So it is read out of
# app/build.gradle.kts, where `versionName` has always lived, and this file is included by both
# shell/CMakeLists.txt (desktop, PortMaster) and native/CMakeLists.txt (the Android .so), so the
# regex exists once rather than once per tree.
#
# Sets, in the includer's scope:
#   PT_VERSION        "0.9.4"     — the Windows .rc, the zip name, and the runtime banner
#   PT_VERSION_COMMA  "0,9,4,0"   — the Windows VERSIONINFO fields, which take four integers
#
# FATAL_ERROR rather than a warning-and-a-default: the file is in this repo and cannot be missing,
# so the only way here is somebody changing the Gradle syntax, and then a three-second regex fix is
# the right outcome. A default would let a build ship a version string that is quietly a lie.

file(READ "${CMAKE_CURRENT_LIST_DIR}/../../app/build.gradle.kts" PT_GRADLE)
if(NOT PT_GRADLE MATCHES "versionName[ \t]*=[ \t]*\"([0-9]+)\\.([0-9]+)\\.([0-9]+)\"")
    message(FATAL_ERROR
            "could not find versionName in app/build.gradle.kts - if its syntax changed, update "
            "the regex in native/cmake/pt_version.cmake")
endif()
set(PT_VERSION       "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
set(PT_VERSION_COMMA "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")
message(STATUS "PocketTracker ${PT_VERSION} (from app/build.gradle.kts)")
