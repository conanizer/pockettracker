// Glyph rasterization, SDL-free: stb_truetype turns a codepoint into an alpha-coverage bitmap and
// its metrics; font.h makes textures from it. ⚠️ helvetica_regular.otf is CFF-outline OpenType
// (`OTTO`), stb_truetype's least-travelled path — check it still produces ink after an update.

#ifndef POCKETTRACKER_FONT_RASTER_H
#define POCKETTRACKER_FONT_RASTER_H

#include <cstddef>
#include <cstdint>
#include <vector>

// Forward-declared so only font_raster.cpp includes the ~200 KB stb_truetype.
struct stbtt_fontinfo;

namespace ptshell {

// Alpha coverage (0..255, row-major) and the offsets that place it against a pen on the baseline.
// `coverage` is empty for a glyph with no ink; `advance` still moves the pen.
struct RasterGlyph {
    std::vector<std::uint8_t> coverage;   // w*h, or empty for a blank glyph
    int  w        = 0;                    // bitmap width  in px
    int  h        = 0;                    // bitmap height in px
    int  xoff     = 0;                    // pen-x → bitmap left edge (usually ≥ 0)
    int  yoff     = 0;                    // baseline-y → bitmap top (negative: above it)
    int  advance  = 0;                    // px to advance the pen after this glyph
    bool has_glyph = false;               // false: the font has no glyph (.notdef)
};

// A parsed font, rasterized at an em size of `px` pixels. Non-copyable: the parse points into its bytes.
class FontRasterizer {
public:
    FontRasterizer();
    ~FontRasterizer();
    FontRasterizer(const FontRasterizer&)            = delete;
    FontRasterizer& operator=(const FontRasterizer&) = delete;

    // Copies the bytes (the parser points into the copy). False if stb_truetype cannot use them.
    bool init(const std::uint8_t* data, std::size_t len);
    bool ready() const { return ready_; }

    // At `px`: the baseline's drop below the text top; ascent − descent + line gap, rounded.
    int ascent_px(float px) const;
    int line_height_px(float px) const;

    // No glyph → `has_glyph` false with the .notdef advance, so a caller can fall back.
    RasterGlyph glyph(std::uint32_t codepoint, float px) const;

private:
    std::vector<std::uint8_t> data_;            // info_ points into these
    stbtt_fontinfo*           info_  = nullptr; // heap so stb_truetype stays inside the .cpp
    bool                      ready_ = false;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_FONT_RASTER_H
