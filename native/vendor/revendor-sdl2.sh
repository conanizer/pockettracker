#!/bin/bash
#
# Re-vendor SDL2 into native/vendor/SDL2 at a given release tag.
#
#   native/vendor/revendor-sdl2.sh release-2.30.9
#
# ─── Why this is a SCRIPT and not a paragraph in a README ──────────────────────────────────────
#
# The vendored copy is a PRUNED subset of upstream (80 MB whole, 21 MB of it actually reachable by
# the CMake build), and the pruning rule is the kind of thing that survives exactly one upgrade if it
# lives in prose. Whoever next bumps SDL would re-clone, copy "the source", and either drag 51 MB of
# test/ back in or — much worse — copy a subset that differs from this one in some way nobody
# notices until an unrelated backend fails to compile six months later.
#
# So the rule is executable. Bumping SDL is: run this with the new tag, look at the diff, commit.
#
# ⚠️ THE JAVA GLUE COMES FROM THE SAME CHECKOUT AS THE C, AND THAT IS THE ENTIRE POINT.
# SDL's Android support is half C and half Java, and the two halves are version-locked:
# SDLActivity.java hardcodes SDL_MAJOR/MINOR/MICRO_VERSION and compares them against the C library's
# nativeGetVersion() at startup, refusing to run on a mismatch. Mixing versions is a classic
# SDL-on-Android failure that presents as an inscrutable init crash. Copying both halves out of ONE
# clone in ONE script makes drift structurally impossible rather than a thing to remember — and
# native/CMakeLists.txt re-asserts it at BUILD time (search for SDL_VERSION_LOCK) so that a hand-edit
# of either half cannot get past a compile.
#
# ⚠️ THIS COPY IS ANDROID-ONLY, BY DESIGN, AND IS NOT THE SHELL'S SDL.
# shell/CMakeLists.txt takes the SYSTEM SDL2 when it can (every PortMaster CFW requires that — the
# device's own libSDL2 carries that hardware's display/audio patches) and FetchContent otherwise.
# The APK cannot do either: F-Droid builds offline from source with no prebuilt binaries, so Android
# is the one target that has to carry SDL in-tree. Two delivery models for two linkage models, which
# is why the pruning below can drop the desktop backends' build plumbing (wayland-protocols/,
# build-scripts/) without breaking anything that is actually built.
#
set -euo pipefail

TAG="${1:-}"
if [ -z "$TAG" ]; then
    echo "usage: $0 <sdl-release-tag>   e.g. $0 release-2.30.9" >&2
    exit 2
fi

VENDOR_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST="$VENDOR_DIR/SDL2"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ⚠️ core.autocrlf=false ON THE CLONE, and it is not a nicety. This repo sets core.autocrlf=true
# globally, so a plain clone here hands back every SDL source file with CRLF line endings — 2117 CRs
# injected into SDLActivity.java alone. Combined with the `* -text` .gitattributes written below
# (which stores bytes verbatim), that would commit a CRLF-ified SDL into the repo and ship it to
# every Linux and F-Droid checkout. It compiles, so nothing would ever complain.
#
# ⚠️⚠️ AND THE FIRST VERSION OF THIS SCRIPT COULD NOT SEE IT. Its check diffed the vendored copy
# against the clone it was made from — both converted by the same setting — so the comparison passed
# while "byte-identical to upstream" was false. A check that compares two things corrupted the same
# way is not a check. That is why the verification at the bottom now compares GIT BLOB HASHES
# against upstream's own object IDs, which no local EOL setting can influence.
echo "cloning SDL $TAG ..."
git -c core.autocrlf=false -c core.eol=lf \
    clone --depth 1 --branch "$TAG" https://github.com/libsdl-org/SDL.git "$WORK/src"

# Provenance, recorded from the clone rather than from the argument: if the tag ever moved, the
# commit is what says which bytes these actually are.
SDL_COMMIT="$(git -C "$WORK/src" rev-parse HEAD)"

rm -rf "$DEST"
mkdir -p "$DEST"

# ─── The subset: everything the CMake build reads, and nothing else ────────────────────────────
#
#   src/      the library itself, WHOLE. Not pruned per-platform on purpose: SDL's CMake decides
#             which backends compile from its own platform tests, and a hand-pruned src/ would turn
#             every future SDL feature flag into a missing-file error. 18 MB is the honest price.
#   include/  the public headers, plus SDL_config.h.cmake / SDL_revision.h.cmake which the build
#             configure_file()s.
#   cmake/    sdlchecks.cmake, macros.cmake, the platform probes.
#   *.in      the five templates CMakeLists.txt configure_file()s (sdl2.pc, sdl2-config, SDL2.spec,
#             SDL2Config.cmake, cmake_uninstall.cmake) — a missing one is a configure error even
#             though nothing installs from this tree.
#
# Dropped: test/ (51 MB), Xcode*/, VisualC*/, visualtest/, docs/, acinclude/, build-scripts/,
# wayland-protocols/, android-project-ant/, mingw/, and android-project/ except the Java glue below.
cp -a "$WORK/src/src"     "$DEST/src"
cp -a "$WORK/src/include" "$DEST/include"
cp -a "$WORK/src/cmake"   "$DEST/cmake"

