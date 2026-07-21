// ─── shell/assets.h — the ASSET SEAM (convergence plan D7) ───────────────────────────────────────
//
// Skins, overlays and (later) the demo project ship as files the shell has to read at run time, and
// WHERE they live is the one thing that genuinely differs between the two worlds this shell runs in:
//
//   * Android — they are packed inside the APK's `assets/`, which `std::filesystem` cannot see at all
//     (there is no path on disk; they are entries in a zip the loader mmaps). SDL's own `SDL_RWFromFile`
//     is the only thing here that reads them: on Android a path with no leading slash is looked up in
//     the APK's AAssetManager by SDL's Android RWops, so the relative path is used verbatim.
//   * Desktop / handheld — they sit beside the executable on a real filesystem. There `SDL_RWFromFile`
//     is `fopen`, which resolves against the process's CWD (whatever a launcher last cd'd to), NOT
//     where the resources are — so the path is anchored to `SDL_GetBasePath()`, the exe's directory.
//
// That fork — and it is the WHOLE fork — lives behind this one function, which is exactly the point of
// naming the seam: `image.{h,cpp}` decode bytes and know nothing of where they came from, `skin.{h,cpp}`
// composite textures and know nothing either, and only this file knows which world it is in. The
// header note in `image.h` already anticipated this: `decode_png(bytes)` is fed from here.

#ifndef POCKETTRACKER_ASSETS_H
#define POCKETTRACKER_ASSETS_H

#include <cstdint>
#include <string>
#include <vector>

namespace ptshell {

// Read a shipped asset by its RELATIVE path (e.g. "themes/amiga-2/bg_top_panel.png"), forward slashes,
// no leading slash. Returns the file's bytes, or an EMPTY vector on any failure — missing, unreadable,
// empty. Never throws. A skin is chrome, not correctness, so the caller treats "empty" as "skip this
// piece" rather than a fatal error (see skin.cpp).
std::vector<std::uint8_t> read_asset(const std::string& rel);

}  // namespace ptshell

#endif  // POCKETTRACKER_ASSETS_H
