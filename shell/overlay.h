// ─── shell/overlay.{h,cpp} — the SCREEN-OVERLAY texture ──────────────────────────────────────────
//
// The CRT-scanline filter drawn OVER the tracker screen: a full-frame PNG composited on top of the
// 640×480 display at an adjustable strength. Chrome, like the touch skin (skin.h) — never in the
// canvas.
//
// One texture, picked from a fixed shipped table (`kScreenOverlays`). The SETTINGS row edits an
// index and the PERSISTED value is the stable `id` string (settings_store.h's rule), resolved at boot.
//
// Lifetime is the RENDERER's, like `Skin`: `unload()` before `SdlVideo::close()`.

#ifndef POCKETTRACKER_OVERLAY_H
#define POCKETTRACKER_OVERLAY_H

#include <SDL.h>

#include <cstdint>
#include <string>

namespace ptshell {

// One entry per overlay PNG under `assets/overlays/`. `id` is the persisted key and asset leaf;
// `displayName` is the OVERLAY column text (the id uppercased, eight characters). The SETTINGS cycle
// is `["OFF"] + kScreenOverlays`: index 0 is OFF, index i is `kScreenOverlays[i-1]`.
struct ScreenOverlayDef {
    const char* id;           // persisted overlay_name + asset-folder leaf: "crt_scanlines"
    const char* displayName;  // SETTINGS column text = uppercase(id).take(8): "CRT_SCAN"
    const char* file;         // asset-seam path: "overlays/crt_scanlines.png"
};

// Only `crt_scanlines.png` ships today (app/src/main/assets/overlays/), so the table has one entry.
// Adding an overlay is one line here; the SETTINGS count and cycle follow from `kScreenOverlayCount`.
inline constexpr ScreenOverlayDef kScreenOverlays[] = {
    {"crt_scanlines", "CRT_SCAN", "overlays/crt_scanlines.png"},
};
inline constexpr int kScreenOverlayCount =
    static_cast<int>(sizeof(kScreenOverlays) / sizeof(kScreenOverlays[0]));

// The number of choices the OVERLAY row cycles: "OFF" + every shipped overlay.
inline constexpr int screen_overlay_choice_count() { return 1 + kScreenOverlayCount; }

/** Resolve a persisted overlay id to its cycle index; "OFF", an unknown or a mangled id → 0 (OFF). */
inline int screen_overlay_index(const std::string& id) {
    for (int i = 0; i < kScreenOverlayCount; ++i)
        if (id == kScreenOverlays[i].id) return i + 1;
    return 0;  // OFF
}

/** The stable id string for a cycle index — 0 → "OFF", i → `kScreenOverlays[i-1].id`. This is what
 *  settings.json persists (`overlay_name`), so the choice survives the table being reordered. */
inline std::string screen_overlay_id(int index) {
    if (index <= 0 || index > kScreenOverlayCount) return "OFF";
    return kScreenOverlays[index - 1].id;
}

/** The SETTINGS display text for a cycle index — "OFF", or the pre-uppercased `displayName`. */
inline std::string screen_overlay_text(int index) {
    if (index <= 0 || index > kScreenOverlayCount) return "OFF";
    return kScreenOverlays[index - 1].displayName;
}

class ScreenOverlay {
public:
    ScreenOverlay() = default;
    ~ScreenOverlay() { unload(); }

    ScreenOverlay(const ScreenOverlay&)            = delete;  // owns an SDL_Texture — non-copyable
    ScreenOverlay& operator=(const ScreenOverlay&) = delete;

    /**
     * Decode `kScreenOverlays[index-1]`'s PNG and upload it to a blended texture. `index` 0 (OFF) or
     * out of range unloads and loads nothing. A missing/corrupt PNG is not fatal (decoration): it
     * unloads and `draw` becomes a no-op. When `log`, prints one `overlay:` line — the on-device
     * account of whether a real PNG came out of the APK.
     * Returns true when a texture is now loaded.
     */
    bool load(SDL_Renderer* renderer, int index, bool log);

    /** Destroy the texture. Idempotent; call before the renderer is destroyed. */
    void unload();

    /** Blit the overlay across `dst` (the tracker frame rect), alpha-blended at `strength` (0–255).
     *  No-op when nothing is loaded or `strength <= 0`. */
    void draw(SDL_Renderer* renderer, const SDL_Rect& dst, int strength) const;

    bool loaded() const { return tex_ != nullptr; }

private:
    SDL_Texture* tex_ = nullptr;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_OVERLAY_H
