#!/bin/bash
#
# Build the OnionOS (Miyoo Mini / Mini+) package: build/miyoo/pockettracker-miyoo.zip
#
# Run from the repo root, on any Linux box (WSL is fine) with cmake, ninja, zip and qemu-arm-static:
#
#     sudo apt install cmake ninja-build zip qemu-user-static
#     bash shell/build-miyoo.sh
#
# ⚠️ NO CONTAINER, and that is the difference from build-portmaster.sh. That build needs
# ubuntu:20.04 because it links the host distro's glibc and the container IS the floor. Here the
# floor arrives with the compiler: shauninman's device toolchain carries its own glibc 2.28 sysroot,
# so the ceiling asserted below is a property of the toolchain rather than of the machine you are
# sitting at. What this script must not do is fall back on a system arm-linux-gnueabihf — that one
# finds its headers in the HOST's /usr/include and produces an ELF the device cannot load.
#
# ⚠️ Nothing here is filtered down to the word "error". Every command prints its own output. Four
# separate sessions of this project have been burned by a build step that failed quietly and left a
# stale artifact behind for the next step to "verify".
set -euo pipefail

SRC=${SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
CACHE=${MIYOO_CACHE:-$HOME/.cache/pockettracker-miyoo}

# ─── The two downloads, pinned ───────────────────────────────────────────────────────────────────
#
# ⚠️ BOTH ARE PINNED BY TAG, NOT BY "latest". A silently newer SDL2 fork is a different set of
# undocumented limitations under a package we cannot test on hardware ourselves, and the whole point
# of §Phase 2 is that the fork is the largest unknown in this port.
TOOLCHAIN_TAG=${TOOLCHAIN_TAG:-v0.0.3}
TOOLCHAIN_URL="https://github.com/shauninman/miyoomini-toolchain-buildroot/releases/download/$TOOLCHAIN_TAG/miyoomini-toolchain.tar.xz"

# The **nogl** variant, deliberately. The GL build carries a 21 MB swiftshader libGLESv2, and this
# app never asks for a GL renderer: sdl-video.cpp creates one STREAMING texture and blits it.
SDL2_TAG=${SDL2_TAG:-sdl2-miyoo-f6319cf9}
SDL2_URL="https://github.com/XK9274/sdl2_miyoo/releases/download/$SDL2_TAG/libSDL2-miyoo-nogl-f6319cf9.tar.gz"

# Read off the toolchain's OWN sysroot (arm-linux-gnueabihf/libc/lib/libc-2.28.so), not guessed at,
# and corroborated by the SDL2 fork: that .so — which every Miyoo port on every one of these devices
# already loads — demands at most GLIBC_2.27.
GLIBC_MAX_ALLOWED=${GLIBC_MAX_ALLOWED:-2.28}

TOOLCHAIN=$CACHE/miyoomini-toolchain
SDL2_LIBDIR=$CACHE/sdl2
SDK=$CACHE/sdk
STUBS=$CACHE/stubs
# ⚠️ THE BUILD TREE IS A SIBLING OF THE OUTPUT, NOT A CHILD OF IT. Step 3 starts `rm -rf "$OUT"`, so
# a build tree under it is deleted with the staging directory and step 3 then cannot find the binary
# it was about to copy.
BUILD=$SRC/build/miyoo-cmake
OUT=$SRC/build/miyoo
STAGE=$OUT/stage
APPDIR=$STAGE/App/PocketTracker
BIN=$APPDIR/pockettracker

cd "$SRC"

echo
echo "############ 1/5  fetch the device toolchain and the SDL2 fork ############"
mkdir -p "$CACHE/dl"
fetch() {   # url, destination file
    [ -f "$2" ] && { echo "cached  $(basename "$2")"; return; }
    echo "fetch   $1"
    curl -sSL --retry 3 -o "$2.part" "$1"
    mv "$2.part" "$2"
}
fetch "$TOOLCHAIN_URL" "$CACHE/dl/miyoomini-toolchain.tar.xz"
fetch "$SDL2_URL"      "$CACHE/dl/sdl2-nogl.tar.gz"

[ -d "$TOOLCHAIN" ]   || { mkdir -p "$CACHE/tc"  && tar -xf  "$CACHE/dl/miyoomini-toolchain.tar.xz" -C "$CACHE/tc" && mv "$CACHE/tc/miyoomini-toolchain" "$TOOLCHAIN"; }
[ -d "$SDL2_LIBDIR" ] || { mkdir -p "$SDL2_LIBDIR" && tar -xzf "$CACHE/dl/sdl2-nogl.tar.gz" -C "$SDL2_LIBDIR" --strip-components=1; }

CROSS="$TOOLCHAIN/bin/arm-linux-gnueabihf-"
"${CROSS}gcc" --version | head -1

# ⚠️ THE SDL2 HERE IS BOTH A LINK SDK AND A SHIPPED LIBRARY, which is the exact inverse of the
# PortMaster package. There is no system libSDL2 on this device, so the headers we compile against
# and the .so the device loads have to be the same build or a mismatch shows up as a wrong-looking
# app rather than as a link error. The headers come from the fork at the same pinned commit.
SDL2_SRC=$CACHE/sdl2-src
if [ ! -d "$SDL2_SRC/.git" ]; then
    git clone --depth 1 https://github.com/XK9274/sdl2_miyoo.git "$SDL2_SRC"
fi
# ⚠️ `tag $SDL2_TAG`, not the abbreviated sha in the tag's name: GitHub's smart protocol will not
# serve an abbreviated commit to `fetch`, and the failure — "couldn't find remote ref" — reads like
# the pin is wrong rather than like the spelling is.
git -C "$SDL2_SRC" fetch --depth 1 origin tag "$SDL2_TAG"
git -C "$SDL2_SRC" checkout -q "refs/tags/$SDL2_TAG"
git -C "$SDL2_SRC" log -1 --format='SDL2 fork      : %H  (%s)'

rm -rf "$SDK"
mkdir -p "$SDK/include/SDL2" "$SDK/lib/cmake/SDL2"
cp "$SDL2_SRC"/include/*.h "$SDK/include/SDL2/"
cp "$SDL2_LIBDIR/libSDL2-2.0.so.0" "$SDK/lib/"
ln -sf libSDL2-2.0.so.0 "$SDK/lib/libSDL2.so"
cat > "$SDK/lib/cmake/SDL2/SDL2Config.cmake" <<'EOF'
get_filename_component(_sdl2_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(SDL2_FOUND TRUE)
set(SDL2_INCLUDE_DIRS "${_sdl2_root}/include/SDL2")
set(SDL2_LIBRARIES    "${_sdl2_root}/lib/libSDL2-2.0.so.0")
if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 SHARED IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION             "${_sdl2_root}/lib/libSDL2-2.0.so.0"
        INTERFACE_INCLUDE_DIRECTORIES "${_sdl2_root}/include/SDL2")
endif()
EOF
printf 'set(PACKAGE_VERSION 2.0.20)\nset(PACKAGE_VERSION_COMPATIBLE TRUE)\n' \
    > "$SDK/lib/cmake/SDL2/SDL2ConfigVersion.cmake"

echo
echo "############ 2/5  cross-build PocketTracker ############"
# ⚠️ `-static-libstdc++ -static-libgcc` costs 370 KB and removes an entire class of unknown: this
# toolchain is gcc 8.3 and the device's own libstdc++ is whatever its buildroot shipped, which is
# not a question anything here can answer. Nothing crosses a C++ ABI boundary at runtime — the
# bundled SDL2 is C — so absorbing it is free of consequence.
#
# ⚠️ `--allow-shlib-undefined` is REPEATED here rather than inherited: toolchain-miyoo.cmake sets it
# through CMAKE_EXE_LINKER_FLAGS_INIT, which only *initializes* the cache variable — passing
# -DCMAKE_EXE_LINKER_FLAGS below replaces it outright, and dropping the flag makes the link fail on
# the SigmaStar libraries that live on the device. The note in the toolchain file says why it is
# needed at all.
rm -rf "$BUILD"
cmake -S "$SRC/shell" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/shell/toolchain-miyoo.cmake" \
    -DMIYOO_TOOLCHAIN="$TOOLCHAIN" \
    -DSDL2_DIR="$SDK/lib/cmake/SDL2" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--allow-shlib-undefined -static-libstdc++ -static-libgcc"
cmake --build "$BUILD"

echo
echo "############ 3/5  stage the Onion APP package ############"
# The zip extracts at the SD CARD ROOT, which is why the staging tree starts with `App/`:
#
#   App/PocketTracker/
#   |- config.json          the Onion shelf entry (label / icon / launch)
#   |- icon.png             74x74, Onion's own size
#   |- launch.sh            the three mmiyoo exports, POCKETTRACKER_HOME, the config seed
#   |- miyoo-config.json    the app's OWN config.json, seeded to $POCKETTRACKER_HOME on first run
#   |- pockettracker        the armhf ELF
#   |- libs/                libSDL2-2.0.so.0 + libneonarmmiyoo.so
#   `- licenses/
#
# ⚠️ THE TWO config.json FILES ARE DIFFERENT FILES and both would be `config.json` in this directory.
# Onion owns the name here, so the app's copy travels as `miyoo-config.json` and launch.sh puts it
# where the app looks. See the block in launch.sh.
#
# ⚠️ The APP shelf, not the Ports shelf. Ports' _required_files.txt / GameDataFile machinery exists
# for engines that need licensed assets the user supplies; we ship a complete app.
rm -rf "$OUT"
mkdir -p "$APPDIR/libs" "$APPDIR/licenses"

cp "$SRC/shell/miyoo/config.json"        "$APPDIR/"
cp "$SRC/shell/miyoo/icon.png"           "$APPDIR/"
cp "$SRC/shell/miyoo/launch.sh"          "$APPDIR/"
cp "$SRC/shell/miyoo/miyoo-config.json"  "$APPDIR/"
cp "$SRC/shell/miyoo/README.md"          "$APPDIR/"
chmod +x "$APPDIR/launch.sh"

cp "$BUILD/pockettracker-sdl" "$BIN"
chmod +x "$BIN"
"${CROSS}strip" "$BIN"

cp "$SDL2_LIBDIR/libSDL2-2.0.so.0"   "$APPDIR/libs/"
cp "$SDL2_LIBDIR/libneonarmmiyoo.so" "$APPDIR/libs/"

# PocketTracker is GPL-3.0 and statically links its decoders, so their notices ship with the binary
# that contains them. `docs/licenses/THIRD-PARTY-NOTICES.md` is the single source of truth and the
# check in step 4 derives the required list from native/vendor/ rather than from a hand-kept one.
cp "$SRC/LICENSE"                                    "$APPDIR/licenses/LICENSE"
cp "$SRC/docs/licenses/THIRD-PARTY-NOTICES.md"       "$APPDIR/licenses/"
cp "$SRC/CREDITS.md"                                 "$APPDIR/licenses/CREDITS.md"
cp "$SRC/native/vendor/ogg/COPYING"                  "$APPDIR/licenses/libogg-COPYING"
cp "$SRC/native/vendor/opus/COPYING"                 "$APPDIR/licenses/libopus-COPYING"
cp "$SRC/native/vendor/opus/LICENSE_PLEASE_READ.txt" "$APPDIR/licenses/libopus-LICENSE_PLEASE_READ.txt"
cp "$SRC/docs/licenses/OFL-1.1-LinuxBiolinum.txt"    "$APPDIR/licenses/OFL-1.1-LinuxBiolinum.txt"

# ⚠️ THESE TWO SHIP ONLY HERE. Every other package links the device's own SDL2 and therefore owes it
# no notice; this one carries the binary, so it carries SDL's zlib text and the fork's GPL-3.0 text
# with it. Taken from the fork's own tree at the pinned commit rather than from a copy of our own.
cp "$SDL2_SRC/LICENSE.txt" "$APPDIR/licenses/libSDL2-zlib-LICENSE.txt"
cp "$SDL2_SRC/LICENSE"     "$APPDIR/licenses/libSDL2-miyoo-fork-GPL-3.0.txt"
ls -1 "$APPDIR/licenses/"

echo
echo "############ 4/5  verify the ARTIFACT (not the build log) ############"
file "$BIN"
echo

# --- 1. the word size and the float ABI -------------------------------------------------------
FILE_OUT=$(file -b "$BIN")
echo "ELF                : $FILE_OUT"
case "$FILE_OUT" in
    *"ELF 32-bit LSB"*"ARM"*"EABI5"*) ;;
    *) echo "FAIL: not a 32-bit ARM EABI5 ELF. The toolchain file did not take."; exit 1 ;;
esac

# --- 2. the CPU it was compiled FOR ------------------------------------------------------------
# ⚠️ Fails for a reason nothing else here does: -mcpu/-mfpu silently not applied. It is also NOT a
# substitute for check 3 — the attributes said v7 while the binary contained ARMv8 instructions.
CPU_ARCH=$("${CROSS}readelf" -A "$BIN" | sed -n 's/.*Tag_CPU_arch: *//p' | head -1)
FP_ARCH=$( "${CROSS}readelf" -A "$BIN" | sed -n 's/.*Tag_FP_arch: *//p'  | head -1)
VFP_ARGS=$("${CROSS}readelf" -A "$BIN" | sed -n 's/.*Tag_ABI_VFP_args: *//p' | head -1)
echo "CPU arch           : $CPU_ARCH   (want v7)"
echo "FP arch            : $FP_ARCH   (want VFPv4)"
echo "VFP args           : $VFP_ARGS   (want VFP registers - hard float)"
[ "$CPU_ARCH" = "v7" ]                  || { echo "FAIL: built for $CPU_ARCH, not ARMv7."; exit 1; }
[ "$FP_ARCH"  = "VFPv4" ]               || { echo "FAIL: FP arch is $FP_ARCH, not VFPv4."; exit 1; }
[ "$VFP_ARGS" = "VFP registers" ]       || { echo "FAIL: soft-float ABI - the device is hard-float."; exit 1; }

# --- 3. no ARMv8-only instruction anywhere in the image ----------------------------------------
# ⚠️⚠️ THE ONE CHECK THE FLAGS CANNOT MAKE FOR YOU. Inline asm goes past the compiler's idea of the
# target entirely: a vendored DSP header guarded `vmaxnm.f32` on `__arm__`, which is true on every
# 32-bit ARM, and the instruction exists only from ARMv8 (and on Cortex-M's FPv5). It would run on
# qemu-user, whose default CPU model has it, and SIGILL on a Cortex-A7.
V8_COUNT=$("${CROSS}objdump" -d "$BIN" | grep -cE '\b(vmaxnm|vminnm|vrint[apmnxz]|vcvta|vcvtn|vcvtp|vcvtm|vsel)\b' || true)
echo "ARMv8-only insns   : $V8_COUNT   (want 0)"
if [ "$V8_COUNT" != "0" ]; then
    echo "FAIL: the image contains ARMv8 instructions a Cortex-A7 cannot execute."
    echo "      Look for inline asm guarded on __arm__ rather than on an ACLE feature macro."
    exit 1
fi

# --- 4. the glibc ceiling ----------------------------------------------------------------------
GLIBC_MAX=$(readelf -V "$BIN" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sed 's/GLIBC_//' | sort -V | tail -1)
echo "max GLIBC required : $GLIBC_MAX   (must be <= $GLIBC_MAX_ALLOWED)"
if [ "$(printf '%s\n%s\n' "$GLIBC_MAX" "$GLIBC_MAX_ALLOWED" | sort -V | tail -1)" != "$GLIBC_MAX_ALLOWED" ]; then
    echo "FAIL: demands glibc $GLIBC_MAX. You are not building with the device toolchain."
    exit 1
fi

# --- 5. SDL2 is dynamic, and it is the one we SHIP (the PortMaster check, inverted) ------------
echo
echo "dynamic deps:"
readelf -d "$BIN" | grep NEEDED
if ! readelf -d "$BIN" | grep -q 'libSDL2-2.0.so.0'; then
    echo "FAIL: not dynamically linked against libSDL2 - it got statically absorbed."
    exit 1
fi
for L in libSDL2-2.0.so.0 libneonarmmiyoo.so; do
    if [ ! -f "$APPDIR/libs/$L" ]; then
        echo "FAIL: libs/$L is ABSENT. This device has no system SDL2 - the app would not start."
        exit 1
    fi
    echo "bundled            : libs/$L  ($(stat -c%s "$APPDIR/libs/$L") bytes)"
done
if readelf -d "$BIN" | grep -q 'libstdc++'; then
    echo "FAIL: libstdc++ is a NEEDED. It was meant to be absorbed - the device's copy is unknown."
    exit 1
fi
echo "libstdc++          : absorbed (not a runtime dependency)"

# --- 6. the ten-button map, resolved through the SDL2 THAT SHIPS -------------------------------
# ⚠️ THE ONLY PART OF THE KEYMAP THAT CAN BE CHECKED WITHOUT THE DEVICE, and its failure is quiet:
# a key name SDL cannot resolve leaves that button doing nothing at all. So the names are read OUT
# of miyoo-config.json (never retyped here — a hand-typed pair prints "got off, want off" forever)
# and handed to the shipped libSDL2 under qemu.
#
# ⚠️ The vendor libraries libSDL2 needs are on the DEVICE. Empty stubs with the right sonames are
# what let the loader finish; SDL_GetKeyFromName touches none of them and needs no SDL_Init.
command -v qemu-arm-static >/dev/null || { echo "FAIL: qemu-arm-static missing (apt install qemu-user-static)."; exit 1; }
mkdir -p "$STUBS"
: > "$CACHE/empty.c"
for L in libmi_common libmi_sys libmi_gfx libmi_ao libcam_os_wrapper; do
    [ -f "$STUBS/$L.so" ] || "${CROSS}gcc" -shared -Wl,-soname,"$L.so" -o "$STUBS/$L.so" -x c "$CACHE/empty.c"
done
cp -f "$SDL2_LIBDIR/libneonarmmiyoo.so" "$STUBS/"

cat > "$CACHE/keycheck.c" <<'EOF'
#include <stdio.h>
#include <SDL.h>
int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        SDL_Keycode k = SDL_GetKeyFromName(argv[i]);
        if (k == SDLK_UNKNOWN) { printf("  UNRESOLVED  %s\n", argv[i]); bad++; }
    }
    printf("unresolved = %d\n", bad);
    return bad != 0 ? 1 : 0;
}
EOF
"${CROSS}gcc" -O1 -I"$SDK/include/SDL2" -o "$CACHE/keycheck" "$CACHE/keycheck.c" \
    -L"$SDK/lib" -lSDL2 -Wl,--allow-shlib-undefined

