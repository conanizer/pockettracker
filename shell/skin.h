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

#include <string>

namespace ptshell {

// The pieces a theme ships, one enumerator per PNG file (the names mirror the filenames under
// `assets/themes/<name>/`). `amiga` and `amiga-2` both carry the full set. `COUNT` sizes the table.
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
     */
    int  load(SDL_Renderer* renderer, const std::string& theme, bool log);

    /** Destroy every texture. Idempotent; call before the renderer is destroyed. */
    void unload();

    /** The texture for a piece, or a {nullptr,0,0} SkinTexture if it did not load. */
    const SkinTexture& piece(SkinPiece p) const { return pieces_[static_cast<int>(p)]; }

    /** Blit a piece into `dst` (scaled, alpha-blended). No-op if the piece did not load — so a caller
     *  need not guard every draw against a theme that shipped an incomplete set. */
    void draw(SDL_Renderer* renderer, SkinPiece p, const SDL_Rect& dst) const;

    bool loaded() const { return count_ > 0; }

private:
    SkinTexture pieces_[static_cast<int>(SkinPiece::COUNT)];
    int         count_ = 0;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_SKIN_H
