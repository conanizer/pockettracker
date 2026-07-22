// ─── shell/device_skin.h — the selectable PORTRAIT2 device skins (convergence D-theme) ─────────────
//
// The shell-side twin of Kotlin's `DeviceSkin` table (ui/theme/DeviceTheme.kt). One entry per portrait
// skin the SETTINGS > LAYOUT row can cycle: which asset folder its PNGs come from, and the three scalars
// the PNG set does NOT carry — the casing fill painted behind/around the skin, the button-label colour,
// and the bezel border in skin X-units.
//
// It is the RENDERER's knowledge (which theme paints the chrome), so it lives shell-side beside
// `skin.{h,cpp}` and `portrait2.{h,cpp}`, not in pt-ui — the canvas stays theme-free (skin.h). The list
// ORDER is the SETTINGS cycle order and the persisted index space (NORM first, DARK second), matching
// Kotlin's `DeviceSkin.ALL`. The persisted key is the `id` STRING, resolved to an index at boot, so the
// choice survives the list being reordered — the "stable string, not an index" contract settings_store.h
// spelled out for exactly this (LAYOUT/SKIN were deferred until Phase D could resolve a name to a list).
//
// Both amiga skins ship a bezel PNG, so `bezelThicknessX` is 3 for each (a solid-colour bezel would use
// a dp fallback, which no shipping skin needs). Values lifted from `DeviceSkin.AMIGA_NORMAL/AMIGA_DARK`
// and `DeviceTheme.AMIGA` (screenBezelThicknessX = 3).

#ifndef POCKETTRACKER_DEVICE_SKIN_H
#define POCKETTRACKER_DEVICE_SKIN_H

#include <cstdint>
#include <string>

namespace ptshell {

struct DeviceSkinDef {
    const char* id;               // persisted key + asset-folder leaf: "amiga" / "amiga-2"
    const char* displayName;      // the SETTINGS skin column text: "NORM" / "DARK"
    uint32_t    casingFillArgb;   // casing clear behind/around the skin (DeviceSkin.casingFillColor)
    uint32_t    labelRgb;         // button-label colour, 0xRRGGBB (DeviceSkin.labelColor)
    float       bezelThicknessX;  // bezel border in skin X-units (DeviceTheme.screenBezelThicknessX)
};

// NORM = beige amiga skin, near-black labels; DARK = slate amiga-2 skin, white labels. DARK is index 1
// and the fallback below, because it is the look the shell shipped hardcoded before selection existed.
inline constexpr DeviceSkinDef kDeviceSkins[] = {
    {"amiga",   "NORM", 0xFFE1D0BA, 0x0D0D0D, 3.0f},
    {"amiga-2", "DARK", 0xFF56606C, 0xFFFFFF, 3.0f},
};
inline constexpr int kDeviceSkinCount = static_cast<int>(sizeof(kDeviceSkins) / sizeof(kDeviceSkins[0]));

/** Resolve a persisted skin id to its index; an unknown / mangled id → DARK (1), the shell's prior
 *  hardcode, so an older or hand-edited settings.json keeps the look it shipped with rather than
 *  jumping to NORM. */
inline int device_skin_index(const std::string& id) {
    for (int i = 0; i < kDeviceSkinCount; ++i)
        if (id == kDeviceSkins[i].id) return i;
    return 1;  // amiga-2 / DARK
}

}  // namespace ptshell

#endif  // POCKETTRACKER_DEVICE_SKIN_H
