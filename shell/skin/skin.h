// The touch-skin textures: PNGs decoded once and uploaded per renderer. ⚠️ `unload()` before
// `SdlVideo::close()`. A `SkinPiece` names a file, not where it lands — that is the renderer's.

#ifndef POCKETTRACKER_SKIN_H
#define POCKETTRACKER_SKIN_H

#include <SDL.h>

#include <cstdint>
#include <string>

namespace ptshell {

// A theme's art set, deciding which PNGs load, the layout and the colour:
//   Chrome      — four background bands + button shapes in the skin's own colours (amiga, amiga-2).
//   Transparent — button shapes only, white on transparent, tinted to the live theme.
enum class SkinArt : uint8_t { Chrome, Transparent };

// One per PNG under `assets/themes/<name>/`. Only the files the art set uses are attempted, so a MISS
// line is a real miss. ⚠️ Append, never insert: skin.cpp's filename table is indexed by it.
enum class SkinPiece {
    TopPanel,              // bg_top_panel.png
    BrandingPanel,         // bg_branding_panel.png
    ButtonBacking,         // bg_button_backing.png
    ScreenBezel,           // bg_screen_bezel.png
    BtnSquareNormal,       // btn_square_normal.png
    BtnSquarePressed,      // btn_square_pressed.png
    BtnSquareNormalDark,   // btn_square_normal_dark.png
    BtnSquarePressedDark,  // btn_square_pressed_dark.png
    BtnWideNormal,         // btn_wide_normal.png
    BtnWidePressed,        // btn_wide_pressed.png
    COUNT
};

// A texture and its native size; null = did not load, a hole to skip.
struct SkinTexture {
    SDL_Texture* tex    = nullptr;
    int          width  = 0;
    int          height = 0;
    explicit     operator bool() const { return tex != nullptr; }
};

class Skin {
public:
    Skin() = default;
    ~Skin() { unload(); }

    Skin(const Skin&)            = delete;  // owns SDL_Texture handles
    Skin& operator=(const Skin&) = delete;

    // Decode `assets/themes/<theme>/`'s PNGs for `art`; a missing piece is skipped. Returns how many
    // loaded; `log` prints one line per piece (size or MISS). ⚠️ Transparent art is tinted at blit
    // time, which works only because it is flat white with real alpha.
    int  load(SDL_Renderer* renderer, const std::string& theme, bool log, SkinArt art);

    // Idempotent; before the renderer is destroyed.
    void unload();

    const SkinTexture& piece(SkinPiece p) const { return pieces_[static_cast<int>(p)]; }

    // Scaled, alpha-blended; a no-op for a piece that did not load.
    void draw(SDL_Renderer* renderer, SkinPiece p, const SDL_Rect& dst) const;

    // Multiplied by `rgb` (the live theme's ink); the colour mod is reset after the blit.
    void draw_tinted(SDL_Renderer* renderer, SkinPiece p, const SDL_Rect& dst, uint32_t rgb) const;

    bool loaded() const { return count_ > 0; }

private:
    SkinTexture pieces_[static_cast<int>(SkinPiece::COUNT)];
    int         count_ = 0;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_SKIN_H
