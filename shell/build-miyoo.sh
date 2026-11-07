#!/bin/bash
#
# Build the OnionOS (Miyoo Mini / Mini+) package: build/miyoo/pockettracker-miyoo.zip
#
# Run from the repo root, on any Linux box with cmake, ninja, zip and qemu-arm-static:
#
#     sudo apt install cmake ninja-build zip qemu-user-static
#     bash shell/build-miyoo.sh
#
# No container: the device toolchain carries its own glibc 2.28 sysroot, so the floor comes with the
# compiler. ⚠️ Never a system arm-linux-gnueabihf: it reads the host's headers and builds an ELF the
# device cannot load.
# ⚠️ Nothing is filtered: a step that fails quietly leaves a stale artifact for the next to "verify".
set -euo pipefail

SRC=${SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
CACHE=${MIYOO_CACHE:-$HOME/.cache/pockettracker-miyoo}

# ─── The two downloads, pinned ───────────────────────────────────────────────────────────────────
#
# ⚠️ Pinned by tag, never "latest": a newer SDL2 fork is new undocumented behaviour on hardware we
# rarely hold.
TOOLCHAIN_TAG=${TOOLCHAIN_TAG:-v0.0.3}
TOOLCHAIN_URL="https://github.com/shauninman/miyoomini-toolchain-buildroot/releases/download/$TOOLCHAIN_TAG/miyoomini-toolchain.tar.xz"

# The nogl variant: the GL one carries a 21 MB libGLESv2, and the app only blits one streaming texture.
SDL2_TAG=${SDL2_TAG:-sdl2-miyoo-f6319cf9}
SDL2_URL="https://github.com/XK9274/sdl2_miyoo/releases/download/$SDL2_TAG/libSDL2-miyoo-nogl-f6319cf9.tar.gz"

# The toolchain sysroot's own glibc (libc-2.28.so).
GLIBC_MAX_ALLOWED=${GLIBC_MAX_ALLOWED:-2.28}

TOOLCHAIN=$CACHE/miyoomini-toolchain
SDL2_LIBDIR=$CACHE/sdl2
SDK=$CACHE/sdk
STUBS=$CACHE/stubs
# ⚠️ Beside $OUT, not inside it: step 3 starts with `rm -rf "$OUT"`.
BUILD=$SRC/build/miyoo-cmake
OUT=$SRC/build/miyoo
STAGE=$OUT/stage
# ⚠️ Under Roms/: Onion records play time and the recent list only for launch commands there
# (`check_is_game`, .tmp_update/runtime.sh).
PORTROOT=$STAGE/Roms/PORTS
GAMEDIR=$PORTROOT/Games/PocketTracker
SHORTCUT=$PORTROOT/Shortcuts/PocketTracker.port
BOXART=$PORTROOT/Imgs/PocketTracker.png
BIN=$GAMEDIR/pockettracker

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
# The fork's tarball has its files at the top level, no enclosing directory.
[ -f "$SDL2_LIBDIR/libSDL2-2.0.so.0" ] || { mkdir -p "$SDL2_LIBDIR" && tar -xzf "$CACHE/dl/sdl2-nogl.tar.gz" -C "$SDL2_LIBDIR"; }
[ -f "$SDL2_LIBDIR/libSDL2-2.0.so.0" ] || { echo "FAIL: libSDL2-2.0.so.0 not found after unpacking the fork's tarball."; exit 1; }

CROSS="$TOOLCHAIN/bin/arm-linux-gnueabihf-"
"${CROSS}gcc" --version | head -1

# ⚠️ This SDL2 is linked against AND shipped (the device has none), so the headers come from the fork
# at the same pinned commit as the .so.
SDL2_SRC=$CACHE/sdl2-src
if [ ! -d "$SDL2_SRC/.git" ]; then
    git clone --depth 1 https://github.com/XK9274/sdl2_miyoo.git "$SDL2_SRC"
fi
# ⚠️ `tag $SDL2_TAG`, not the short sha in its name: GitHub will not fetch an abbreviated commit.
# ⚠️ The fork's repository is gone from GitHub (404 since 2026-10), so a cached checkout is the only
# copy: never fetch when the pinned tag is already here.
git -C "$SDL2_SRC" rev-parse -q --verify "refs/tags/$SDL2_TAG" > /dev/null \
    || git -C "$SDL2_SRC" fetch --depth 1 origin tag "$SDL2_TAG"
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

# ⚠️ Our own libneonarmmiyoo.so, not the tarball's, which has no licence: neon_compat.c defines the
# same 25 symbols in plain C (check 9). The soname is the bare file name libSDL2 asks for.
# ⚠️ The CPU flags repeat toolchain-miyoo.cmake's: this compile skips CMake, and the toolchain's
# default is a VFPv3 target check 2 rejects.
NEON_SO=$SDK/lib/libneonarmmiyoo.so
"${CROSS}gcc" -O2 -fPIC -shared -Wl,-soname,libneonarmmiyoo.so \
    -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard \
    -o "$NEON_SO" "$SRC/shell/miyoo/neon_compat.c"

echo
echo "############ 2/5  cross-build PocketTracker ############"
# Static libstdc++ (370 KB): the device's own is unknown, and nothing C++ crosses into SDL2.
# ⚠️ `--allow-shlib-undefined` repeated: -DCMAKE_EXE_LINKER_FLAGS replaces the toolchain file's
# initial value, and the link then fails on the device's chip libraries.
rm -rf "$BUILD"
cmake -S "$SRC/shell" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/shell/toolchain-miyoo.cmake" \
    -DMIYOO_TOOLCHAIN="$TOOLCHAIN" \
    -DSDL2_DIR="$SDK/lib/cmake/SDL2" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--allow-shlib-undefined -static-libstdc++ -static-libgcc"
cmake --build "$BUILD"

echo
echo "############ 3/5  stage the Onion PORT package ############"
# The zip extracts at the SD CARD ROOT, which is why the staging tree starts with `Roms/`:
#
#   Roms/PORTS/Shortcuts/PocketTracker.port     the list entry Onion runs
#   Roms/PORTS/Imgs/PocketTracker.png           what the list draws beside it
#   Roms/PORTS/Games/PocketTracker/
#   |- launch.sh            the three mmiyoo exports, POCKETTRACKER_HOME, the config seed
#   |- miyoo-config.json    the app's OWN config.json, seeded to $POCKETTRACKER_HOME on first run
#   |- pockettracker        the armhf ELF
#   |- libs/                libSDL2-2.0.so.0 + libneonarmmiyoo.so
#   |- licenses/
#   `- demo/                OPTIONAL: a starter song, copied to the user's folder on first launch
#
# ⚠️ The shortcut and the image pair by base name, which is also what the play-time database stores
# (check 11). The shortcut's presence check names our own binary.
# ⚠️ It needs Onion's Ports Collection (Emu/PORTS/launch_standalone.sh) — a README line, not a check.
rm -rf "$OUT"
mkdir -p "$GAMEDIR/libs" "$GAMEDIR/licenses" "$(dirname "$SHORTCUT")" "$(dirname "$BOXART")"

cp "$SRC/shell/miyoo/PocketTracker.port"  "$SHORTCUT"
cp "$SRC/shell/miyoo/boxart.png"          "$BOXART"
cp "$SRC/shell/miyoo/launch.sh"           "$GAMEDIR/"
cp "$SRC/shell/miyoo/miyoo-config.json"   "$GAMEDIR/"
cp "$SRC/shell/miyoo/README.md"           "$GAMEDIR/"
chmod +x "$GAMEDIR/launch.sh" "$SHORTCUT"

# ── The demo song, if this tree has one ──────────────────────────────────────────────────────────
# Optional and gitignored: other people's samples, fine on a test card, not in a published package.
# Staged beside the app; launch.sh copies it to $POCKETTRACKER_HOME once, so an upgrade never
# overwrites an edited copy.
DEMO_DIR=${DEMO_DIR:-$SRC/shell/miyoo/demo}
if [ -d "$DEMO_DIR" ]; then
    cp -r "$DEMO_DIR" "$GAMEDIR/demo"
    echo "demo song          : staged from $DEMO_DIR ($(du -sh "$GAMEDIR/demo" | cut -f1))"
    find "$GAMEDIR/demo" -type f -printf '  %P\n' | sort
else
    echo "demo song          : none ($DEMO_DIR absent) - packaging without one"
fi

cp "$BUILD/pockettracker-sdl" "$BIN"
chmod +x "$BIN"
"${CROSS}strip" "$BIN"

cp "$SDL2_LIBDIR/libSDL2-2.0.so.0"   "$GAMEDIR/libs/"
cp "$NEON_SO"                        "$GAMEDIR/libs/"

# The statically linked components' notices ship with the binary (THIRD-PARTY-NOTICES.md; check 8).
cp "$SRC/LICENSE"                                    "$GAMEDIR/licenses/LICENSE"
cp "$SRC/docs/licenses/THIRD-PARTY-NOTICES.md"       "$GAMEDIR/licenses/"
cp "$SRC/CREDITS.md"                                 "$GAMEDIR/licenses/CREDITS.md"
cp "$SRC/native/vendor/ogg/COPYING"                  "$GAMEDIR/licenses/libogg-COPYING"
cp "$SRC/native/vendor/opus/COPYING"                 "$GAMEDIR/licenses/libopus-COPYING"
cp "$SRC/native/vendor/opus/LICENSE_PLEASE_READ.txt" "$GAMEDIR/licenses/libopus-LICENSE_PLEASE_READ.txt"
cp "$SRC/docs/licenses/OFL-1.1-LiberationSans.txt"   "$GAMEDIR/licenses/OFL-1.1-LiberationSans.txt"
cp "$SRC/docs/licenses/OFL-1.1-QuinqueFive.txt"      "$GAMEDIR/licenses/OFL-1.1-QuinqueFive.txt"

# ⚠️ Only this package ships SDL2, so only it carries SDL's zlib text and the fork's GPL-3.0 text,
# from the fork's own tree.
cp "$SDL2_SRC/LICENSE.txt" "$GAMEDIR/licenses/libSDL2-zlib-LICENSE.txt"
cp "$SDL2_SRC/LICENSE"     "$GAMEDIR/licenses/libSDL2-miyoo-fork-GPL-3.0.txt"
ls -1 "$GAMEDIR/licenses/"

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

# --- 2 and 3 run over both images we compile: the app, and the helper built outside CMake.
for IMAGE in "$BIN" "$GAMEDIR/libs/libneonarmmiyoo.so"; do
echo "--- $(basename "$IMAGE")"

# --- 2. the CPU it was compiled FOR ------------------------------------------------------------
# ⚠️ Catches -mcpu/-mfpu not applied. Not a substitute for check 3: the attributes can say v7 over
# ARMv8 instructions.
CPU_ARCH=$("${CROSS}readelf" -A "$IMAGE" | sed -n 's/.*Tag_CPU_arch: *//p' | head -1)
FP_ARCH=$( "${CROSS}readelf" -A "$IMAGE" | sed -n 's/.*Tag_FP_arch: *//p'  | head -1)
VFP_ARGS=$("${CROSS}readelf" -A "$IMAGE" | sed -n 's/.*Tag_ABI_VFP_args: *//p' | head -1)
echo "CPU arch           : $CPU_ARCH   (want v7)"
echo "FP arch            : $FP_ARCH   (want VFPv4)"
echo "VFP args           : $VFP_ARGS   (want VFP registers - hard float)"
[ "$CPU_ARCH" = "v7" ]                  || { echo "FAIL: built for $CPU_ARCH, not ARMv7."; exit 1; }
[ "$FP_ARCH"  = "VFPv4" ]               || { echo "FAIL: FP arch is $FP_ARCH, not VFPv4."; exit 1; }
[ "$VFP_ARGS" = "VFP registers" ]       || { echo "FAIL: soft-float ABI - the device is hard-float."; exit 1; }

# --- 3. no ARMv8-only instruction anywhere in the image ----------------------------------------
# ⚠️ Inline asm bypasses the CPU flags: an `__arm__` guard is true on every 32-bit ARM, and an ARMv8
# instruction runs under qemu-user and SIGILLs on a Cortex-A7.
V8_COUNT=$("${CROSS}objdump" -d "$IMAGE" | grep -cE '\b(vmaxnm|vminnm|vrint[apmnxz]|vcvta|vcvtn|vcvtp|vcvtm|vsel)\b' || true)
echo "ARMv8-only insns   : $V8_COUNT   (want 0)"
if [ "$V8_COUNT" != "0" ]; then
    echo "FAIL: the image contains ARMv8 instructions a Cortex-A7 cannot execute."
    echo "      Look for inline asm guarded on __arm__ rather than on an ACLE feature macro."
    exit 1
fi
done

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
    if [ ! -f "$GAMEDIR/libs/$L" ]; then
        echo "FAIL: libs/$L is ABSENT. This device has no system SDL2 - the app would not start."
        exit 1
    fi
    echo "bundled            : libs/$L  ($(stat -c%s "$GAMEDIR/libs/$L") bytes)"
done
if readelf -d "$BIN" | grep -q 'libstdc++'; then
    echo "FAIL: libstdc++ is a NEEDED. It was meant to be absorbed - the device's copy is unknown."
    exit 1
fi
echo "libstdc++          : absorbed (not a runtime dependency)"

# --- 6. the ten-button map, resolved through the SDL2 THAT SHIPS -------------------------------
# ⚠️ A name SDL cannot resolve is a button that silently does nothing. The names are read out of
# miyoo-config.json and handed to the shipped libSDL2 under qemu, as controller names (the Mini's
# buttons arrive as a pad). Empty stubs stand in for the device's chip libraries; the lookups need
# none of them.
command -v qemu-arm-static >/dev/null || { echo "FAIL: qemu-arm-static missing (apt install qemu-user-static)."; exit 1; }
mkdir -p "$STUBS"
: > "$CACHE/empty.c"
for L in libmi_common libmi_sys libmi_gfx libmi_ao libcam_os_wrapper; do
    [ -f "$STUBS/$L.so" ] || "${CROSS}gcc" -shared -Wl,-soname,"$L.so" -o "$STUBS/$L.so" -x c "$CACHE/empty.c"
done
cp -f "$GAMEDIR/libs/libneonarmmiyoo.so" "$STUBS/"

cat > "$CACHE/keycheck.c" <<'EOF'
#include <stdio.h>
#include <SDL.h>
int main(int argc, char** argv) {
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        const SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(argv[i]);
        const int trigger = a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        if (!trigger && SDL_GameControllerGetButtonFromString(argv[i]) == SDL_CONTROLLER_BUTTON_INVALID) {
            printf("  UNRESOLVED  %s\n", argv[i]);
            bad++;
        }
    }
    printf("unresolved = %d\n", bad);
    return bad != 0 ? 1 : 0;
}
EOF
"${CROSS}gcc" -O1 -I"$SDK/include/SDL2" -o "$CACHE/keycheck" "$CACHE/keycheck.c" \
    -L"$SDK/lib" -lSDL2 -Wl,--allow-shlib-undefined

