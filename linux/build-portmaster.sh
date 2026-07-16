#!/bin/bash
#
# Build the PortMaster (aarch64) package: build/portmaster/pockettracker.zip
#
# Run from the repo root, on a box with docker:
#
#     docker build -t pockettracker-build -f linux/Dockerfile.portmaster linux/
#     docker run --rm -v "$PWD:/src" pockettracker-build bash /src/linux/build-portmaster.sh
#
# ⚠️ Do NOT run this on the host toolchain just because the host HAS an aarch64 cross compiler.
# It will build, link and produce a perfectly good ELF that no handheld can load. The container is
# the glibc floor; see linux/Dockerfile.portmaster.
#
# ⚠️ Nothing here is filtered down to the word "error". Every command prints its own output. Four
# separate sessions of this project have been burned by a build step that failed quietly and left a
# stale artifact behind for the next step to "verify".
set -euo pipefail

SRC=${SRC:-/src}

# ⚠️ THE SDL2 WE BUILD IS A LINK-TIME SDK AND IS NEVER SHIPPED. The port runs against the DEVICE's
# libSDL2 — the copy its CFW patched for that hardware's display (KMSDRM) and audio (ALSA). Shipping
# our own would shadow the one that actually knows the screen.
#
# So the version here is not "the newest that works", it is THE COMPATIBILITY FLOOR, and pinning it
# low turns a claim into a guarantee: you cannot link a symbol that does not exist in the .so you
# linked against, so if the shell ever reaches for a newer SDL API this build FAILS HERE, loudly, on
# a dev box — instead of on a stranger's handheld as `undefined symbol`. 2.0.18 is the floor because
# the shell calls SDL_GetTicks64(), which landed in exactly that release; everything else it needs is
# 2.0.0-era. Raise this ONLY together with the requirement in linux/portmaster/README.md.
SDL2_TAG=${SDL2_TAG:-release-2.0.18}

# Every Tier-1 CFW is at or above Ubuntu 20.04's glibc. This is asserted on the artifact below.
GLIBC_MAX_ALLOWED=${GLIBC_MAX_ALLOWED:-2.31}

SYSROOT=/usr/aarch64-linux-gnu
SDL2_SRC=/tmp/sdl2-src
SDL2_BUILD=/tmp/sdl2-build
BUILD=$SRC/build/aarch64
OUT=$SRC/build/portmaster
STAGE=$OUT/stage
BIN=$STAGE/pockettracker/pockettracker.aarch64

cd "$SRC"
git config --global --add safe.directory '*'

echo
echo "############ 1/5  cross-build SDL2 $SDL2_TAG (link SDK only, NOT shipped) ############"
rm -rf "$SDL2_SRC" "$SDL2_BUILD"
git clone --depth 1 --branch "$SDL2_TAG" https://github.com/libsdl-org/SDL.git "$SDL2_SRC"
cmake -S "$SDL2_SRC" -B "$SDL2_BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
    -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
    -DCMAKE_SYSTEM_NAME=Linux \
    -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_INSTALL_PREFIX="$SYSROOT" \
    -DSDL_SHARED=ON \
    -DSDL_STATIC=OFF \
    -DSDL_TEST=OFF
cmake --build "$SDL2_BUILD"
cmake --install "$SDL2_BUILD"

echo
echo "############ 2/5  cross-build PocketTracker ############"
rm -rf "$BUILD"
cmake -S "$SRC/linux" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/linux/toolchain-aarch64.cmake" \
    -DSDL2_DIR="$SYSROOT/lib/cmake/SDL2"
cmake --build "$BUILD"

echo
echo "############ 3/5  stage the package ############"
# Layout is PortMaster's, and the two names must agree: the port directory and the binary's stem are
# both `pockettracker` (lowercase, the catalog's rule), while the launch script is capitalised.
#
#   pockettracker.zip
#   |- port.json
#   |- gameinfo.xml
#   |- README.md
#   |- PocketTracker.sh
#   `- pockettracker/
#      |- pockettracker.aarch64
#      `- licenses/
rm -rf "$OUT"
mkdir -p "$STAGE/pockettracker/licenses"