# Every quoted string to the RIGHT of a colon inside the "keyboard" object — i.e. the key names,
# never the button names.
mapfile -t KEYNAMES < <(sed -n '/"keyboard"/,/^  }/p' "$APPDIR/miyoo-config.json" \
    | sed 's/^[^:]*://' | grep -oE '"[^"]+"' | tr -d '"')
echo
echo "key names in miyoo-config.json : ${#KEYNAMES[@]}"
[ "${#KEYNAMES[@]}" -ge 12 ] || { echo "FAIL: only ${#KEYNAMES[@]} key names parsed - the sed no longer matches the file."; exit 1; }

run_keycheck() {   # names... -> prints the tool's own line, returns its exit code
    qemu-arm-static -L "$TOOLCHAIN/arm-linux-gnueabihf/libc" \
        -E LD_LIBRARY_PATH="$SDK/lib:$STUBS" "$CACHE/keycheck" "$@"
}
if ! run_keycheck "${KEYNAMES[@]}"; then
    echo "FAIL: a key name in miyoo-config.json is not one SDL answers to - that button does nothing."
    exit 1
fi
# ⚠️ THE CONTROL, and it is not ceremony: this check's pass is "nothing was printed", which cannot
# tell a working lookup from a keycheck that never ran.
if run_keycheck "Nonexistent Key" >/dev/null 2>&1; then
    echo "FAIL: the control passed - keycheck resolves a name that does not exist, so it proves nothing."
    exit 1
