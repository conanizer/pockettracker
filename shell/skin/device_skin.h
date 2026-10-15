// The portrait skins SETTINGS > LAYOUT cycles, in order: asset folder plus what the PNGs do not carry
// (casing fill, label colour, bezel border in skin X-units). settings.json stores the `id`.

#ifndef POCKETTRACKER_DEVICE_SKIN_H
#define POCKETTRACKER_DEVICE_SKIN_H

#include "skin/skin.h"   // SkinArt

#include <cstdint>
#include <string>

namespace ptshell {

struct DeviceSkinDef {
    const char* id;               // persisted key and asset-folder leaf
    const char* displayName;      // the SETTINGS skin column text
    uint32_t    casingFillArgb;   // casing clear behind/around the skin
    uint32_t    labelRgb;         // button-label colour, 0xRRGGBB
    float       bezelThicknessX;  // bezel border in skin X-units
    SkinArt     art;
};

// NORM = beige, near-black labels; DARK (the default) = slate, white labels. TRNS is Transparent art:
// button shapes only, the bare full-width layout, and the live theme's colours — its two constants
// stay 0 so a mistaken read on the Chrome path comes out black, not plausibly beige.
inline constexpr DeviceSkinDef kDeviceSkins[] = {
    {"amiga",             "NORM", 0xFFE1D0BA, 0x0D0D0D, 3.0f, SkinArt::Chrome},
    {"amiga-2",           "DARK", 0xFF56606C, 0xFFFFFF, 3.0f, SkinArt::Chrome},
    {"amiga-transparent", "TRNS", 0,          0,        0.0f, SkinArt::Transparent},
};
inline constexpr int kDeviceSkinCount = static_cast<int>(sizeof(kDeviceSkins) / sizeof(kDeviceSkins[0]));

// A persisted id → index; unknown or mangled → DARK.
inline int device_skin_index(const std::string& id) {
    for (int i = 0; i < kDeviceSkinCount; ++i)
        if (id == kDeviceSkins[i].id) return i;
    return 1;  // amiga-2 / DARK
}

}  // namespace ptshell

#endif  // POCKETTRACKER_DEVICE_SKIN_H
