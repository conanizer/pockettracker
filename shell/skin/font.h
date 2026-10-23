// The button font: `font_raster.h` rasterizes the font file, this uploads glyphs to textures cached
// per (codepoint, size) and blits UTF-8 labels. Glyphs are white with alpha = coverage, tinted per
// draw, in LINEAR scale mode like the skin around them. ⚠️ `unload()` before `SdlVideo::close()`.

#ifndef POCKETTRACKER_FONT_H
#define POCKETTRACKER_FONT_H

#include <SDL.h>

#include "skin/font_raster.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ptshell {

class Font {
public:
    Font() = default;
    ~Font() { unload(); }

    Font(const Font&)            = delete;  // owns SDL_Texture handles
    Font& operator=(const Font&) = delete;

    // Read and parse the font file through the asset seam; false (and `loaded()` false) if missing or
    // bad, so the caller falls back to the 5×5 font. `log` prints one `font:` line.
    bool load(SDL_Renderer* renderer, const std::string& asset_rel, bool log);
    bool loaded() const { return ready_; }

    // Idempotent; before the renderer is destroyed.
    void unload();

    // UTF-8 `text`, top-left at (x_left, y_top), em size `px`, tinted `rgb`; the baseline is
    // `ascent_px(px)` below the top. A missing codepoint is skipped, its advance kept.
    void draw_text(const std::string& text, int x_left, int y_top, float px, uint32_t rgb);

    // ⚠️ No measure(): a width must sum cached advances, not rasterize per frame.

    int ascent_px(float px) const { return rast_.ascent_px(px); }

private:
    // A cached glyph texture and how to place it; null with an advance = a space.
    struct GlyphTex {
        SDL_Texture* tex     = nullptr;
        int          w       = 0;
        int          h       = 0;
        int          xoff    = 0;   // pen-x → texture left
        int          yoff    = 0;   // baseline-y → texture top
        int          advance = 0;
    };

    const GlyphTex& glyph_tex(std::uint32_t codepoint, float px);   // rasterize + upload on miss
    SDL_Texture*    make_texture(int w, int h, const std::uint32_t* argb) const;

    static std::uint64_t glyph_key(std::uint32_t codepoint, float px);

    SDL_Renderer*                              renderer_ = nullptr;
    FontRasterizer                             rast_;
    std::unordered_map<std::uint64_t, GlyphTex> cache_;
    bool                                       ready_ = false;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_FONT_H