# Every quoted string to the RIGHT of a colon inside the "gamepad" object — i.e. the Miyoo button
# names, never PocketTracker's button names.
mapfile -t CFGNAMES < <(sed -n '/"gamepad"/,/^  }/p' "$GAMEDIR/miyoo-config.json" \
    | sed 's/^[^:]*://' | grep -oE '"[^"]+"' | tr -d '"')
echo
echo "names in miyoo-config.json     : ${#CFGNAMES[@]}"
[ "${#CFGNAMES[@]}" -ge 12 ] || { echo "FAIL: only ${#CFGNAMES[@]} names parsed - the sed no longer matches the file."; exit 1; }

# Miyoo button names ("X", "L1") become controller names through the APP'S OWN translation,
# compiled for the host — never a table retyped here.
cat > "$CACHE/expand.cpp" <<'EOF'
#include "ui/input_config.h"
#include <cstdio>
int main(int argc, char** argv) {
    pt::ui::ButtonBindings b;
    auto& slot = b[pt::ui::Button::A];
    slot.emplace();
    for (int i = 1; i < argc; ++i) slot->emplace_back(argv[i]);
    pt::ui::miyoo_mini_to_pad_names(b);
    for (const std::string& k : *slot) std::puts(k.c_str());
}
EOF
g++ -std=c++17 -O1 -I"$SRC/native" -o "$CACHE/expand" "$CACHE/expand.cpp" "$SRC/native/ui/input_config.cpp"
mapfile -t KEYNAMES < <("$CACHE/expand" "${CFGNAMES[@]}")
echo "controller names they become    : ${#KEYNAMES[@]}"
[ "${#KEYNAMES[@]}" -ge "${#CFGNAMES[@]}" ] || { echo "FAIL: the translation lost names."; exit 1; }

