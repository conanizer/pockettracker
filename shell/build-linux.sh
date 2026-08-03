#!/bin/bash
#
# Build the Linux desktop package: build/linux/PocketTracker-<version>-linux-x64.tar.gz
#
# Run from the repo root, on a Linux box (or WSL) with a C++17 toolchain and libsdl2-dev:
#
#     bash shell/build-linux.sh
#
# This is the DESKTOP x86-64 package. The handheld aarch64 one is shell/build-portmaster.sh, and the
# two are deliberately different builds rather than one script with a flag — they disagree about the
# thing that matters most, which is SDL2. The PortMaster package links against the DEVICE's libSDL2
# (its CFW patched that copy for the hardware's display and audio); a desktop links whatever the
# distro ships. Neither can stand in for the other.
#
# ⚠️ Nothing here is filtered. Every command prints its own output — a build step that fails quietly
# and leaves a stale artifact behind for the next step to "verify" is the failure this project has
# been bitten by repeatedly.
set -euo pipefail

SRC=${SRC:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}
BUILD=$SRC/shell/build-release-linux
OUT=$SRC/build/linux

cd "$SRC"

# The version is the APK's, read from the one place that holds it. A package whose name and a
# CHANGELOG disagree is a support question nobody can answer, so it is read rather than passed in.
VERSION=$(sed -n 's/.*versionName[ \t]*=[ \t]*"\([0-9]*\.[0-9]*\.[0-9]*\)".*/\1/p' app/build.gradle.kts | head -1)
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
# CMAKE_BUILD_TYPE explicitly: shell/CMakeLists.txt warns that a Debug engine may not keep up with a
# real-time audio callback, and an empty build type is not the same as Release.
#
# `tee` rather than a redirect, so the configure output still appears here in full — the log is read
# below for the version line and nothing about the build is hidden to get it.
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
# ⚠️ THE BINARY IS THE DISCRIMINATOR, NOT THE EXIT CODE. A green build over a stale executable is the
# recurring failure here, so the checks below read the file that is about to be shipped.

# (a) Newer than every source file, or the package ships code that is not this tree's.
#
# ⚠️ `-printf` and `sort`, not `xargs ls -t | head`: `head` closes the pipe, `ls` dies of SIGPIPE, and
# under `set -e` the whole script exits at this line having printed nothing. A staleness check that
# aborts the run is not a staleness check.
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

# (b) It is a 64-bit x86 ELF and it is dynamically linked against SDL2 — which is what the README's
#     dependency line promises, so it is asserted rather than assumed.
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

# (c) CMake and this script must have read the SAME version out of app/build.gradle.kts.
#
# ⚠️ THIS IS NOT "ASK THE BINARY WHAT VERSION IT IS", AND THE DIFFERENCE MATTERS. The desktop ELF
# carries no version string — only the Windows build compiles PT_VERSION into a resource — so a
# `--version` check here would find nothing, report nothing and pass, which is a check that cannot
# fail. What CAN be verified is the coupling that exists: shell/CMakeLists.txt parses the version out
# of app/build.gradle.kts itself and announces it, so the two parsers are compared against each other.
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
# ONE top-level folder: a tarball that unpacks a dozen loose files into someone's home directory is a
# tarball they will lose. The binary is renamed on the way in — the build target is a developer name.
install -m 755 "$BIN" "$STAGE/PocketTracker"

mkdir -p "$STAGE/licenses"
cp "$SRC/LICENSE"                                  "$STAGE/licenses/LICENSE"
cp "$SRC/licenses/THIRD-PARTY-NOTICES.md"          "$STAGE/licenses/THIRD-PARTY-NOTICES.md"
cp "$SRC/CREDITS.md"                               "$STAGE/licenses/CREDITS.md"

cat > "$STAGE/README.txt" <<EOF
PocketTracker $VERSION — Linux (x86-64)

Run:
    ./PocketTracker

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

# ⚠️ EVERYTHING ABOVE INSPECTED THE STAGING DIRECTORY, AND THE TARBALL IS WHAT SHIPS. A broken
# archive step would leave every check above green and the artifact empty, so the contents are read
# back out of the finished file — sizes included, because a zero-byte member also extracts cleanly.
echo
echo "read back out of the tarball:"
for NEEDED in "PocketTracker" "README.txt" "licenses/LICENSE" "licenses/THIRD-PARTY-NOTICES.md" "licenses/CREDITS.md"; do
    BYTES=$(tar xzOf "$TARBALL" "PocketTracker-$VERSION/$NEEDED" 2>/dev/null | wc -c)
    echo "  $NEEDED: $BYTES bytes"
    if [ "$BYTES" -lt 100 ]; then
        echo "FAIL: PocketTracker-$VERSION/$NEEDED is missing or empty inside the tarball."
        exit 1
    fi
done

rm -rf "$OUT/stage"
echo
echo "OK: $TARBALL"
