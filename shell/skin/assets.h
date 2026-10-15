// The asset seam: shipped files (skins, overlays) live inside the APK on Android — no path on disk;
// SDL's RWops looks a relative path up in the AAssetManager — and beside the executable elsewhere,
// where the path is anchored to `SDL_GetBasePath()` rather than the launcher's working directory.

#ifndef POCKETTRACKER_ASSETS_H
#define POCKETTRACKER_ASSETS_H

#include <cstdint>
#include <string>
#include <vector>

namespace ptshell {

// A shipped asset by relative path ("themes/amiga-2/bg_top_panel.png"). Empty on any failure; never
// throws — the caller skips that piece.
std::vector<std::uint8_t> read_asset(const std::string& rel);

}  // namespace ptshell

#endif  // POCKETTRACKER_ASSETS_H
