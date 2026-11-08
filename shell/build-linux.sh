#!/bin/bash
#
# Build the Linux desktop package: build/linux/PocketTracker-<version>-linux-x64.tar.gz
#
# Run from the repo root, on a Linux box (or WSL) with a C++17 toolchain and libsdl2-dev:
#
#     bash shell/build-linux.sh
#
# ⚠️ A package for other people comes out of shell/Dockerfile, `--target linux` (the release workflow does);
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
. "$SRC/shell/package-licenses.sh"

rm -rf "$OUT"

echo
echo "############ 1/4  build ############"
# Release stated: an empty build type is not Release.
cmake -S shell -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(nproc)"

BIN=$BUILD/pockettracker-sdl
if [ ! -x "$BIN" ]; then
    echo "FAIL: $BIN was not produced"
    exit 1
fi

# The version this configure read (native/cmake/pt_version.cmake), so the name cannot differ from it.
VERSION=$(cat "$BUILD/version.txt")
if [ -z "$VERSION" ]; then
    echo "FAIL: $BUILD/version.txt is empty — the configure did not write the version"
    exit 1
fi
echo "version = $VERSION"
STAGE=$OUT/stage/PocketTracker-$VERSION
mkdir -p "$STAGE"

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

echo
echo "############ 3/4  stage ############"
# One top-level folder, so the tarball does not scatter files into a home directory.
install -m 755 "$BIN" "$STAGE/PocketTracker"
# The application-menu entry: the script writes a .desktop file pointing at wherever this folder is.
install -m 755 "$SRC/shell/linux/install-desktop.sh" "$STAGE/install-desktop.sh"
cp "$SRC/docs/images/logo-app.png"                 "$STAGE/pockettracker.png"

mkdir -p "$STAGE/licenses"
copy_licences "$STAGE/licenses"
check_notices "$STAGE/licenses/THIRD-PARTY-NOTICES.md"

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
for NEEDED in PocketTracker README.txt install-desktop.sh pockettracker.png $(licence_names | sed 's:^:licenses/:'); do
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
