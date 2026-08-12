#!/bin/bash
#
# Build the PortMaster (aarch64) package: build/portmaster/pockettracker.zip
#
# Run from the repo root, on a box with docker:
#
#     docker build -t pockettracker-build -f shell/Dockerfile.portmaster shell/
#     docker run --rm -v "$PWD:/src" pockettracker-build bash /src/shell/build-portmaster.sh
#
# ⚠️ Do NOT run this on the host toolchain just because the host HAS an aarch64 cross compiler.
# It will build, link and produce a perfectly good ELF that no handheld can load. The container is
# the glibc floor; see shell/Dockerfile.portmaster.
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
# 2.0.0-era. Raise this ONLY together with the requirement in shell/portmaster/README.md.
SDL2_TAG=${SDL2_TAG:-release-2.0.18}

# Every Tier-1 CFW is at or above Ubuntu 20.04's glibc. This is asserted on the artifact below.
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
    -DSDL2_DIR="$SYSROOT/lib/cmake/SDL2"
cmake --build "$BUILD"

echo
echo "############ 3/5  stage the package ############"
# TWO layouts come out of this build, and they are not the same tree.
#
# $STAGE is the CATALOG REPO layout: exactly what gets copied into PortMaster-New's
# `ports/pockettracker/`. Its five root files are the ones `tools/build_release.py` requires there
# (port.json, README.md, screenshot.{png|jpg}, gameinfo.xml, the launch script) — a screenshot one
# level down reads to that checker as a port with no screenshot at all.
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
# The ZIP is a different shape, and which shape is not ours to pick: upstream's `build_port_zip()`
# moves those four metadata files INTO the port directory, renames README.md to `<portname>.md`, and
# leaves only the launch script at the root. Step 5 reproduces that transform, so the zip a tester
# installs is the zip the catalog will later hand a user — and so `gameinfo.xml`'s
# `./pockettracker/screenshot.png` resolves on the device, which is the same file the checker wanted
# at the root. One file, two correct places, one transform between them.
#
# ⚠️ The two names must agree: the port directory and the binary's stem are both `pockettracker`
# (lowercase, the catalog's rule), while the launch script is capitalised.
rm -rf "$OUT"
mkdir -p "$STAGE/pockettracker/licenses"

cp "$SRC/shell/portmaster/PocketTracker.sh" "$STAGE/"
cp "$SRC/shell/portmaster/port.json"        "$STAGE/"
cp "$SRC/shell/portmaster/gameinfo.xml"     "$STAGE/"
cp "$SRC/shell/portmaster/README.md"        "$STAGE/"
chmod +x "$STAGE/PocketTracker.sh"

# The art. It is staged at the ROOT here because that is where the catalog checker looks; step 5 puts
# it where `gameinfo.xml` points the CFW's game list (`<image>./pockettracker/screenshot.png`).
# `port.json` names no image at all — the catalog picks up this file by name, the way the shipped
# LittleGPTracker port does.
#
# ⚠️ It is the ANDROID capture, and that is deliberate rather than lazy: both frontends render the
# same 640x480 design through the same 5x5 font, so it shows what the handheld actually draws. It is
# 1280x960 — a 2x capture, so exactly 4:3 and comfortably over PortMaster's 640x480 floor — and it
# shows the SONG screen mid-edit rather than a title card, which the porting guide asks for.
cp "$SRC/docs/images/screenshot.png" "$STAGE/screenshot.png"

cp "$BUILD/pockettracker-sdl" "$BIN"
chmod +x "$BIN"
aarch64-linux-gnu-strip "$BIN"

# PocketTracker is GPL-3.0, and it statically links its decoders — so their notices ship with the
# binary that contains them, not just with the source tree that built it. A missing one is a licence
# breach in the artifact, which is the only thing a user ever receives.
#
# ⚠️ A vendored component can arrive with its own notice already missing — KissFFT's copyright banner
# was stripped from the source it was copied from, and opusfile carries no COPYING at all despite
# every one of its headers pointing at one. So `docs/licenses/THIRD-PARTY-NOTICES.md` is the single
# source of truth, and reproduces the text inline where upstream gave us no file to ship. Add a
# component there in the same commit that vendors it; the check further down enforces it.
cp "$SRC/LICENSE"                                       "$STAGE/pockettracker/licenses/LICENSE"
cp "$SRC/docs/licenses/THIRD-PARTY-NOTICES.md"          "$STAGE/pockettracker/licenses/"
cp "$SRC/CREDITS.md"                                    "$STAGE/pockettracker/licenses/CREDITS.md"
cp "$SRC/native/vendor/ogg/COPYING"                     "$STAGE/pockettracker/licenses/libogg-COPYING"
cp "$SRC/native/vendor/opus/COPYING"                    "$STAGE/pockettracker/licenses/libopus-COPYING"
cp "$SRC/native/vendor/opus/LICENSE_PLEASE_READ.txt"    "$STAGE/pockettracker/licenses/libopus-LICENSE_PLEASE_READ.txt"

