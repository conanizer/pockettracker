# Vendored: stb_image

- **File:** `stb_image.h` — a single-header PNG/JPEG/… decoder by Sean Barrett (nothings/stb).
- **Version:** v2.30 (the version string at the top of the header).
- **Source:** https://github.com/nothings/stb — `stb_image.h`
- **Pinned commit:** `31c1ad37456438565541f4919958214b6e762fb4` (fetched 2026-07-21).
- **Licence:** dual — public domain (Unlicense) **or** MIT, at your option. The full statement is
  at the end of the header. Notice recorded in `licenses/THIRD-PARTY-NOTICES.md`.

## Why it is here, and where it may be used

Vendored for **convergence plan D2** — the touch skin, CRT overlays and theme PNGs are decoded to
textures, and there was no PNG *reader* in the C++ tree (`tools/ptshot` has only a hand-rolled PNG
*writer*).

⚠️ **It lives under `native/vendor/` for ONE reason: the licence guard.** `shell/build-portmaster.sh`
derives its notice checklist from `native/vendor/*/`, so a decoder vendored anywhere else would be
invisible to it. Physical location is not permission to include: **this header is SHELL-ONLY.**

- The one translation unit that `#define STB_IMAGE_IMPLEMENTATION`s it is `shell/image.cpp`.
- Nothing in `native/` (the engine) or `native/ui/` (pt-ui) includes it. The canvas keeps its four
  primitives; the skin is composited shell-side (D1). pt-ui stays image-free **by construction**:
  `ptshot`/`ptinput`/`ptmapper`/`ptdispatch` link `pt-ui` *without* `shell/image.cpp`, so any
  stb_image reference leaking into a screen module breaks *those* tools' link.

## Verbatim — do not edit

The header is committed byte-for-byte as fetched. To update: re-fetch `stb_image.h` at a newer
commit, replace this file's version + pinned commit above, and re-run `ctest -R d-image-decode`.
Never hand-edit the header (the same rule the SDL2 and codec vendors carry).

## Build wiring

- Desktop shell: `shell/CMakeLists.txt` compiles `image.cpp` into `pockettracker-sdl`.
- Android shell: `native/CMakeLists.txt`'s `if(ANDROID)` block compiles `shell/image.cpp` into the
  `pockettracker-sdl` `.so`.
- The `native/` include root (PUBLIC on the `pockettracker` target) is what makes
  `#include "vendor/stb_image/stb_image.h"` resolve from a shell TU — no include line is restated.
- Decode is proven by `tools/ptdecode` (ctest `d-image-decode`), which compiles `shell/image.cpp`
  and links nothing else — the standing proof the decoder needs only stb_image + the standard library.