fi
echo "control            : an invented name IS rejected"

# --- 7. no key doing two jobs -------------------------------------------------------------------
# ⚠️ A DIFFERENT FAILURE FROM 6, and the one §3.4 of the scope warns about: a name can resolve
# perfectly and still be bound to two buttons, and the symptom then looks like the gptokeyb
# double-input bug rather than like a config typo.
DUPES=$(printf '%s\n' "${KEYNAMES[@]}" | sort | uniq -d | tr '\n' ' ')
echo "keys bound twice   : ${DUPES:-none}"
[ -z "$DUPES" ] || { echo "FAIL: those keys are bound to more than one button."; exit 1; }

# --- 8. every statically linked component must have a notice -----------------------------------
# ⚠️ DERIVED FROM THE TREE, NOT FROM A LIST SOMEONE MUST REMEMBER TO UPDATE — the same commit that
# forgets the notice forgets the list entry. So the vendor directory IS the list.
NOTICES="$APPDIR/licenses/THIRD-PARTY-NOTICES.md"
echo
echo "licence notices:"
MISSING=""
VENDORED=""
for D in "$SRC"/native/vendor/*/; do VENDORED="$VENDORED $(basename "$D")"; done
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
    exit 1
fi

echo
echo "############ 5/5  zip (extracts at the SD card root) ############"
# ⚠️ MODES ARE SET HERE, NOT INHERITED. `zip` records whatever `stat` reports, and Onion runs
# launch.sh — which, unlike the binary, cannot repair its own bit once the card is in the device.
find "$APPDIR" -type f -exec chmod 644 {} +
chmod 755 "$APPDIR/launch.sh" "$BIN" "$APPDIR/libs"/*.so*
# ⚠️ …and on a filesystem that carries no Unix modes — a Windows drive mounted under WSL is the one
# that will actually happen — the chmod above is silently a no-op and every member is recorded 0777.
# That still RUNS, so nothing here fails on it; it is said out loud instead, because "the package
# came out with different permissions" is otherwise invisible until a user reports it.
STAGED_MODE=$(stat -c%a "$APPDIR/config.json")
if [ "$STAGED_MODE" != "644" ]; then
    echo "NOTE: this filesystem reports mode $STAGED_MODE where 644 was set, so it carries no Unix"
    echo "      modes and every zip member will be 0777. Fine for a test build; build a RELEASE on a"
    echo "      native Linux filesystem."
fi
rm -f "$OUT/pockettracker-miyoo.zip"
( cd "$STAGE" && zip -r "$OUT/pockettracker-miyoo.zip" App -x '.*' )
echo
ls -lh "$OUT/pockettracker-miyoo.zip"
unzip -l "$OUT/pockettracker-miyoo.zip"

# ⚠️ READ IT BACK OUT OF THE ZIP. Everything above inspected files a broken `zip` step could still
# have failed to include, and the zip is what ships.
echo
echo "read back out of the zip:"
for M in App/PocketTracker/pockettracker \
         App/PocketTracker/launch.sh \
         App/PocketTracker/config.json \
         App/PocketTracker/miyoo-config.json \
         App/PocketTracker/icon.png \
         App/PocketTracker/README.md \
         App/PocketTracker/libs/libSDL2-2.0.so.0 \
         App/PocketTracker/libs/libneonarmmiyoo.so \
         App/PocketTracker/licenses/LICENSE \
         App/PocketTracker/licenses/THIRD-PARTY-NOTICES.md \
         App/PocketTracker/licenses/CREDITS.md \
         App/PocketTracker/licenses/libogg-COPYING \
         App/PocketTracker/licenses/libopus-COPYING \
         App/PocketTracker/licenses/libopus-LICENSE_PLEASE_READ.txt \
         App/PocketTracker/licenses/OFL-1.1-LinuxBiolinum.txt \
         App/PocketTracker/licenses/libSDL2-zlib-LICENSE.txt \
         App/PocketTracker/licenses/libSDL2-miyoo-fork-GPL-3.0.txt ; do
    # ⚠️ `|| BYTES=0` is what makes the failure READABLE: unzip exits 11 on a missing member, and
    # under `set -euo pipefail` that kills the script before the line that would have named it.
    BYTES=$(unzip -p "$OUT/pockettracker-miyoo.zip" "$M" | wc -c) || BYTES=0
    printf '  %9s  %s\n' "$BYTES" "$M"
    if [ "$BYTES" -lt 60 ]; then
        echo "FAIL: $M is missing or empty inside the zip."
        exit 1
    fi
done

echo
echo "OK  $OUT/pockettracker-miyoo.zip"
echo "    Extract at the SD card root; it lands in App/PocketTracker/ and appears on Onion's Apps shelf."
