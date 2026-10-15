// The button font: `font_raster.h` rasterizes the .otf, this uploads glyphs to textures cached per
// (codepoint, size) and blits UTF-8 labels. Glyphs are white with alpha = coverage, tinted per draw,
// in LINEAR scale mode like the skin around them. ⚠️ `unload()` before `SdlVideo::close()`.
// D-pad arrows come from a second Font on Linux Biolinum (Helvetica has none); `draw_arrow` is the
// line-arrow fallback when that font is missing.

#ifndef POCKETTRACKER_FONT_H
#define POCKETTRACKER_FONT_H

#include <SDL.h>

#include "skin/font_raster.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ptshell {

// The four D-pad directions; mapping a Button onto one is the caller's.
enum class Arrow { Up, Down, Left, Right };

class Font {
public:
    Font() = default;
    ~Font() { unload(); }

    Font(const Font&)            = delete;  // owns SDL_Texture handles
    Font& operator=(const Font&) = delete;

    // Read and parse the .otf through the asset seam; false (and `loaded()` false) if missing or bad,
    // so the caller falls back to the 5×5 font. `log` prints one `font:` line.
    bool load(SDL_Renderer* renderer, const std::string& asset_rel, bool log);
    bool loaded() const { return ready_; }

    // Idempotent; before the renderer is destroyed.
    void unload();

    // UTF-8 `text`, top-left at (x_left, y_top), em size `px`, tinted `rgb`; the baseline is
    // `ascent_px(px)` below the top. A missing codepoint is skipped, its advance kept.
    void draw_text(const std::string& text, int x_left, int y_top, float px, uint32_t rgb);

    // ⚠️ No measure(): a width must sum cached advances, not rasterize per frame.

    int ascent_px(float px) const { return rast_.ascent_px(px); }

    // A generated line-arrow sprite filling `box`, tinted `rgb`. Independent of the .otf, but the
    // caller gates it on `loaded()` so a failed load drops the whole cluster to 5×5 labels together.
    void draw_arrow(Arrow dir, const SDL_Rect& box, uint32_t rgb);

private:
    // A cached glyph or arrow texture and how to place it; null with an advance = a space.
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
