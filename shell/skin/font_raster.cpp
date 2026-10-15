// The only stb_truetype implementation TU (see native/vendor/stb_truetype/PT-VENDORING.md); text is
// drawn shell-side, never in the canvas.

#include <cmath>

// No STBTT_STATIC: this is the only includer, and the tools that link it want ordinary linkage.
#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype/stb_truetype.h"

#include "skin/font_raster.h"

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

    // Font 0 — a single-family button font holds no more.
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

    // The glyph index, so a missing glyph shows: index 0 is .notdef.
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
        // Into our own buffer (stride = w) — the non-allocating variant.
        stbtt_MakeGlyphBitmap(info_, out.coverage.data(), w, h, /*stride=*/w, scale, scale, gi);
        out.w    = w;
        out.h    = h;
        out.xoff = x0;
        out.yoff = y0;
    }
    return out;
}

}  // namespace ptshell