# ONE list, used both to copy and to verify below. Two lists would be two things to keep in step,
# and the verifier's path mapping getting out of step with the copy is not hypothetical — a `*.in`
# glob here first time round claimed test/Makefile.in was missing from a tree that was never
# supposed to contain it.
#
# The first eight are the templates CMakeLists.txt configure_file()s — a missing one is a configure
# error even though nothing installs from this tree. The last four are licence and provenance:
# LICENSE.txt is the zlib text licenses/THIRD-PARTY-NOTICES.md points at, and the other three are
# how a reader finds out what this directory is without leaving the repo.
SDL_TOP_FILES="
CMakeLists.txt
SDL2Config.cmake.in
sdl2-config-version.cmake.in
sdl2-config.cmake.in
sdl2-config.in
sdl2.pc.in
SDL2.spec.in
cmake_uninstall.cmake.in
LICENSE.txt
README-SDL.txt
CREDITS.txt
WhatsNew.txt
"
for F in $SDL_TOP_FILES; do cp -a "$WORK/src/$F" "$DEST/$F"; done

# src/hidapi/testgui is a macOS test application — never referenced by any CMakeLists in the tree
# (checked), and it carries two of the subset's three binary files (an .icns and a .strings).
#
# ⚠️ It does NOT carry the third: src/main/winrt/SDL2-WinRTResource_BlankCursor.cur survives, and
# that is deliberate. Deleting it would be a per-platform prune of src/, which the note above
# forbids for good reason, to buy a cosmetic "this tree is 100% text" line. So the honest statement
# is the narrower one: the vendored tree contains exactly one binary file, a WinRT cursor resource,
# and no archives or executables of any kind — nothing F-Droid's scanner objects to. (An earlier
# version of this comment claimed testgui was the only binary in the subset. It was not.)
rm -rf "$DEST/src/hidapi/testgui"

# The Java half, from the same checkout as the C half — see the warning at the top of this file.
# Flattened out of upstream's android-project/app/src/main/java/ because the rest of that directory
# is a sample Gradle project (and carries a gradle-wrapper.jar, which is a binary and would be the
# only one left in the tree). app/build.gradle.kts adds THIS path to the Android sourceSet.
mkdir -p "$DEST/android/java/org/libsdl/app"
cp -a "$WORK/src/android-project/app/src/main/java/org/libsdl/app/." \
      "$DEST/android/java/org/libsdl/app/"

# ⚠️ Verbatim bytes, no EOL conversion. This repo runs core.autocrlf=true, which would rewrite every
# line ending on checkout and make "is the vendored copy still upstream's?" unanswerable on Windows.
# Same rule and same reason as native/songcore/.gitattributes and testdata/.gitattributes: a tree
# that is byte-compared must never be EOL-converted. The comparison this protects is at the bottom
# of this script, and it is the only thing standing between a corrupted vendor drop and a build that
# looks fine until it does not.
cat > "$DEST/.gitattributes" <<'ATTRS'
# Vendored upstream source: store and check out the bytes exactly as SDL published them, on every
# platform. This repo sets core.autocrlf=true, which would otherwise rewrite line endings on
# checkout and destroy the byte-for-byte comparison against upstream that revendor-sdl2.sh makes.
* -text
ATTRS

cat > "$DEST/PT-VENDORING.md" <<PROV
# SDL2 — vendored for the Android build