# The OFL text ships even though this package bundles no font. THIRD-PARTY-NOTICES.md, which does
# ship, cites licenses/OFL-1.1-LinuxBiolinum.txt by path — a notices file pointing at a file that is
# not in the artifact is the breach it exists to prevent. And if assets/fonts/ ever travels with a
# handheld build, the OFL's "must accompany the font" clause is already satisfied rather than newly
# breached.
cp "$SRC/docs/licenses/OFL-1.1-LinuxBiolinum.txt"       "$STAGE/pockettracker/licenses/OFL-1.1-LinuxBiolinum.txt"
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

# --- every statically linked component must have a notice -------------------------------------
# ⚠️ DERIVED FROM THE TREE, NOT FROM A LIST SOMEONE MUST REMEMBER TO UPDATE. The failure this
# guards against already happened once: libraries were vendored and their notices were never added,
# and no list-based check could have caught it, because the same commit that forgets the notice
# forgets the list entry. So the vendor directory IS the list — add `native/vendor/foo/` and this
# build fails until THIRD-PARTY-NOTICES.md names `foo`.
NOTICES="$STAGE/pockettracker/licenses/THIRD-PARTY-NOTICES.md"
echo
echo "licence notices:"
MISSING=""
# ⚠️ `basename`, not `ls -1`: some shells list directories with a trailing slash, and `dr_flac/`
# then matches the PATH `native/vendor/dr_flac/` in this file's prose rather than the component
# name — the check would pass without ever proving a notice exists. It did exactly that when first
# written. Look at WHERE a check fires, not just whether it went green.
VENDORED=""
for D in "$SRC"/native/vendor/*/; do VENDORED="$VENDORED $(basename "$D")"; done
# The vendored tree, plus the three third-party components that live outside it.
#
# ⚠️ Since convergence C1 this list also contains SDL2, which THIS artifact does not ship: the
# PortMaster zip links the device's own libSDL2 (see the SDL2_TAG floor above), and native/vendor/SDL2
# exists for the Android build, which has to carry SDL in-tree because F-Droid compiles offline from
# source. Over-inclusive on purpose — the question this loop asks is "is every vendored component
# documented?", and the answer must be yes regardless of which artifact links which. THIRD-PARTY-NOTICES.md
# is the file that then answers per artifact; it has a table for exactly this.
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
# `build_port_zip()` in PortMaster-New/tools/build_release.py: the port directory is copied verbatim,
# the four root metadata files are moved INTO it, README.md becomes `<portname>.md`, and only the
# launch script stays at the zip root. Mirrored here so the zip posted to a tester and the zip the
# catalog builds from the same tree are the same artifact — otherwise the beta tests a layout no
# user will ever install.
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
# Two ways it can go wrong, each failing for a reason the other cannot: harbourmaster runs the launch
# script from the ZIP ROOT (it is an `items` entry), and the CFW's game list reads the screenshot at
# the path gameinfo.xml names — one level down, which is where the checker did NOT want it. So the
# same file being in the right place for one consumer says nothing about the other.
#
# ⚠️ Both `got`s are derived from the artifact, and both need `|| ...=0`: grep exits 1 on no match
# and unzip exits 11 on a missing member, either of which kills the script under `set -euo pipefail`
# before the FAIL line that would have named the problem.
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

# ⚠️ READ THE LICENCES BACK OUT OF THE ZIP, not out of the staging dir. Everything above this line
# inspected files that a broken `zip` step could still have failed to include — and the zip is what
# ships. Same discipline as verifying the launch script's CR bytes out of the archive.
echo
echo "licences, read back out of the zip:"
for L in LICENSE THIRD-PARTY-NOTICES.md CREDITS.md libogg-COPYING libopus-COPYING \
         libopus-LICENSE_PLEASE_READ.txt OFL-1.1-LinuxBiolinum.txt; do
    # ⚠️ `|| BYTES=0` is what makes the failure READABLE. A member that is not in the archive makes
    # unzip exit 11, and under `set -euo pipefail` that kills the script mid-loop — before the FAIL
    # line below, so the package is rejected without ever naming the file that is missing. Absent and
    # empty are the same verdict here, so they take the same path.
    BYTES=$(unzip -p "$OUT/pockettracker.zip" "pockettracker/licenses/$L" | wc -c) || BYTES=0
    echo "  $BYTES bytes   $L"
    if [ "$BYTES" -lt 100 ]; then
        echo "FAIL: pockettracker/licenses/$L is missing or empty inside the zip."
        exit 1
    fi
done