run_keycheck() {   # names... -> prints the tool's own line, returns its exit code
    qemu-arm-static -L "$TOOLCHAIN/arm-linux-gnueabihf/libc" \
        -E LD_LIBRARY_PATH="$SDK/lib:$STUBS" "$CACHE/keycheck" "$@"
}
if ! run_keycheck "${KEYNAMES[@]}"; then
    echo "FAIL: a name in miyoo-config.json is not one SDL answers to - that button does nothing."
    exit 1
fi
# ⚠️ The control: a pass prints nothing, which a keycheck that never ran does too.
if run_keycheck "nonexistentbutton" >/dev/null 2>&1; then
    echo "FAIL: the control passed - keycheck resolves a name that does not exist, so it proves nothing."
    exit 1
fi
echo "control            : an invented name IS rejected"

# --- 7. no button doing two jobs -------------------------------------------------------------------
# A name can resolve and still be bound to two buttons, which on the device looks like double input.
DUPES=$(printf '%s\n' "${KEYNAMES[@]}" | sort | uniq -d | tr '\n' ' ')
echo "bound twice        : ${DUPES:-none}"
[ -z "$DUPES" ] || { echo "FAIL: those Miyoo buttons are bound to more than one app button."; exit 1; }

# --- 8. every statically linked component must have a notice -----------------------------------
# ⚠️ The vendor directory is the list; a hand-kept one would be forgotten by the same commit.
NOTICES="$GAMEDIR/licenses/THIRD-PARTY-NOTICES.md"
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

