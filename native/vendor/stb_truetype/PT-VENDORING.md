# Vendored: stb_truetype

- **File:** `stb_truetype.h` — a single-header TrueType/OpenType font rasterizer by Sean Barrett
  (nothings/stb).
- **Version:** v1.26 (the version string in the header's VERSION HISTORY).
- **Source:** https://github.com/nothings/stb — `stb_truetype.h`
- **Pinned commit:** `6e9f34d5429cf16790ec43c9bac3f1ee4ad1f760` (fetched 2026-07-22).
- **Licence:** dual — public domain (Unlicense) **or** MIT, at your option. The full statement is at
  the end of the header. Notice recorded in `licenses/THIRD-PARTY-NOTICES.md`, the same slot stb_image
  uses.

## Why it is here, and where it may be used

Vendored for **convergence plan D** — the PORTRAIT2 device skin draws its button labels in the app's
Helvetica (`helvetica_regular.otf`), and there was no font rasterizer in the C++ tree. Kotlin got the
font from Compose's `Text`; the shell has to rasterize glyphs itself.

⚠️ **It lives under `native/vendor/` for ONE reason: the licence guard.** `shell/build-portmaster.sh`
derives its notice checklist from `native/vendor/*/`, so a rasterizer vendored anywhere else would be
invisible to it. Physical location is not permission to include: **this header is SHELL-ONLY**, exactly
like stb_image.

- The one translation unit that `#define STB_TRUETYPE_IMPLEMENTATION`s it is `shell/font_raster.cpp`.
- Nothing in `native/` (the engine) or `native/ui/` (pt-ui) includes it. Text is drawn shell-side onto
  the device skin, never in the canvas — pt-ui keeps its four primitives. pt-ui stays font-free **by
  construction**: `ptshot`/`ptinput`/`ptmapper`/`ptdispatch` link `pt-ui` *without* `font_raster.cpp`,
  so any stb_truetype reference leaking into a screen module breaks *those* tools' link.

## ⚠️ CFF / OpenType — the specific risk this dependency carries

`helvetica_regular.otf` is **CFF/PostScript-outline OpenType** (`OTTO`), not the commoner TrueType
(`glyf`) flavour. stb_truetype supports CFF, but that path is the half most likely to regress silently
on a version bump — and a regression is invisible on device (a blank or tofu label reads as a layout
bug). `tools/ptfont` (ctest `d-font-raster`) asserts, against the exact shipped `.otf`, that it really
is `OTTO` and that every letter the labels use produces ink. Re-run it after any bump.

- ⚠️ **Helvetica has no arrow glyphs.** U+2190–2193 return `.notdef` (a tofu box). On Android those
  D-pad arrows came from SYSTEM FONT FALLBACK, which stb_truetype (one font, no fallback) has not — so
  the shell draws the D-pad arrows itself as smooth triangles (`shell/font.cpp` `draw_arrow`). ptfont
  documents this and would flag a future font that shipped real arrows.

## Verbatim — do not edit

The header is committed byte-for-byte as fetched. To update: re-fetch `stb_truetype.h` at a newer
commit, replace this file's version + pinned commit above, and re-run `ctest -R d-font-raster`. Never
hand-edit the header (the same rule the SDL2, stb_image and codec vendors carry).

## Build wiring

- Desktop shell: `shell/CMakeLists.txt` compiles `font_raster.cpp` (and `font.cpp`) into
  `pockettracker-sdl`.
- Android shell: `native/CMakeLists.txt`'s `if(ANDROID)` block compiles both into the
  `pockettracker-sdl` `.so`.
- The `native/` include root (PUBLIC on the `pockettracker` target) is what makes
  `#include "vendor/stb_truetype/stb_truetype.h"` resolve from a shell TU — no include line is restated.
- Rasterization is proven by `tools/ptfont` (ctest `d-font-raster`), which compiles `font_raster.cpp`
  and links nothing else — the standing proof the rasterizer needs only stb_truetype + the standard
  library, and that the CFF font still produces ink.
