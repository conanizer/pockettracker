#!/bin/bash
#
# Build the Linux desktop package: build/linux/PocketTracker-<version>-linux-x64.tar.gz
#
# Run from the repo root, on a Linux box (or WSL) with a C++17 toolchain and libsdl2-dev:
#
#     bash shell/build-linux.sh
#
# ⚠️ A package for other people comes out of shell/Dockerfile.linux (the release workflow does);
# a host build runs only on distros at least as new as the host.
#
# The handheld package is shell/build-portmaster.sh: it links the device's patched SDL2, this one the
# distribution's.
#
# ⚠️ Nothing is filtered: a step that fails quietly leaves a stale artifact for the next to "verify".
set -euo pipefail

SRC=${SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
BUILD=$SRC/shell/build-release-linux
OUT=$SRC/build/linux

cd "$SRC"

# The APK's version, read from the one place that holds it.
VERSION=$(sed -n 's/.*versionName[ \t]*=[ \t]*"\([0-9]*\.[0-9]*\.[0-9]*\(\.[0-9]*\)\{0,1\}\)".*/\1/p' app/build.gradle.kts | head -1)
if [ -z "$VERSION" ]; then
    echo "FAIL: could not read versionName from app/build.gradle.kts"
    exit 1
fi
echo "version = $VERSION"

STAGE=$OUT/stage/PocketTracker-$VERSION
rm -rf "$OUT"
mkdir -p "$STAGE"

echo
echo "############ 1/4  build ############"
# Release stated: an empty build type is not Release. `tee`, so the output shows in full and the log
# is still there for the version check below.
CONFIGURE_LOG=$BUILD/configure.log
mkdir -p "$BUILD"
cmake -S shell -B "$BUILD" -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "$CONFIGURE_LOG"
cmake --build "$BUILD" -j"$(nproc)"

BIN=$BUILD/pockettracker-sdl
if [ ! -x "$BIN" ]; then
    echo "FAIL: $BIN was not produced"
    exit 1
fi

echo
echo "############ 2/4  check the ARTIFACT ############"
# ⚠️ The binary is the discriminator, not the exit code: a green build can sit over a stale executable.

# (a) Newer than every source file, or the package ships code that is not this tree's.
#
# ⚠️ `-printf` and `sort`, not `ls -t | head`: `head` closing the pipe kills `ls`, and `set -e` then
# ends the script here in silence.
NEWEST_SRC=$(find "$SRC/native" "$SRC/shell" \
                  \( -name '*.cpp' -o -name '*.h' -o -name '*.c' \) \
                  -not -path '*/vendor/*' -not -path '*/build*/*' \
                  -printf '%T@ %p\n' 2>/dev/null | sort -rn | sed -n '1s/^[^ ]* //p') || true
if [ -z "$NEWEST_SRC" ]; then
    echo "FAIL: found no source files to compare against — the staleness check would pass vacuously"
    exit 1
fi
if [ "$NEWEST_SRC" -nt "$BIN" ]; then
    echo "FAIL: $BIN is OLDER than $NEWEST_SRC — rebuild before packaging"
    exit 1
fi
echo "binary is newer than the newest source file ($NEWEST_SRC)"

# (b) A 64-bit x86 ELF, linked against SDL2 as the README promises.
file "$BIN"
if ! file "$BIN" | grep -q 'ELF 64-bit.*x86-64'; then
    echo "FAIL: $BIN is not an x86-64 ELF"
    exit 1
fi
echo
echo "shared libraries it needs:"
ldd "$BIN"
if ! ldd "$BIN" | grep -q 'libSDL2'; then
    echo "FAIL: the binary does not link libSDL2 — the README's dependency line would be wrong"
    exit 1
fi

# (c) CMake and this script read the same version: CMake's own parse announces it at configure.
CMAKE_VERSION_LINE=$(grep -F 'PocketTracker' "$CONFIGURE_LOG" | grep -F 'from app/build.gradle.kts' | head -1)
if [ -z "$CMAKE_VERSION_LINE" ]; then
    echo "FAIL: shell/CMakeLists.txt did not announce a version — its parser or its message changed,"
    echo "      and this check would otherwise pass by finding nothing."
    exit 1
fi
echo "cmake said: $CMAKE_VERSION_LINE"
if ! echo "$CMAKE_VERSION_LINE" | grep -qF "PocketTracker $VERSION "; then
    echo "FAIL: cmake read a different version than this script did (script: $VERSION)"
    exit 1
fi

echo
echo "############ 3/4  stage ############"
# One top-level folder, so the tarball does not scatter files into a home directory.
install -m 755 "$BIN" "$STAGE/PocketTracker"
# The application-menu entry: the script writes a .desktop file pointing at wherever this folder is.
install -m 755 "$SRC/shell/linux/install-desktop.sh" "$STAGE/install-desktop.sh"
cp "$SRC/docs/images/logo-app.png"                 "$STAGE/pockettracker.png"

mkdir -p "$STAGE/licenses"
cp "$SRC/LICENSE"                                  "$STAGE/licenses/LICENSE"
cp "$SRC/docs/licenses/THIRD-PARTY-NOTICES.md"     "$STAGE/licenses/THIRD-PARTY-NOTICES.md"
cp "$SRC/CREDITS.md"                               "$STAGE/licenses/CREDITS.md"

# libogg and libopus are BSD-3-Clause: their notices travel with the binary. Taken from the vendored
# source that was compiled, as build-windows.ps1 and build-portmaster.sh do.
cp "$SRC/native/vendor/ogg/COPYING"                     "$STAGE/licenses/libogg-COPYING"
cp "$SRC/native/vendor/opus/COPYING"                    "$STAGE/licenses/libopus-COPYING"
cp "$SRC/native/vendor/opus/LICENSE_PLEASE_READ.txt"    "$STAGE/licenses/libopus-LICENSE_PLEASE_READ.txt"

# The OFL texts ship though no font file does: THIRD-PARTY-NOTICES.md cites them by path, and the 5×5
# UI font compiled into the binary is adapted from QuinqueFive.
cp "$SRC/docs/licenses/OFL-1.1-LiberationSans.txt"       "$STAGE/licenses/OFL-1.1-LiberationSans.txt"
cp "$SRC/docs/licenses/OFL-1.1-QuinqueFive.txt"          "$STAGE/licenses/OFL-1.1-QuinqueFive.txt"

cat > "$STAGE/README.txt" <<EOF
PocketTracker $VERSION — Linux (x86-64)

Run:
    ./PocketTracker

Add it to your application menu and launcher, with its icon (again after moving this folder):
    ./install-desktop.sh

Requires SDL2 at runtime (most distributions call the package libsdl2-2.0-0 or sdl2):
    Debian / Ubuntu   sudo apt install libsdl2-2.0-0
    Fedora            sudo dnf install SDL2
    Arch              sudo pacman -S sdl2

Your files live in \$XDG_DATA_HOME/PocketTracker (usually ~/.local/share/PocketTracker):

    Projects/     songs (.ptp)
    Samples/      WAV, MP3, FLAC, OGG, Opus, M4A
    Soundfonts/   .sf2

Set POCKETTRACKER_HOME to put them somewhere else.

Controls, the manual and the full feature list:
    https://github.com/conanizer/pockettracker

Licensed under the GNU General Public License v3.0 or later — see licenses/LICENSE.
Third-party components and their notices are in licenses/THIRD-PARTY-NOTICES.md.
EOF

ls -la "$STAGE"

echo
echo "############ 4/4  tar, then read it BACK OUT ############"
TARBALL=$OUT/PocketTracker-$VERSION-linux-x64.tar.gz
( cd "$OUT/stage" && tar czf "$TARBALL" "PocketTracker-$VERSION" )
ls -lh "$TARBALL"
tar tzf "$TARBALL"

# ⚠️ The checks above read the staging folder; the tarball is what ships. Read back out of it, sizes
# included — an empty member extracts cleanly too.
echo
echo "read back out of the tarball:"
for NEEDED in "PocketTracker" "README.txt" "install-desktop.sh" "pockettracker.png" "licenses/LICENSE" "licenses/THIRD-PARTY-NOTICES.md" \
              "licenses/CREDITS.md" "licenses/OFL-1.1-LiberationSans.txt" "licenses/OFL-1.1-QuinqueFive.txt" \
              "licenses/libogg-COPYING" "licenses/libopus-COPYING" \
              "licenses/libopus-LICENSE_PLEASE_READ.txt"; do
    # ⚠️ `|| BYTES=0`: a missing member makes tar exit 2, and pipefail would end the script before the
    # FAIL line names it.
    BYTES=$(tar xzOf "$TARBALL" "PocketTracker-$VERSION/$NEEDED" 2>/dev/null | wc -c) || BYTES=0
    echo "  $NEEDED: $BYTES bytes"
    if [ "$BYTES" -lt 100 ]; then
        echo "FAIL: PocketTracker-$VERSION/$NEEDED is missing or empty inside the tarball."
        exit 1
    fi
done

rm -rf "$OUT/stage"
echo
echo "OK: $TARBALL"
