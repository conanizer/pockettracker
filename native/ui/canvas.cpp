#include "canvas.h"

#include <algorithm>

namespace pt::ui {

namespace {

// src-over, 8-bit, integer. Compose composites a translucent colour onto the canvas exactly this
// way; the only place the UI needs it is the dialog/overlay backdrop (0xCC000000), but a canvas that
// silently ignored alpha would make that backdrop opaque black and hide the screen behind it.
inline uint32_t blend(uint32_t dst, uint32_t src) {
    const uint32_t sa = (src >> 24) & 0xFF;
    if (sa == 0xFF) return src;
    if (sa == 0) return dst;
    const uint32_t ia = 255 - sa;
    const uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    const uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    // +127 rounds to nearest rather than truncating — the difference is a single LSB, but the
    // backdrop is drawn over the whole screen and a truncating blend darkens it visibly over time
    // if it is ever composited twice.
    const uint32_t r = (sr * sa + dr * ia + 127) / 255;
    const uint32_t g = (sg * sa + dg * ia + 127) / 255;
    const uint32_t b = (sb * sa + db * ia + 127) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

}  // namespace

void Canvas::clear(Argb color) {
    std::fill(px_.begin(), px_.end(), color);
}

void Canvas::set_clip(int x, int y, int w, int h) {
    clipX_ = std::max(0, x);
    clipY_ = std::max(0, y);
    clipW_ = std::max(0, std::min(x + w, DESIGN_W) - clipX_);
    clipH_ = std::max(0, std::min(y + h, DESIGN_H) - clipY_);
}

void Canvas::reset_clip() {
    clipX_ = 0;
    clipY_ = 0;
    clipW_ = DESIGN_W;
    clipH_ = DESIGN_H;
}

void Canvas::blend_px(int x, int y, Argb color) {
    if (x < clipX_ || y < clipY_ || x >= clipX_ + clipW_ || y >= clipY_ + clipH_) return;
    uint32_t& d = px_[static_cast<size_t>(y) * DESIGN_W + x];
    d           = blend(d, color);
}

void Canvas::fill_rect(int x, int y, int w, int h, Argb color) {
    if (w <= 0 || h <= 0) return;

    // Clamp to the clip once, then run the rows — a per-pixel clip test across a full-screen
    // backdrop is 300k branches the rasteriser does not need.
    const int x0 = std::max(x, clipX_);
    const int y0 = std::max(y, clipY_);
    const int x1 = std::min(x + w, clipX_ + clipW_);
    const int y1 = std::min(y + h, clipY_ + clipH_);
    if (x0 >= x1 || y0 >= y1) return;

    const bool opaque = ((color >> 24) & 0xFF) == 0xFF;
    for (int py = y0; py < y1; ++py) {
        uint32_t* row = px_.data() + static_cast<size_t>(py) * DESIGN_W;
        if (opaque) {
            std::fill(row + x0, row + x1, color);
        } else {
            for (int px = x0; px < x1; ++px) row[px] = blend(row[px], color);
        }
    }
}

void Canvas::stroke_rect(int x, int y, int w, int h, Argb color, int thickness) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    const int t = std::min(thickness, std::min(w, h));
    fill_rect(x, y, w, t, color);                  // top
    fill_rect(x, y + h - t, w, t, color);          // bottom
    fill_rect(x, y + t, t, h - 2 * t, color);      // left
    fill_rect(x + w - t, y + t, t, h - 2 * t, color);  // right
}

void Canvas::draw_glyph(const Glyph& g, int x, int y, Argb color, int font_scale) {
    if (font_scale <= 0) return;
    for (int row = 0; row < 5; ++row) {
        const uint8_t bits = g[static_cast<size_t>(row)];
        if (bits == 0) continue;
        for (int col = 0; col < 5; ++col) {
            if ((bits >> (4 - col)) & 1) {
                // One source pixel becomes a font_scale × font_scale block. Nearest-neighbour by
                // construction, which is what keeps the glyphs hard-edged (Compose has to ask for
                // FilterQuality.None to get the same thing out of its atlas blit).
                fill_rect(x + col * font_scale, y + row * font_scale, font_scale, font_scale, color);
            }
        }
    }
}

void Canvas::draw_char(char c, int x, int y, Argb color, int font_scale) {
    draw_glyph(glyph_for(c), x, y, color, font_scale);
}

void Canvas::draw_text(const std::string& text, int x, int y, Argb color, int spacing,
                       int font_scale) {
    int cx = x;
    for (const char c : text) {
        draw_char(c, cx, y, color, font_scale);
        cx += 5 * font_scale + spacing;
    }
}

int Canvas::text_width(const std::string& text, int spacing, int font_scale) {
    if (text.empty()) return 0;
    const int n = static_cast<int>(text.size());
    return n * (5 * font_scale + spacing) - spacing;
}

}  // namespace pt::ui
