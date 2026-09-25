// ─── shell/skin.h — the touch-skin textures, decoded once and owned by the renderer (D1/D2/D7) ───
//
// Phase D's touch skin is chrome drawn AROUND the 640×480 frame, in device-resolution space, in the
// shell's `present()` — never in the canvas (pt-ui keeps its four primitives; see image.h). This holds
// the SDL textures for one theme's skin: the PNGs are read through the D7 asset seam (assets.h),
// decoded through the D2 decoder (image.h), and uploaded to `SDL_Texture`s here, once, at load.
//
// It is the SHELL's, and its lifetime is the RENDERER's: the textures are created from an
// `SDL_Renderer*` and must be destroyed before it is (`unload()` before `SdlVideo::close()`).
//
// ⚠️ This holder is deliberately SEMANTIC-FREE: a `SkinPiece` names the FILE it came from, not the band
// it lands in. Which piece goes where — the top panel, the bezel the frame sits inside, the button
// cluster — is the RENDERER's knowledge (ScreenLayouts.kt / VirtualControlsPortrait2), and belongs in
// the code that computes the destination rects, not in the thing that merely owns the pixels.

#ifndef POCKETTRACKER_SKIN_H
#define POCKETTRACKER_SKIN_H

#include <SDL.h>

#include <cstdint>
#include <string>

namespace ptshell {

// Which ART SET a theme ships. A theme carries exactly one, and the choice decides three things at
// once: which PNGs `Skin::load` looks for, how the renderer lays them out, and what colour they are
// drawn in. `kFiles` in skin.cpp says which sets each file belongs to — both share the generic button
// shapes, each from its own folder.
//
//   Chrome       — the four background bands plus the generic button shapes, in the skin's own
//                  colours; the button CHARACTERS are drawn over them in a font by the renderer.
//                  `amiga` and `amiga-2`.
//   Transparent  — the generic button SHAPES alone, authored as white ink on transparent, and no
//                  background art: the shapes are tinted to the live theme and the characters are
//                  drawn over them in a font, in that same colour. `amiga-transparent`.
enum class SkinArt : uint8_t { Chrome, Transparent };

// The pieces a theme ships, one enumerator per PNG file (the names mirror the filenames under
// `assets/themes/<name>/`). `COUNT` sizes the table.
//
// ⚠️ `Skin::load` attempts only the files its theme's art set uses, so a theme is never asked for art
// it was never going to ship and a MISS line is always a real miss.
//
// ⚠️ AN ENUMERATOR'S NUMBER IS ITS IDENTITY — append, never insert: skin.cpp's filename table is
// indexed by it, and the static_assert there catches only a COUNT mismatch, not a shifted row.
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

// A loaded texture and its source dimensions (the renderer needs the native size to scale it into a
// band). `tex == nullptr` means "this piece did not load" — a hole to skip, not a crash.
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

    Skin(const Skin&)            = delete;  // owns SDL_Texture handles — non-copyable
    Skin& operator=(const Skin&) = delete;

    /**
     * Decode the PNGs under `assets/themes/<theme>/` and upload each to a texture on `renderer`.
     *
     * A missing or corrupt piece is SKIPPED, not fatal: a skin is decoration, and a theme that ships
     * without (say) the dark button variants should draw the rest rather than nothing. Returns how many
     * pieces loaded. When `log`, prints one `skin:` line per piece with its dimensions or MISS — the
     * on-device readout that tells a real decode from a silent no-op (there is no console assertion for
     * this on a phone; the log line IS the assertion).
     *
     * `art` selects which pieces to attempt (see SkinArt) — a transparent theme is never asked for the
     * chrome bands, so the MISS lines stay real misses rather than lines of "this theme was never going
     * to have that".
     *
     * ⚠️ The Transparent set is uploaded untouched and tinted at blit time, which works only because it
     * is authored flat white with a real alpha channel. Art authored opaque (ink on a solid ground)
     * would come out as a solid box no colour mod could lighten.
     */
    int  load(SDL_Renderer* renderer, const std::string& theme, bool log, SkinArt art);

    /** Destroy every texture. Idempotent; call before the renderer is destroyed. */
    void unload();

    /** The texture for a piece, or a {nullptr,0,0} SkinTexture if it did not load. */
    const SkinTexture& piece(SkinPiece p) const { return pieces_[static_cast<int>(p)]; }

    /** Blit a piece into `dst` (scaled, alpha-blended). No-op if the piece did not load — so a caller
     *  need not guard every draw against a theme that shipped an incomplete set. */
    void draw(SDL_Renderer* renderer, SkinPiece p, const SDL_Rect& dst) const;

    /** As `draw`, but multiplying the piece by `rgb` (0xRRGGBB) — the ink colour of the Transparent
     *  set, which is the live theme's and therefore cannot be baked into the texture at load. The mod
     *  is set and put back to white around the blit, so it never leaks into the next piece drawn. */
    void draw_tinted(SDL_Renderer* renderer, SkinPiece p, const SDL_Rect& dst, uint32_t rgb) const;

    bool loaded() const { return count_ > 0; }

private:
    SkinTexture pieces_[static_cast<int>(SkinPiece::COUNT)];
    int         count_ = 0;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_SKIN_H