# --- 9. our libneonarmmiyoo.so defines every symbol the shipped libSDL2 needs from it ----------
# ⚠️ The wanted list is read out of the libSDL2 that ships, so a bumped fork that calls one more
# scaler fails here, not at boot. The qemu run cannot see it: a missing function binds lazily, at
# the first frame.
echo
WANT=$("${CROSS}gcc-nm" -D --undefined-only "$GAMEDIR/libs/libSDL2-2.0.so.0" \
    | grep -oE '\b(neon_memcpy|scale[0-9]+x[0-9]+_n(16|32))\b' | sort -u)
HAVE=$("${CROSS}gcc-nm" -D --defined-only "$GAMEDIR/libs/libneonarmmiyoo.so" \
    | grep -oE '\b(neon_memcpy|scale[0-9]+x[0-9]+_n(16|32))\b' | sort -u)
WANT_N=$(printf '%s\n' "$WANT" | grep -c . || true)
HAVE_N=$(printf '%s\n' "$HAVE" | grep -c . || true)
UNMET=$(comm -23 <(printf '%s\n' "$WANT") <(printf '%s\n' "$HAVE") | tr '\n' ' ')
echo "libSDL2 wants from libneonarmmiyoo : $WANT_N symbols"
echo "our libneonarmmiyoo defines         : $HAVE_N symbols"
echo "unresolved                          : ${UNMET:-none}"
# An empty wanted list would pass as "nothing missing".
if [ "$WANT_N" -lt 25 ]; then
    echo "FAIL: read only $WANT_N wanted symbols out of libSDL2, and it has 25. The reader is broken,"
    echo "      or the pinned fork changed. Check with: ${CROSS}gcc-nm -D --undefined-only <so>"
    exit 1
