#!/bin/bash
#
# Package the macOS app: build/macos/PocketTracker-<version>-macos.zip
#
# Run from the repo root on a Mac, after building the shell as a universal (Apple Silicon + Intel) app:
#
#     cmake -S shell -B shell/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
#     cmake --build shell/build --config Release -j
#     bash shell/build-macos.sh
#
# Packages, does not build — like build-windows.ps1. It checks the finished bundle, signs it ad hoc
# (no Apple account: Gatekeeper still asks once, see README), zips it and reads the zip back.
set -euo pipefail

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=$SRC/shell/build
OUT=$SRC/build/macos
APP=$BUILD/PocketTracker.app
BIN=$APP/Contents/MacOS/PocketTracker

cd "$SRC"

VERSION=$(sed -n 's/.*versionName[ \t]*=[ \t]*"\([0-9]*\.[0-9]*\.[0-9]*\(\.[0-9]*\)\{0,1\}\)".*/\1/p' app/build.gradle.kts | head -1)
if [ -z "$VERSION" ]; then
    echo "FAIL: could not read versionName from app/build.gradle.kts"
    exit 1
fi
echo "version = $VERSION"

echo
echo "############ 1/4  check the ARTIFACT ############"
if [ ! -x "$BIN" ]; then
    echo "FAIL: $BIN was not produced - build the shell first (see the top of this file)"
    exit 1
fi

# Older than any source = the zip would ship code that is not this tree's.
STALE=$(find "$SRC/native" "$SRC/shell" \( -name '*.cpp' -o -name '*.h' -o -name '*.c' -o -name 'CMakeLists.txt' \) \
             -not -path '*/vendor/*' -not -path '*/build*/*' -newer "$BIN" | head -1)
if [ -n "$STALE" ]; then
    echo "FAIL: $BIN is OLDER than $STALE - rebuild before packaging"
    exit 1
fi
echo "binary is newer than every source file"

# Both architectures, or half of all Macs get "this app cannot be opened".
ARCHS=$(lipo -archs "$BIN")
echo "architectures: $ARCHS"
for A in arm64 x86_64; do
    if ! echo " $ARCHS " | grep -q " $A "; then
        echo "FAIL: the binary has no $A slice - configure with CMAKE_OSX_ARCHITECTURES=\"arm64;x86_64\""
        exit 1
    fi
done

# Only system libraries: anything else (a Homebrew SDL2) exists on the build machine alone.
echo
echo "libraries it loads:"
otool -L "$BIN"
# Library lines are the indented ones; a universal binary prints a header per architecture.
FOREIGN=$(otool -L "$BIN" | grep -E '^[[:space:]]' | awk '{print $1}' | grep -v -E '^(/System/Library/|/usr/lib/)' || true)
if [ -n "$FOREIGN" ]; then
    echo "FAIL: the binary loads libraries a stranger's Mac does not have:"
    echo "$FOREIGN"
    exit 1
fi

PLIST_VERSION=$(plutil -extract CFBundleShortVersionString raw "$APP/Contents/Info.plist")
echo
echo "Info.plist version: $PLIST_VERSION"
if [ "$PLIST_VERSION" != "$VERSION" ]; then
    echo "FAIL: Info.plist says $PLIST_VERSION, app/build.gradle.kts says $VERSION"
    exit 1
fi
if [ ! -s "$APP/Contents/Resources/PocketTracker.icns" ]; then
    echo "FAIL: the bundle has no icon"
    exit 1
fi

SDL_LICENSE=$BUILD/_deps/sdl2-src/LICENSE.txt
if [ ! -s "$SDL_LICENSE" ]; then
    echo "FAIL: no $SDL_LICENSE - SDL2 is linked into the app, so its notice must ship with it"
    exit 1
fi

echo
echo "############ 2/4  sign (ad hoc) ############"
# ⚠️ An Apple Silicon Mac refuses to run an unsigned bundle at all ("is damaged"). Ad hoc is enough to
# run; it is not notarisation, so Gatekeeper still asks once.
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

echo
echo "############ 3/4  stage ############"
STAGE=$OUT/stage/PocketTracker-$VERSION
rm -rf "$OUT"
mkdir -p "$STAGE/licenses"
# ditto, not cp: it keeps the bundle's symlinks, permissions and signature intact.
ditto "$APP" "$STAGE/PocketTracker.app"
cp "$SRC/shell/macos/README.txt"                     "$STAGE/README.txt"
cp "$SRC/LICENSE"                                    "$STAGE/licenses/LICENSE"
cp "$SRC/docs/licenses/THIRD-PARTY-NOTICES.md"       "$STAGE/licenses/THIRD-PARTY-NOTICES.md"
cp "$SRC/CREDITS.md"                                 "$STAGE/licenses/CREDITS.md"
cp "$SRC/docs/licenses/OFL-1.1-LinuxBiolinum.txt"    "$STAGE/licenses/OFL-1.1-LinuxBiolinum.txt"
cp "$SRC/native/vendor/ogg/COPYING"                  "$STAGE/licenses/libogg-COPYING"
cp "$SRC/native/vendor/opus/COPYING"                 "$STAGE/licenses/libopus-COPYING"
cp "$SRC/native/vendor/opus/LICENSE_PLEASE_READ.txt" "$STAGE/licenses/libopus-LICENSE_PLEASE_READ.txt"
cp "$SDL_LICENSE"                                    "$STAGE/licenses/SDL2-LICENSE.txt"
ls -la "$STAGE" "$STAGE/licenses"

echo
echo "############ 4/4  zip, then read it BACK OUT ############"
ZIP=$OUT/PocketTracker-$VERSION-macos.zip
ditto -c -k --keepParent "$STAGE" "$ZIP"
ls -lh "$ZIP"

# The zip is what ships, so the checks run again on what comes out of it.
CHECK=$OUT/readback
mkdir -p "$CHECK"
ditto -x -k "$ZIP" "$CHECK"
ROOT=$CHECK/PocketTracker-$VERSION
for NEEDED in "PocketTracker.app/Contents/MacOS/PocketTracker" "PocketTracker.app/Contents/Info.plist" \
              "PocketTracker.app/Contents/Resources/PocketTracker.icns" "README.txt" \
              "licenses/LICENSE" "licenses/THIRD-PARTY-NOTICES.md" "licenses/CREDITS.md" \
              "licenses/OFL-1.1-LinuxBiolinum.txt" "licenses/SDL2-LICENSE.txt" \
              "licenses/libogg-COPYING" "licenses/libopus-COPYING" "licenses/libopus-LICENSE_PLEASE_READ.txt"; do
    BYTES=$(wc -c < "$ROOT/$NEEDED" 2>/dev/null || echo 0)
    echo "  $NEEDED: $BYTES bytes"
    if [ "$BYTES" -lt 100 ]; then
        echo "FAIL: $NEEDED is missing or empty inside the zip"
        exit 1
    fi
done
codesign --verify --deep --strict "$ROOT/PocketTracker.app"
echo "  signature intact after the round trip"

rm -rf "$OUT/stage" "$CHECK"
echo
echo "OK: $ZIP"
