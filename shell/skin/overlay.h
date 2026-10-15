// The CRT overlay: a full-frame PNG over the 640×480 display at an adjustable strength. The SETTINGS
// row edits an index; settings.json stores the stable `id`. ⚠️ `unload()` before `SdlVideo::close()`.

#ifndef POCKETTRACKER_OVERLAY_H
#define POCKETTRACKER_OVERLAY_H

#include <SDL.h>

#include <cstdint>
#include <string>

namespace ptshell {

// One per PNG under `assets/overlays/`. The row cycles OFF (0), then entry i at index i + 1.
struct ScreenOverlayDef {
    const char* id;           // persisted overlay_name and asset leaf
    const char* displayName;  // SETTINGS text: uppercase(id), eight characters
    const char* file;         // asset-seam path
};

// Adding an overlay is one line here.
inline constexpr ScreenOverlayDef kScreenOverlays[] = {
    {"crt_scanlines", "CRT_SCAN", "overlays/crt_scanlines.png"},
};
inline constexpr int kScreenOverlayCount =
    static_cast<int>(sizeof(kScreenOverlays) / sizeof(kScreenOverlays[0]));

inline constexpr int screen_overlay_choice_count() { return 1 + kScreenOverlayCount; }

// A persisted id → its cycle index; "OFF", unknown or mangled → 0.
inline int screen_overlay_index(const std::string& id) {
    for (int i = 0; i < kScreenOverlayCount; ++i)
        if (id == kScreenOverlays[i].id) return i + 1;
    return 0;  // OFF
}

// The id settings.json stores, so the choice survives the table being reordered.
inline std::string screen_overlay_id(int index) {
    if (index <= 0 || index > kScreenOverlayCount) return "OFF";
    return kScreenOverlays[index - 1].id;
}

inline std::string screen_overlay_text(int index) {
    if (index <= 0 || index > kScreenOverlayCount) return "OFF";
    return kScreenOverlays[index - 1].displayName;
}

class ScreenOverlay {
public:
    ScreenOverlay() = default;
    ~ScreenOverlay() { unload(); }

    ScreenOverlay(const ScreenOverlay&)            = delete;  // owns an SDL_Texture
    ScreenOverlay& operator=(const ScreenOverlay&) = delete;

    // Decode entry `index - 1` into a blended texture; 0 or out of range unloads. A bad PNG unloads
    // and `draw` does nothing. `log` prints one `overlay:` line. True when a texture is loaded.
    bool load(SDL_Renderer* renderer, int index, bool log);

    // Idempotent; before the renderer is destroyed.
    void unload();

    // Across `dst` at `strength` (0–255) alpha; no-op when unloaded or 0.
    void draw(SDL_Renderer* renderer, const SDL_Rect& dst, int strength) const;

    bool loaded() const { return tex_ != nullptr; }

private:
    SDL_Texture* tex_ = nullptr;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_OVERLAY_H