fi
if [ -n "$UNMET" ]; then
    echo "FAIL: the app cannot load - those symbols are undefined at runtime."
    echo "      Add them to shell/miyoo/neon_compat.c."
    exit 1
fi

# --- 10. every sample and soundfont the demo song names is IN the demo we staged -----------------
# ⚠️ A media path that does not resolve is silent everywhere: the instrument loads empty and plays
# nothing. The paths are read out of the .ptp and re-rooted with the two anchors of
# songcore::app_root_relative_tail (media_path.h), as the device does.
if [ -d "$GAMEDIR/demo" ]; then
echo
# Anchor 1 is "/PocketTracker/"; anchor 2 is the last media sub-tree, kept whole so it re-roots as
# "Samples/…". Only one of the three ever appears in a real path, so loop order is not a tie-break.
demo_tail() {
    local p=$1 sub t best=
    case "$p" in */PocketTracker/*) printf '%s\n' "${p##*/PocketTracker/}"; return ;; esac
    for sub in /Samples/ /Soundfonts/ /Renders/; do
        case "$p" in *"$sub"*) t=${p##*"$sub"}; best="${sub:1}$t" ;; esac
    done
    printf '%s\n' "$best"
}
mapfile -t DEMO_REFS < <(
    find "$GAMEDIR/demo" -name '*.ptp' -print0 | xargs -0 cat \
        | grep -oE '"(sampleFilePath|soundfontPath)":"[^"]+"' \
        | sed 's/^"[^"]*":"//; s/"$//' | sort -u)
