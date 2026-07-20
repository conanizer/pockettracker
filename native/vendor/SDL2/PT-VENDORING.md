# SDL2 — vendored for the Android build

**Do not edit anything in this directory.** It is a pruned but otherwise **verbatim** copy of
upstream SDL2, produced by `native/vendor/revendor-sdl2.sh`. To change the version, run that
script with a new tag; to change what is copied, edit the script.

| | |
|---|---|
| upstream | https://github.com/libsdl-org/SDL |
| tag | `release-2.30.9` |
| commit | `c98c4fbff6d8f3016a3ce6685bf8f43433c3efcc` |
| licence | zlib — `LICENSE.txt`, and `licenses/THIRD-PARTY-NOTICES.md` at the repo root |

## Why this exists at all

The SDL shell on desktop and on the handhelds does **not** use this copy: `shell/CMakeLists.txt`
links the *system* SDL2 where there is one (mandatory on PortMaster CFWs) and FetchContent's it
otherwise. Only the APK needs SDL in-tree, because F-Droid builds offline, from source, with no
prebuilt binaries — see convergence plan C1.

## What was dropped

`test/` (51 MB), `Xcode*/`, `VisualC*/`, `visualtest/`, `docs/`, `acinclude/`,
`build-scripts/`, `wayland-protocols/`, `android-project-ant/`, `mingw/`,
`src/hidapi/testgui/`, and all of `android-project/` except the Java glue, which is flattened
into `android/java/`. `src/` itself is **not** pruned per-platform: SDL's own CMake decides which
backends compile, and a hand-pruned `src/` turns every future SDL option into a missing-file error.

## The one rule

⚠️ **The C and the Java are version-locked.** `android/java/org/libsdl/app/SDLActivity.java`
hardcodes the SDL version and refuses to run against a `libSDL2.so` reporting a different one.
Both halves come out of one clone here, and `native/CMakeLists.txt` re-checks them against each
other at build time (`SDL_VERSION_LOCK`). Never update one by hand.
