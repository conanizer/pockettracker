// ─── shell/font.h — the shell's text renderer: Helvetica labels as SDL textures (convergence D) ────
//
// The SDL half of the button font. `font_raster.h` (pure, SDL-free) turns the app's Helvetica .otf into
// alpha-coverage bitmaps; this uploads them to `SDL_Texture`s, caches them per (codepoint, size), and
// blits UTF-8 strings — the twin of skin.cpp (SDL textures) over image.cpp (pure decode). It is the
// RENDERER's, and its lifetime is the renderer's: textures come from an `SDL_Renderer*` and must be
// destroyed before it is (`unload()` before `SdlVideo::close()`), exactly as `Skin` requires.
//
// Two things it draws:
//   • draw_text — LABELS: the letters ("L Shift", "Sel", "A", …) in Helvetica, AND the D-pad arrows
//     (↑↓←→) when a `Font` loaded from an arrow-bearing font is used (the shell bundles Linux Biolinum
//     for exactly those four glyphs, since Helvetica's are .notdef — ptfont proves it). Both are real
//     glyphs, alpha-blended and tinted, which is what Android did (Text through the system fallback).
//   • draw_arrow — the FALLBACK D-pad arrow, used only when the arrow font failed to load: a smooth
//     anti-aliased THIN-LINE arrow (a stroked shaft + an open chevron head) drawn from a hi-res coverage
//     sprite, linear-filtered, so a missing arrow font degrades to a clean line arrow rather than tofu.
//
// Glyph textures are white with alpha = coverage, then tinted per draw via `SDL_SetTextureColorMod`, so
// one cached glyph serves any label colour. All of them carry LINEAR scale mode: the label is chrome
// scaled to the button, and must smooth like the skin around it, not stair-step like the framebuffer.

#ifndef POCKETTRACKER_FONT_H
#define POCKETTRACKER_FONT_H

#include <SDL.h>

#include "font_raster.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ptshell {

// The four D-pad directions, for draw_arrow. Not a Button — the mapping Button::DPAD_* → Arrow is the
// caller's (portrait2), the same way SkinPiece names a file and the renderer decides where it lands.
enum class Arrow { Up, Down, Left, Right };

class Font {
public:
    Font() = default;
    ~Font() { unload(); }

    Font(const Font&)            = delete;  // owns SDL_Texture handles — non-copyable
    Font& operator=(const Font&) = delete;

    /**
     * Read the .otf through the D7 asset seam (`assets.h`) and parse it. Returns false — and leaves
     * `loaded()` false — if the asset is missing or unparseable, so a caller can fall back to the 5×5
     * font rather than draw nothing. `log` prints one `font:` line (the on-device account of whether the
     * font actually loaded, the same role `skin:`'s lines play — there is no console assertion on a
     * phone). `renderer` is remembered for the glyph/arrow textures.
     */
    bool load(SDL_Renderer* renderer, const std::string& asset_rel, bool log);
    bool loaded() const { return ready_; }

    /** Destroy every cached glyph/arrow texture. Idempotent; call before the renderer is destroyed. */
    void unload();

    /**
     * Draw UTF-8 `text` with its top-left at (`x_left`, `y_top`), em size `px` (the fontSize sense: the
     * em square maps to px pixels), tinted `rgb` (0xRRGGBB). The baseline is placed `ascent_px(px)`
     * below `y_top`, matching a Compose `Text`'s first-baseline-to-top. A codepoint the font lacks is
     * skipped (its advance still applies) — letters only reach here; arrows go through draw_arrow.
     */
    void draw_text(const std::string& text, int x_left, int y_top, float px, uint32_t rgb);

    /** Total advance width of `text` at `px`, for a caller that wants to centre rather than offset. */
    int measure(const std::string& text, float px) const;

    /** The baseline drop below the text top at `px` — a Compose `Text` places its first baseline here. */
    int ascent_px(float px) const { return rast_.ascent_px(px); }

    /**
     * Draw a smooth D-pad arrow filling `box` (centred, with a small internal margin), tinted `rgb`.
     * Independent of the loaded .otf — it is a generated line-arrow sprite — but gated on `loaded()` by
     * the caller so a font-load failure falls the whole cluster back to the 5×5 labels together.
     */
    void draw_arrow(Arrow dir, const SDL_Rect& box, uint32_t rgb);

private:
    // A cached glyph (or arrow) texture and the metrics to place it. `tex == nullptr` with a positive
    // advance is a blank glyph (space): the pen moves, nothing is drawn.
    struct GlyphTex {
        SDL_Texture* tex     = nullptr;
        int          w       = 0;
        int          h       = 0;
        int          xoff    = 0;   // pen-x → texture left  (glyphs); 0 for arrows
        int          yoff    = 0;   // baseline-y → texture top (glyphs); unused for arrows
        int          advance = 0;   // glyphs only
    };

    const GlyphTex& glyph_tex(std::uint32_t codepoint, float px);   // rasterize + upload on miss
    const GlyphTex& arrow_tex(Arrow dir);                           // generate + upload on miss
    SDL_Texture*    make_texture(int w, int h, const std::uint32_t* argb) const;

    static std::uint64_t glyph_key(std::uint32_t codepoint, float px);
    static std::uint64_t arrow_key(Arrow dir);

    SDL_Renderer*                              renderer_ = nullptr;
    FontRasterizer                             rast_;
    std::unordered_map<std::uint64_t, GlyphTex> cache_;
    bool                                       ready_ = false;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_FONT_H