echo "demo media named   : ${#DEMO_REFS[@]}"
# An empty parse would pass: it has nothing unresolved.
[ "${#DEMO_REFS[@]}" -ge 1 ] || { echo "FAIL: no sample or soundfont path parsed out of the demo .ptp - the reader is broken."; exit 1; }
DEMO_UNMET=0
for REF in "${DEMO_REFS[@]}"; do
    TAIL=$(demo_tail "$REF")
    if [ -z "$TAIL" ]; then
        # app_root_relative_tail returns "" and the app leaves the path as authored - unfindable here.
        echo "  UNROOTED  $REF"; DEMO_UNMET=$((DEMO_UNMET + 1)); continue
    fi
    if [ -f "$GAMEDIR/demo/$TAIL" ]; then
        echo "  ok        $TAIL"
    # The device's own last step: resolve_case_insensitive, for a project authored where storage did
    # not care about case. Reported rather than passed silently - it only works by that fallback.
    elif FOUND=$(find "$GAMEDIR/demo" -ipath "$GAMEDIR/demo/$TAIL" -type f | head -1) && [ -n "$FOUND" ]; then
        echo "  ok        $TAIL   (CASE DIFFERS on disk: ${FOUND#$GAMEDIR/demo/})"
    else
        echo "  MISSING   $TAIL   <- named by $REF"; DEMO_UNMET=$((DEMO_UNMET + 1))
    fi
done
echo "unresolved         : $DEMO_UNMET   (want 0)"
if [ "$DEMO_UNMET" != "0" ]; then
    echo "FAIL: the demo names media that is not in it. Those instruments load EMPTY on the device and"
    echo "      say nothing. Add the files to $DEMO_DIR, or clear the instruments in the project."
    exit 1
fi
fi

# --- 11. the shortcut, read back with ONION'S OWN PARSE ------------------------------------------
# ⚠️ Four strings, none failing loudly: a wrong folder hides the port (`.notfound`), a wrong executable
# is an entry that does nothing. Read with Onion's own greps (Emu/PORTS/import.sh), which also pick up
# an assignment inside a comment.
echo
port_field() {   # field name -> what Onion's import.sh would read out of the staged shortcut
    grep "$1=" "$SHORTCUT" | cut -d "=" -f2 | grep -o '".*"' | tr -d '"'
}
P_DIR=$(port_field GameDir)
P_EXE=$(port_field GameExecutable)
P_DATA=$(port_field GameDataFile)
printf 'shortcut GameDir   : %s\n'        "${P_DIR:-<EMPTY>}"
printf 'shortcut executable: %s\n'        "${P_EXE:-<EMPTY>}"
printf 'shortcut data file : %s\n'        "${P_DATA:-<EMPTY>}"
# An empty field, or two lines because a comment matched, would make every test below vacuous.
for F in "$P_DIR" "$P_EXE" "$P_DATA"; do
    case "$F" in
        "")  echo "FAIL: a field parsed EMPTY. The shortcut's assignments are not what import.sh reads."; exit 1 ;;
        *"
"*) echo "FAIL: a field parsed as MORE THAN ONE line - a comment in the shortcut carries an assignment."; exit 1 ;;
    esac
done
[ "$P_DIR" = "$(basename "$GAMEDIR")" ] \
    || { echo "FAIL: the shortcut names Games/$P_DIR, and this build staged Games/$(basename "$GAMEDIR")."; exit 1; }
[ -x "$GAMEDIR/$P_EXE" ] \
    || { echo "FAIL: $P_EXE is not an executable file in the staged game directory."; exit 1; }
# Onion's presence check, character for character (launch_standalone.sh and import.sh both run it).
# It is what decides whether the entry appears at all.
FOUND_DATA=$(find "$GAMEDIR" -maxdepth 2 -type f -iname "$P_DATA" | head -1)
echo "presence check finds: ${FOUND_DATA:-<NOTHING>}"
[ -n "$FOUND_DATA" ] \
    || { echo "FAIL: Onion would rename this shortcut to .notfound - the port never appears in the list."; exit 1; }

