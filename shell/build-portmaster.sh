#!/bin/bash
#
# Build the PortMaster (aarch64) package: build/portmaster/pockettracker.zip
#
# Run from the repo root, on a box with docker:
#
#     docker build -t pockettracker-build -f shell/Dockerfile.portmaster shell/
#     docker run --rm -v "$PWD:/src" pockettracker-build bash /src/shell/build-portmaster.sh
#
# ⚠️ Never on the host's own cross compiler: it builds an ELF no handheld can load. The container is
# the glibc floor (Dockerfile.portmaster).
# ⚠️ Nothing is filtered: a step that fails quietly leaves a stale artifact for the next to "verify".
set -euo pipefail

SRC=${SRC:-/src}

# ⚠️ This SDL2 is only linked against, never shipped: the port runs on the device's own, patched by its
# CFW for the display and audio. Its version is the floor the app supports — 2.0.18, the first with
# SDL_GetTicks64() — so a newer API fails the link here, not on a handheld. Raise it only together with
# shell/portmaster/README.md and SDL2_TAG in Dockerfile.linux.
SDL2_TAG=${SDL2_TAG:-release-2.0.18}

# Every Tier-1 CFW has at least Ubuntu 20.04's glibc. Checked on the artifact below.
GLIBC_MAX_ALLOWED=${GLIBC_MAX_ALLOWED:-2.31}

SYSROOT=/usr/aarch64-linux-gnu
SDL2_SRC=/tmp/sdl2-src
SDL2_BUILD=/tmp/sdl2-build
BUILD=$SRC/build/aarch64
OUT=$SRC/build/portmaster
STAGE=$OUT/stage
ZIPROOT=$OUT/ziproot
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
cmake -S "$SRC/shell" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/shell/toolchain-aarch64.cmake" \
    -DPT_HANDHELD=ON \
    -DSDL2_DIR="$SYSROOT/lib/cmake/SDL2"
cmake --build "$BUILD"

echo
echo "############ 3/5  stage the package ############"
# Two layouts. $STAGE is the catalog's: what is copied into PortMaster-New's `ports/pockettracker/`,
# with the five root files its `tools/build_release.py` wants there (a screenshot one level down reads
# to it as none).
#
#   $STAGE/
#   |- PocketTracker.sh
#   |- port.json
#   |- gameinfo.xml
#   |- README.md
#   |- screenshot.png
#   `- pockettracker/
#      |- pockettracker.aarch64
#      `- licenses/
#
# The zip is upstream's shape (step 5), so a tester installs what the catalog will hand a user.
# ⚠️ The port directory and the binary's stem are both `pockettracker` (lowercase, the catalog's rule);
# the launch script is capitalised.
rm -rf "$OUT"
mkdir -p "$STAGE/pockettracker/licenses"

cp "$SRC/shell/portmaster/PocketTracker.sh" "$STAGE/"
cp "$SRC/shell/portmaster/port.json"        "$STAGE/"
cp "$SRC/shell/portmaster/gameinfo.xml"     "$STAGE/"
cp "$SRC/shell/portmaster/README.md"        "$STAGE/"
chmod +x "$STAGE/PocketTracker.sh"

# At the root for the catalog's checker; step 5 moves it where gameinfo.xml points the CFW's game list.
# port.json names no image — the catalog finds it by name. Any platform's capture shows what the
# handheld draws; it must be 4:3 and at least 640x480.
cp "$SRC/docs/images/screenshot.png" "$STAGE/screenshot.png"

cp "$BUILD/pockettracker-sdl" "$BIN"
chmod +x "$BIN"
aarch64-linux-gnu-strip "$BIN"

# The statically linked components' notices ship with the binary that contains them.
# ⚠️ THIRD-PARTY-NOTICES.md is the one source: it carries the text inline where upstream ships no file
# (KissFFT, opusfile). Add a component there with the commit that vendors it; step 4 checks.
cp "$SRC/LICENSE"                                       "$STAGE/pockettracker/licenses/LICENSE"
cp "$SRC/docs/licenses/THIRD-PARTY-NOTICES.md"          "$STAGE/pockettracker/licenses/"
cp "$SRC/CREDITS.md"                                    "$STAGE/pockettracker/licenses/CREDITS.md"
cp "$SRC/native/vendor/ogg/COPYING"                     "$STAGE/pockettracker/licenses/libogg-COPYING"
cp "$SRC/native/vendor/opus/COPYING"                    "$STAGE/pockettracker/licenses/libopus-COPYING"
cp "$SRC/native/vendor/opus/LICENSE_PLEASE_READ.txt"    "$STAGE/pockettracker/licenses/libopus-LICENSE_PLEASE_READ.txt"

# The OFL texts ship though no font file does: THIRD-PARTY-NOTICES.md cites them by path, and the 5×5
# UI font compiled into the binary is adapted from QuinqueFive.
cp "$SRC/docs/licenses/OFL-1.1-LiberationSans.txt"      "$STAGE/pockettracker/licenses/OFL-1.1-LiberationSans.txt"
cp "$SRC/docs/licenses/OFL-1.1-QuinqueFive.txt"         "$STAGE/pockettracker/licenses/OFL-1.1-QuinqueFive.txt"
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