cp "$SRC/linux/portmaster/PocketTracker.sh" "$STAGE/"
cp "$SRC/linux/portmaster/port.json"        "$STAGE/"
cp "$SRC/linux/portmaster/gameinfo.xml"     "$STAGE/"
cp "$SRC/linux/portmaster/README.md"        "$STAGE/"
chmod +x "$STAGE/PocketTracker.sh"

cp "$BUILD/pockettracker-sdl" "$BIN"
chmod +x "$BIN"
aarch64-linux-gnu-strip "$BIN"

# PocketTracker is GPL-3.0, and it statically links its decoders — so their notices ship with the
# binary that contains them, not just with the source tree that built it. A missing one is a licence
# breach in the artifact, which is the only thing a user ever receives.
cp "$SRC/LICENSE"                     "$STAGE/pockettracker/licenses/LICENSE"
cp "$SRC/native/vendor/ogg/COPYING"   "$STAGE/pockettracker/licenses/libogg-COPYING"
cp "$SRC/native/vendor/opus/COPYING"  "$STAGE/pockettracker/licenses/libopus-COPYING"
ls -1 "$STAGE/pockettracker/licenses/"

echo
echo "############ 4/5  verify the ARTIFACT (not the build log) ############"
file "$BIN"
echo

# --- the glibc floor: the whole reason for the 20.04 container -------------------------------
GLIBC_MAX=$(readelf -V "$BIN" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sed 's/GLIBC_//' | sort -V | tail -1)
echo "max GLIBC required : $GLIBC_MAX   (must be <= $GLIBC_MAX_ALLOWED)"
if [ "$(printf '%s\n%s\n' "$GLIBC_MAX" "$GLIBC_MAX_ALLOWED" | sort -V | tail -1)" != "$GLIBC_MAX_ALLOWED" ]; then
    echo "FAIL: demands glibc $GLIBC_MAX, newer than the CFW floor $GLIBC_MAX_ALLOWED."
    echo "      You are almost certainly not building in the 20.04 container."
    exit 1
fi

# --- SDL2 must be the DEVICE's: dynamically needed, and not bundled beside us ------------------
echo
echo "dynamic deps:"
readelf -d "$BIN" | grep NEEDED
if ! readelf -d "$BIN" | grep -q 'libSDL2-2.0.so.0'; then
    echo "FAIL: not dynamically linked against libSDL2 - SDL2 got statically bundled."
    echo "      The port must run on the CFW's own patched SDL2."
    exit 1
fi
if [ -d "$STAGE/pockettracker/libs.aarch64" ]; then
    echo "FAIL: libs.aarch64 is present - a shipped libSDL2 would shadow the device's."
    exit 1
fi

# --- the bug this port was rebuilt for --------------------------------------------------------
# gptokeyb injects keyboard presses for the same physical buttons the shell already reads off the
# pad, and its defaults disagree with the shell's keyboard map about what they mean (start=enter,
# and Enter is the A button). It made START insert a chain and stopped playback from stopping.
# The reasoning is in PocketTracker.sh; this is the guard that keeps it out of the artifact.
#
# ⚠️ Comments are STRIPPED before the match, and the first cut of this check forgot to: it grepped
# the raw file, matched the very comment block that explains why gptokeyb must never run here, and
# failed a build whose launch script was correct. A guard that cannot tell documentation from code
# fires on the fix as readily as on the bug.
if sed 's/#.*//' "$STAGE/PocketTracker.sh" | grep -qi 'gptokeyb'; then
    echo "FAIL: PocketTracker.sh runs gptokeyb. Read the comment at the top of that file."
    exit 1
fi
echo "gptokeyb           : not invoked by the launch script (correct)"

# --- no realtime-thread debug instrumentation in a shipped binary -----------------------------
if strings "$BIN" | grep -q 'KILDBG'; then
    echo "FAIL: built from the KIL investigation branch - it printf()s on the audio thread."
    exit 1
fi
echo "debug instr.       : none"
echo "SDL2 link floor    : $SDL2_TAG"

echo
echo "############ 5/5  zip ############"
( cd "$STAGE" && zip -r "$OUT/pockettracker.zip" . -x '.*' )
echo
ls -lh "$OUT/pockettracker.zip"
unzip -l "$OUT/pockettracker.zip"