# The play-time condition, with runtime.sh's own grep, over the path this zip installs to.
INSTALL_PATH=/mnt/SDCARD/${SHORTCUT#"$STAGE"/}
echo "installs to        : $INSTALL_PATH"
echo "$INSTALL_PATH" | grep -q "/mnt/SDCARD/Roms/" \
    || { echo "FAIL: that path is not under Roms/ - no play time, no Game Switcher, no launch count."; exit 1; }

# ⚠️ Artwork pairs by base name alone; a mismatch is a silent missing picture.
echo "artwork pairs with : $(basename "$BOXART" .png)  (shortcut: $(basename "$SHORTCUT" .port))"
[ "$(basename "$BOXART" .png)" = "$(basename "$SHORTCUT" .port)" ] \
    || { echo "FAIL: the image and the shortcut have different base names - the list shows no picture."; exit 1; }

# ⚠️ Onion's audio flag stays off: launch.sh's wait for the chip runs only if launch.sh stops the
# server itself (PocketTracker.port says why).
KILLFLAG=$(grep '^KillAudioserver=' "$SHORTCUT" | cut -d= -f2)
echo "audio flag         : KillAudioserver=$KILLFLAG   (want 0 - launch.sh owns the handover)"
[ "$KILLFLAG" = "0" ] \
    || { echo "FAIL: Onion would stop the sound server without the wait launch.sh does."; exit 1; }

echo
echo "############ 5/5  zip (extracts at the SD card root) ############"
# ⚠️ Modes set, not inherited: zip records what `stat` says, and launch.sh cannot repair its own bit.
find "$PORTROOT" -type f -exec chmod 644 {} +
chmod 755 "$GAMEDIR/launch.sh" "$SHORTCUT" "$BIN" "$GAMEDIR/libs"/*.so*
# On a filesystem without Unix modes the chmod is a no-op and every member is 0777. It still runs,
# so it is a note, not a failure.
STAGED_MODE=$(stat -c%a "$GAMEDIR/miyoo-config.json")
if [ "$STAGED_MODE" != "644" ]; then
    echo "NOTE: this filesystem reports mode $STAGED_MODE where 644 was set, so it carries no Unix"
    echo "      modes and every zip member will be 0777. Fine for a test build; build a RELEASE on a"
    echo "      native Linux filesystem."
fi
rm -f "$OUT/pockettracker-miyoo.zip"
( cd "$STAGE" && zip -r "$OUT/pockettracker-miyoo.zip" Roms -x '.*' )
echo
ls -lh "$OUT/pockettracker-miyoo.zip"
unzip -l "$OUT/pockettracker-miyoo.zip"

# ⚠️ Out of the zip, not the staging folder: the zip is what ships.
echo
echo "read back out of the zip:"
G=Roms/PORTS/Games/PocketTracker
MEMBERS=( Roms/PORTS/Shortcuts/PocketTracker.port
          Roms/PORTS/Imgs/PocketTracker.png
          $G/pockettracker
          $G/launch.sh
          $G/miyoo-config.json
          $G/README.md
          $G/libs/libSDL2-2.0.so.0
          $G/libs/libneonarmmiyoo.so
          $G/licenses/LICENSE
          $G/licenses/THIRD-PARTY-NOTICES.md
          $G/licenses/CREDITS.md
          $G/licenses/libogg-COPYING
          $G/licenses/libopus-COPYING
          $G/licenses/libopus-LICENSE_PLEASE_READ.txt
          $G/licenses/OFL-1.1-LiberationSans.txt
          $G/licenses/OFL-1.1-QuinqueFive.txt
          $G/licenses/libSDL2-zlib-LICENSE.txt
          $G/licenses/libSDL2-miyoo-fork-GPL-3.0.txt )
# An array, with the demo's members appended from the staging tree: their names carry spaces, and
# nothing else reads the demo back out of the zip.
if [ -d "$GAMEDIR/demo" ]; then
    while IFS= read -r M; do MEMBERS+=("$M"); done \
        < <(cd "$STAGE" && find "$G/demo" -type f | LC_ALL=C sort)
fi
for M in "${MEMBERS[@]}"; do
    # ⚠️ `|| BYTES=0`: unzip exits 11 on a miss, and pipefail would end the script before the FAIL line.
    BYTES=$(unzip -p "$OUT/pockettracker-miyoo.zip" "$M" | wc -c) || BYTES=0
    printf '  %9s  %s\n' "$BYTES" "$M"
    if [ "$BYTES" -lt 60 ]; then
        echo "FAIL: $M is missing or empty inside the zip."
        exit 1
    fi
done

echo
echo "OK  $OUT/pockettracker-miyoo.zip"
echo "    Extract at the SD card root. It lands in Roms/PORTS/ and appears in Onion's ports list,"
echo "    where play time, launch count and the Game Switcher are recorded. Needs Onion's Ports"
echo "    Collection package installed."