# --- no gptokeyb ------------------------------------------------------------------------------
# It would press keys for the buttons the app already reads off the pad (PocketTracker.sh says why).
# ⚠️ Comments stripped first: the launch script's own comment names it.
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

# --- every statically linked component must have a notice -------------------------------------
# ⚠️ The vendor directory is the list: add `native/vendor/foo/` and this fails until
# THIRD-PARTY-NOTICES.md names `foo`. A hand-kept list would be forgotten by the same commit.
NOTICES="$STAGE/pockettracker/licenses/THIRD-PARTY-NOTICES.md"
echo
echo "licence notices:"
MISSING=""
# ⚠️ `basename`, not `ls -1`: a trailing slash makes `dr_flac/` match a path in the notices' prose
# instead of the component's name, and the check passes on nothing.
VENDORED=""
for D in "$SRC"/native/vendor/*/; do VENDORED="$VENDORED $(basename "$D")"; done
# The vendored tree and the three components outside it — SDL2 included though this zip does not
# ship it: the question is whether every vendored component is documented. The notices' table says
# which artifact carries what.
for COMPONENT in $VENDORED kissfft daisysp soundpipe; do
    if grep -qi -- "$COMPONENT" "$NOTICES"; then
        echo "  ok      $COMPONENT"
    else
        echo "  MISSING $COMPONENT"
        MISSING="$MISSING $COMPONENT"
    fi
done
if [ -n "$MISSING" ]; then
    echo "FAIL: statically linked but not named in THIRD-PARTY-NOTICES.md:$MISSING"
    echo "      The artifact is the only thing a user receives. Document it there, then rebuild."
    exit 1
fi

echo
echo "############ 5/5  zip (upstream's transform, not one of ours) ############"
# `build_port_zip()` in PortMaster-New/tools/build_release.py: the four root metadata files move INTO
# the port directory, README.md becomes `<portname>.md`, only the launch script stays at the root.
rm -rf "$ZIPROOT"
mkdir -p "$ZIPROOT"
cp -a "$STAGE/pockettracker"  "$ZIPROOT/"
cp    "$STAGE/PocketTracker.sh" "$ZIPROOT/"
cp    "$STAGE/port.json" "$STAGE/gameinfo.xml" "$STAGE/screenshot.png" "$ZIPROOT/pockettracker/"
cp    "$STAGE/README.md"        "$ZIPROOT/pockettracker/pockettracker.md"
( cd "$ZIPROOT" && zip -r "$OUT/pockettracker.zip" . -x '.*' )
echo
ls -lh "$OUT/pockettracker.zip"
unzip -l "$OUT/pockettracker.zip"

# --- the transform, read back out of the zip --------------------------------------------------
# harbourmaster runs the launch script from the zip root; the CFW reads the screenshot where
# gameinfo.xml points. ⚠️ `|| …=0`: grep and unzip exit non-zero on a miss, and pipefail would end the
# script before the FAIL line names it.
echo
SCRIPT_AT_ROOT=$(unzip -l "$OUT/pockettracker.zip" | grep -c ' PocketTracker\.sh$') || SCRIPT_AT_ROOT=0
echo "launch script at zip root : $SCRIPT_AT_ROOT   (want 1)"
if [ "$SCRIPT_AT_ROOT" != "1" ]; then
    echo "FAIL: PocketTracker.sh is not at the zip root - harbourmaster installs the items from there."
    exit 1
fi

GAMEINFO_IMG=$(sed -n 's:.*<image>\./\(.*\)</image>.*:\1:p' "$STAGE/gameinfo.xml")
IMG_BYTES=$(unzip -p "$OUT/pockettracker.zip" "$GAMEINFO_IMG" | wc -c) || IMG_BYTES=0
echo "gameinfo <image>          : $GAMEINFO_IMG -> $IMG_BYTES bytes in the zip"
if [ "$IMG_BYTES" -lt 100 ]; then
    echo "FAIL: gameinfo.xml points at $GAMEINFO_IMG, which is not in the zip (or is empty)."
    exit 1
fi

# ⚠️ Out of the zip, not the staging folder: the zip is what ships.
echo
echo "licences, read back out of the zip:"
for L in LICENSE THIRD-PARTY-NOTICES.md CREDITS.md libogg-COPYING libopus-COPYING \
         libopus-LICENSE_PLEASE_READ.txt OFL-1.1-LiberationSans.txt OFL-1.1-QuinqueFive.txt; do
    # ⚠️ `|| BYTES=0`, for the same reason: the FAIL line must name the file.
    BYTES=$(unzip -p "$OUT/pockettracker.zip" "pockettracker/licenses/$L" | wc -c) || BYTES=0
    echo "  $BYTES bytes   $L"
    if [ "$BYTES" -lt 100 ]; then
        echo "FAIL: pockettracker/licenses/$L is missing or empty inside the zip."
        exit 1
    fi
done
