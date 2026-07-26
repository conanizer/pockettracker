// ─── shell/overlay.{h,cpp} — the SCREEN-OVERLAY texture (convergence plan D6) ─────────────────────
//
// The CRT-scanline filter Android draws OVER the tracker screen: a full-frame PNG composited on top of
// the 640×480 display at an adjustable strength, exactly as `ScreenLayouts.kt`'s `TrackerScreen` does
// with `drawImage(..., alpha = overlayStrength / 255f)` over `PixelPerfectTracker`. Like the touch
// skin (skin.h) it is CHROME the shell draws around/over the frame — never in the canvas, which keeps
// its four primitives (image.h) — so it lives shell-side and holds its own SDL texture.
//
// It is ONE texture, not a set: the pieces are named by a fixed table below (`kScreenOverlays`), the
// shell-side twin of Kotlin's `assets/overlays/` listing. Android enumerated that folder at run time;
// the shell converges on a fixed shipped set exactly as `device_skin.h` did for the skins — an index
// is meaningless without the list it indexes, so the SETTINGS row edits an index and the PERSISTED
// value is the stable `id` string (settings_store.h's rule), resolved to an index at boot.
//
// Lifetime is the RENDERER's, like `Skin`: the texture is created from an `SDL_Renderer*` and must be
// destroyed before it is (`unload()` before `SdlVideo::close()`).

#ifndef POCKETTRACKER_OVERLAY_H
#define POCKETTRACKER_OVERLAY_H

#include <SDL.h>

#include <cstdint>
#include <string>

namespace ptshell {

// One entry per overlay PNG shipped under `assets/overlays/`. `id` is the persisted key + asset leaf
// (matching Android's `overlay_name` SharedPreferences value); `displayName` is what SETTINGS paints
// in the OVERLAY column — Kotlin shows `overlayName.uppercase().take(8)`, so the table carries that
// pre-computed (e.g. "crt_scanlines" → "CRT_SCAN"). The INDEX space the SETTINGS row cycles is
// `["OFF"] + kScreenOverlays`, matching Kotlin's `listOf("OFF") + overlayFiles`: index 0 is OFF, and
// index 1.. is `kScreenOverlays[index-1]`.
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

/** Resolve a persisted overlay id to its cycle index; "OFF", an unknown, or a mangled id → 0 (OFF),
 *  matching Kotlin's `options.indexOf(name)` falling back to OFF for anything not in the list. */
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
     * Decode `kScreenOverlays[index-1]`'s PNG (via the D7 asset seam → D2 decoder) and upload it to a
     * blended texture on `renderer`. `index` 0 (OFF) or out of range unloads and loads nothing — the
     * "OFF" choice draws no overlay. A missing/corrupt PNG is NOT fatal (a filter is decoration, not
     * correctness): it unloads and `loaded()` stays false, so `draw` becomes a no-op. When `log`,
     * prints one `overlay:` line — the on-device account of whether a real PNG came out of the APK,
     * there being no console test on a phone, exactly as `Skin::load` does.
     *
     * Returns true when a texture is now loaded.
     */
    bool load(SDL_Renderer* renderer, int index, bool log);

    /** Destroy the texture. Idempotent; call before the renderer is destroyed. */
    void unload();

    /** Blit the overlay across `dst` (the tracker frame rect), scaled and alpha-blended at
     *  `strength` (0–255 → the texture's alpha mod, the shell twin of Kotlin's `alpha = STR/255f`).
     *  No-op when nothing loaded or `strength <= 0`, so callers need not guard. */
    void draw(SDL_Renderer* renderer, const SDL_Rect& dst, int strength) const;

    bool loaded() const { return tex_ != nullptr; }

private:
    SDL_Texture* tex_ = nullptr;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_OVERLAY_H