**Do not edit anything in this directory.** It is a pruned but otherwise **verbatim** copy of
upstream SDL2, produced by \`native/vendor/revendor-sdl2.sh\`. To change the version, run that
script with a new tag; to change what is copied, edit the script.

| | |
|---|---|
| upstream | https://github.com/libsdl-org/SDL |
| tag | \`$TAG\` |
| commit | \`$SDL_COMMIT\` |
| licence | zlib — \`LICENSE.txt\`, and \`licenses/THIRD-PARTY-NOTICES.md\` at the repo root |

## Why this exists at all

The SDL shell on desktop and on the handhelds does **not** use this copy: \`shell/CMakeLists.txt\`
links the *system* SDL2 where there is one (mandatory on PortMaster CFWs) and FetchContent's it
otherwise. Only the APK needs SDL in-tree, because F-Droid builds offline, from source, with no
prebuilt binaries — see convergence plan C1.

## What was dropped

\`test/\` (51 MB), \`Xcode*/\`, \`VisualC*/\`, \`visualtest/\`, \`docs/\`, \`acinclude/\`,
\`build-scripts/\`, \`wayland-protocols/\`, \`android-project-ant/\`, \`mingw/\`,
\`src/hidapi/testgui/\`, and all of \`android-project/\` except the Java glue, which is flattened
into \`android/java/\`. \`src/\` itself is **not** pruned per-platform: SDL's own CMake decides which
backends compile, and a hand-pruned \`src/\` turns every future SDL option into a missing-file error.

## The one rule

⚠️ **The C and the Java are version-locked.** \`android/java/org/libsdl/app/SDLActivity.java\`
hardcodes the SDL version and refuses to run against a \`libSDL2.so\` reporting a different one.
Both halves come out of one clone here, and \`native/CMakeLists.txt\` re-checks them against each
other at build time (\`SDL_VERSION_LOCK\`). Never update one by hand.
PROV

# ─── Prove the copy is upstream's, by GIT BLOB HASH ────────────────────────────────────────────
#
# Every file gets hashed with `git hash-object` and compared against the object ID upstream's own
# tree records for that path. That is the strongest available statement of "these are upstream's
# bytes": the blob ID is a hash of the content, computed identically everywhere, and no
# core.autocrlf, no core.eol, no platform and no editor can make a changed file hash the same.
#
# ⚠️ This replaces a `diff -r` against the clone, which LOOKED equivalent and was not: it compared
# the copy with the source it was copied from, so any corruption applied to BOTH — exactly what
# core.autocrlf does — passed silently. A negative control that cannot fail is not a control, and
# this one was measured failing to fail: the tree it certified had 2117 CRs injected into
# SDLActivity.java alone.
echo "verifying the vendored tree against upstream's own blob hashes ..."
git -C "$WORK/src" ls-tree -r HEAD > "$WORK/upstream-tree.txt"

# upstream path -> vendored path. Only the Java glue is relocated; everything else keeps its path.
vendored_path_for() {
    case "$1" in
        android-project/app/src/main/java/org/libsdl/app/*)
            echo "$DEST/android/java/org/libsdl/app/${1##*/}" ;;
        src/*|include/*|cmake/*)
            echo "$DEST/$1" ;;
        *)
            # A top-level file is vendored only if it is in the one list above — matched whole, not
            # by a glob. `*.in` would also swallow test/Makefile.in and visualtest/Makefile.in,
            # which are pruned, and report them as missing.
            for F in $SDL_TOP_FILES; do
                [ "$1" = "$F" ] && { echo "$DEST/$1"; return; }
            done
            echo "" ;;   # deliberately not vendored
    esac
}

CHECKED=0; DRIFT=0
while read -r _mode _type OID PATHNAME; do
    TARGET="$(vendored_path_for "$PATHNAME")"
    [ -n "$TARGET" ] || continue                      # pruned on purpose
    case "$PATHNAME" in src/hidapi/testgui/*) continue ;; esac
    if [ ! -f "$TARGET" ]; then
        echo "FAIL: missing from the vendored tree: $PATHNAME"; DRIFT=1; continue
    fi
    MINE="$(git hash-object "$TARGET")"
    if [ "$MINE" != "$OID" ]; then
        echo "FAIL: $PATHNAME does not match upstream (upstream $OID, vendored $MINE)"; DRIFT=1
    fi
    CHECKED=$((CHECKED + 1))
done < "$WORK/upstream-tree.txt"

if [ "$DRIFT" -ne 0 ]; then
    echo "the vendored tree is NOT upstream's bytes - refusing to leave it in place"; exit 1
fi
# ⚠️ A loop that checked nothing would also report no drift. Assert it did work.
if [ "$CHECKED" -lt 1000 ]; then
    echo "FAIL: only $CHECKED files were verified - the path mapping above is wrong, not the tree"; exit 1
fi
echo "  $CHECKED files match upstream's blob hashes exactly"

# The version triple, read out of BOTH halves, printed so the operator sees them agree. The build
# asserts this too (native/CMakeLists.txt, SDL_VERSION_LOCK); printing it here is what makes a bad
# re-vendor visible at the moment it happens instead of at the next compile.
C_VER="$(awk '/#define SDL_MAJOR_VERSION/{m=$3} /#define SDL_MINOR_VERSION/{n=$3} /#define SDL_PATCHLEVEL/{p=$3} END{print m"."n"."p}' "$DEST/include/SDL_version.h")"
J_VER="$(awk -F'= *' '/SDL_MAJOR_VERSION *=/{gsub(/;/,"",$2); m=$2} /SDL_MINOR_VERSION *=/{gsub(/;/,"",$2); n=$2} /SDL_MICRO_VERSION *=/{gsub(/;/,"",$2); p=$2} END{print m"."n"."p}' "$DEST/android/java/org/libsdl/app/SDLActivity.java")"

echo
echo "vendored SDL2 $TAG ($SDL_COMMIT)"
echo "  C   include/SDL_version.h                  -> $C_VER"
echo "  Java android/.../SDLActivity.java          -> $J_VER"
[ "$C_VER" = "$J_VER" ] || { echo "FAIL: C/Java version mismatch in a single upstream checkout - that should be impossible"; exit 1; }
echo "  tree verified byte-identical to upstream (minus the documented prunes)"
echo
echo "next: update the SDL2 section of licenses/THIRD-PARTY-NOTICES.md if the version changed,"
echo "      then rebuild the APK and check libSDL2.so is in it."
