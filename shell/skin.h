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
// once: which PNGs `Skin::load` looks for, how they are uploaded, and what the renderer draws over
// them. `kFiles` in skin.cpp says which sets each file belongs to — a file may belong to more than
// one, which is how the two shape-only sets share the same generic button art.
//
//   Chrome       — the four background bands plus the generic button shapes, in the skin's own
//                  colours; the button CHARACTERS are drawn over them in a font by the renderer.
//                  `amiga` and `amiga-2`.
//   Bitmap       — one image PER BUTTON, each already carrying its own character, so nothing is drawn
//                  on top. No background art at all. `amiga-bitmap`.
//   Transparent  — the generic button SHAPES alone, authored as white ink on transparent, and no
//                  background art: the shapes are tinted to the live theme and the characters are
//                  drawn over them in a font, in that same colour. `amiga-transparent`.
enum class SkinArt : uint8_t { Chrome, Bitmap, Transparent };

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
    // ── Bitmap set: one per button, character included ──
    BmpUpNormal,           // btn_up_normal.png
    BmpUpPressed,          // btn_up_pressed.png
    BmpDownNormal,         // btn_down_normal.png
    BmpDownPressed,        // btn_down_pressed.png
    BmpLeftNormal,         // btn_left_normal.png
    BmpLeftPressed,        // btn_left_pressed.png
    BmpRightNormal,        // btn_right_normal.png
    BmpRightPressed,       // btn_right_pressed.png
    BmpANormal,            // btn_a_normal.png
    BmpAPressed,           // btn_a_pressed.png
    BmpBNormal,            // btn_b_normal.png
    BmpBPressed,           // btn_b_pressed.png
    BmpSelNormal,          // btn_sel_normal.png
    BmpSelPressed,         // btn_sel_pressed.png
    BmpStartNormal,        // btn_start_normal.png
    BmpStartPressed,       // btn_start_pressed.png
    BmpLShiftNormal,       // btn_lshift_normal.png
    BmpLShiftPressed,      // btn_lshift_pressed.png
    BmpRShiftNormal,       // btn_rshift_normal.png
    BmpRShiftPressed,      // btn_rshift_pressed.png
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
     * `art` selects which pieces to attempt (see SkinArt) — a chrome theme is never asked for
     * per-button art, nor the reverse, so the MISS lines stay real misses rather than twenty lines of
     * "this theme was never going to have that".
     *
     * ⚠️ It also changes HOW the Bitmap set is uploaded. That art is two-colour and OPAQUE (black
     * ground, white ink), but it is drawn TINTED to the live tracker theme — so the ink's brightness
     * becomes ALPHA and the colour becomes flat white, leaving `SDL_SetTextureColorMod` free to pick
     * the ink colour per frame and the black ground to fall away to whatever is behind it. Uploading it
     * as-is would paint an opaque black box no colour mod could lighten. Scaling is NEAREST for the
     * same art, not the others' LINEAR: it is pixel art, and smoothing it is the one thing that would
     * stop it reading as pixel art.
     *
     * ⚠️ The Transparent set needs NEITHER — it is authored white-on-transparent already, so it goes up
     * untouched and tints straight away, and its edges are anti-aliased curves that want the smoothing.
     * The rule both branches follow is "match the handling to what the art IS", not to which set it is in.
     */
    int  load(SDL_Renderer* renderer, const std::string& theme, bool log, SkinArt art);

    /** Destroy every texture. Idempotent; call before the renderer is destroyed. */
    void unload();

    /** The texture for a piece, or a {nullptr,0,0} SkinTexture if it did not load. */
    const SkinTexture& piece(SkinPiece p) const { return pieces_[static_cast<int>(p)]; }

    /** Blit a piece into `dst` (scaled, alpha-blended). No-op if the piece did not load — so a caller
     *  need not guard every draw against a theme that shipped an incomplete set. */
    void draw(SDL_Renderer* renderer, SkinPiece p, const SDL_Rect& dst) const;

    /** As `draw`, but multiplying the piece by `rgb` (0xRRGGBB) — the ink colour of the two tinted art
     *  sets, which is the live theme's and therefore cannot be baked into the texture at load. The mod
     *  is set and put back to white around the blit, so it never leaks into the next piece drawn. */
    void draw_tinted(SDL_Renderer* renderer, SkinPiece p, const SDL_Rect& dst, uint32_t rgb) const;

    bool loaded() const { return count_ > 0; }

private:
    SkinTexture pieces_[static_cast<int>(SkinPiece::COUNT)];
    int         count_ = 0;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_SKIN_H
