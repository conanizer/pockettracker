// ─── shell/font_raster.cpp — the ONE stb_truetype implementation TU (convergence D — button font) ──
//
// ⚠️ This is the only translation unit in the whole tree that pulls in the stb_truetype implementation,
// exactly as shell/image.cpp is the only one that pulls in stb_image. Nothing in native/ (the engine)
// or native/ui/ (pt-ui) includes it: text is drawn shell-side onto the device skin, never in the
// canvas (pt-ui keeps its four primitives). See native/vendor/stb_truetype/PT-VENDORING.md.

#include <cmath>

// stb_truetype's implementation, its single dependency block. No STBTT_STATIC: the symbols are internal
// to this TU already (it is the only includer), and the tools that link it want ordinary linkage.
#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype/stb_truetype.h"

#include "font_raster.h"

namespace ptshell {

namespace {
inline int iround(float v) { return static_cast<int>(std::floor(v + 0.5f)); }
}  // namespace

FontRasterizer::FontRasterizer()  = default;
FontRasterizer::~FontRasterizer() { delete info_; }

bool FontRasterizer::init(const std::uint8_t* data, std::size_t len) {
    ready_ = false;
    delete info_;
    info_ = nullptr;
    data_.clear();
    if (data == nullptr || len < 4) return false;

    data_.assign(data, data + len);

    // Font 0 of the file (a plain .otf/.ttf has exactly one; a .ttc collection has several — we take
    // the first, which is all a single-family button font ever holds).
    const int offset = stbtt_GetFontOffsetForIndex(data_.data(), 0);
    if (offset < 0) {
        data_.clear();
        return false;
    }

    info_ = new stbtt_fontinfo{};
    if (!stbtt_InitFont(info_, data_.data(), offset)) {
        delete info_;
        info_ = nullptr;
        data_.clear();
        return false;
    }
    ready_ = true;
    return true;
}

int FontRasterizer::ascent_px(float px) const {
    if (!ready_) return 0;
    const float scale = stbtt_ScaleForMappingEmToPixels(info_, px);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(info_, &ascent, &descent, &lineGap);
    return iround(static_cast<float>(ascent) * scale);
}

int FontRasterizer::descent_px(float px) const {
    if (!ready_) return 0;
    const float scale = stbtt_ScaleForMappingEmToPixels(info_, px);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(info_, &ascent, &descent, &lineGap);
    return iround(static_cast<float>(descent) * scale);  // descent is negative in font units
}

int FontRasterizer::line_height_px(float px) const {
    if (!ready_) return 0;
    const float scale = stbtt_ScaleForMappingEmToPixels(info_, px);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(info_, &ascent, &descent, &lineGap);
    return iround(static_cast<float>(ascent - descent + lineGap) * scale);
}

RasterGlyph FontRasterizer::glyph(std::uint32_t codepoint, float px) const {
    RasterGlyph out;
    if (!ready_) return out;

    const float scale = stbtt_ScaleForMappingEmToPixels(info_, px);

    // Glyph INDEX, not the codepoint API, so a missing glyph is observable: index 0 is .notdef. A caller
    // can then fall back (the 5×5 arrows) rather than blit a tofu box.
    const int gi = stbtt_FindGlyphIndex(info_, static_cast<int>(codepoint));
    out.has_glyph = (gi != 0);

    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(info_, gi, &advance, &lsb);
    out.advance = iround(static_cast<float>(advance) * scale);

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(info_, gi, scale, scale, &x0, &y0, &x1, &y1);
    const int w = x1 - x0;
    const int h = y1 - y0;
    if (w > 0 && h > 0) {
        out.coverage.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        // Rasterize into OUR buffer (stride = w). This is the non-allocating variant — the malloc
        // variant would hand back a buffer we would only copy and free.
        stbtt_MakeGlyphBitmap(info_, out.coverage.data(), w, h, /*stride=*/w, scale, scale, gi);
        out.w    = w;
        out.h    = h;
        out.xoff = x0;
        out.yoff = y0;
    }
    return out;
}

}  // namespace ptshell
