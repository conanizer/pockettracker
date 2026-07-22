// ─── shell/font_raster.h — glyph rasterization, SDL-free (convergence D — the button font) ────────
//
// The PURE half of the shell's text renderer: it parses a TrueType/OpenType font (stb_truetype) and
// rasterizes a Unicode codepoint to an 8-bit alpha-coverage bitmap plus the metrics to place it. No
// SDL, no textures, no window — exactly the split image.cpp (pure `decode_png`) makes against skin.cpp
// (the SDL textures). `shell/font.{h,cpp}` builds `SDL_Texture`s on top of this; `tools/ptfont` drives
// THIS directly, linking nothing, so the rasterizer is proven on a real compiler independent of SDL.
//
// ⚠️ The button font is `helvetica_regular.otf`, which is CFF/PostScript-outline OpenType (`OTTO`), not
// the commoner TrueType (`glyf`) flavour. stb_truetype supports CFF, but that path is the one at risk
// of silently regressing on a version bump — which is precisely why ptfont asserts a CFF font still
// produces ink, and does it against the exact file the shell ships.

#ifndef POCKETTRACKER_FONT_RASTER_H
#define POCKETTRACKER_FONT_RASTER_H

#include <cstddef>
#include <cstdint>
#include <vector>

// The font's parsed form is stb_truetype's; kept out of this header (forward-declared) so only
// font_raster.cpp pulls in the ~200 KB single-header library — the same discipline image.cpp keeps for
// stb_image. The struct tag is all a pointer member needs.
struct stbtt_fontinfo;

namespace ptshell {

// One rasterized glyph: an alpha coverage bitmap (0..255 per pixel, row-major, `w*h` bytes) plus the
// offsets that place it against a pen sitting on the baseline. `coverage` is EMPTY for a glyph with no
// ink (a space, or a zero-area glyph) — `advance` is still valid, so the pen still moves.
struct RasterGlyph {
    std::vector<std::uint8_t> coverage;   // w*h, or empty for a blank glyph
    int  w        = 0;                    // bitmap width  in px
    int  h        = 0;                    // bitmap height in px
    int  xoff     = 0;                    // pen-x → bitmap left edge (usually ≥ 0)
    int  yoff     = 0;                    // baseline-y → bitmap top edge (negative: above the baseline)
    int  advance  = 0;                    // px to advance the pen after this glyph
    bool has_glyph = false;               // false when the font has NO glyph for the codepoint (.notdef)
};

// Owns a parsed font and rasterizes glyphs from it at a requested pixel EM size ("font size" in the
// CSS/Compose sense — the em square maps to `px` pixels). Non-copyable: it holds the parse state and
// the font bytes it points into.
class FontRasterizer {
public:
    FontRasterizer();
    ~FontRasterizer();
    FontRasterizer(const FontRasterizer&)            = delete;
    FontRasterizer& operator=(const FontRasterizer&) = delete;

    // Parse `len` bytes of a .ttf/.otf. Copies them (the parser points back into the copy), so the
    // caller's buffer need not outlive this. Returns false — and leaves `ready()` false — if the bytes
    // are not a font stb_truetype can use.
    bool init(const std::uint8_t* data, std::size_t len);
    bool ready() const { return ready_; }

    // Vertical metrics at pixel size `px`. `ascent_px` is how far the baseline sits below the text top;
    // `line_height_px` is ascent − descent + line-gap, all scaled and rounded.
    int ascent_px(float px) const;
    int descent_px(float px) const;
    int line_height_px(float px) const;

    // Rasterize one Unicode codepoint at `px`. On a font with no glyph for it, `has_glyph` is false and
    // the .notdef advance is returned (so a caller can choose to fall back rather than draw a tofu box).
    RasterGlyph glyph(std::uint32_t codepoint, float px) const;

private:
    std::vector<std::uint8_t> data_;            // the font bytes (info_ points into these)
    stbtt_fontinfo*           info_  = nullptr; // heap so stb_truetype stays inside the .cpp
    bool                      ready_ = false;
};

}  // namespace ptshell

#endif  // POCKETTRACKER_FONT_RASTER_H
