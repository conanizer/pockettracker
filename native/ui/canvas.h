#pragma once

// A 640×480 software framebuffer and the primitives the whole UI is drawn with: filled and stroked
// rects, bitmap text, a clip. The shell scales the finished frame onto the display.

#include <cstdint>
#include <string>
#include <vector>

#include "font5x5.h"
#include "theme.h"

namespace pt::ui {

inline constexpr int DESIGN_W = 640;
inline constexpr int DESIGN_H = 480;

// The modal dim; the shell blends the same value over the letterbox bars.
inline constexpr Argb MODAL_BACKDROP = 0xCC000000;

enum class TextSize { NORMAL, SMALL, LARGE, BUTTON };

class Canvas {
public:
    Canvas() : px_(static_cast<size_t>(DESIGN_W) * DESIGN_H, 0xFF000000) { reset_clip(); }

    // ── The frame ────────────────────────────────────────────────────────────────────────────────

    // The whole canvas, clip ignored.
    void clear(Argb color = 0xFF000000);

    // ARGB, row-major — what the shell uploads.
    const uint32_t* pixels() const { return px_.data(); }
    int             pitch_bytes() const { return DESIGN_W * 4; }

    // ── Primitives ───────────────────────────────────────────────────────────────────────────────

    // Blends src-over when `color` has alpha < 255.
    void fill_rect(int x, int y, int w, int h, Argb color);

    // An outline inside the bounds.
    void stroke_rect(int x, int y, int w, int h, Argb color, int thickness = 1);

    // UTF-8, top-left at (x, y): values, names, help text. A code point the font lacks draws blank
    // and still advances.
    void text(const std::string& text, int x, int y, Argb color, TextSize size = TextSize::NORMAL);

    // `text`, for what is being typed and the keys that type it.
    void text_as_typed(const std::string& text, int x, int y, Argb color, TextSize size = TextSize::NORMAL) {
        this->text(text, x, y, color, size);
    }

    // A UI word: a label, title, button, or a value that is a word (ON, SAMPLER).
    void label(const std::string& text, int x, int y, Argb color, TextSize size = TextSize::NORMAL) {
        this->text(text, x, y, color, size);
    }

    // A UI word inside a line that also holds a value: cased("PHRASE ") + hex2(id).
    std::string cased(const std::string& text) const { return text; }

    // No trailing gap.
    static int text_width(const std::string& text, TextSize size = TextSize::NORMAL);

    // A letter's box and its pen step, for fields that put each character in a cursor cell.
    static int glyph_width(TextSize size = TextSize::NORMAL);
    static int char_step(TextSize size = TextSize::NORMAL);

    static int glyph_count(const std::string& text);   // code points

    // The byte offset where code point `glyph` starts; `text.size()` at or past the end.
    static size_t glyph_offset(const std::string& text, int glyph);

    // At most `max_glyphs` columns, a cut marked with `…` inside the budget. Never splits a code point.
    static std::string clip_text(const std::string& text, int max_glyphs);

    // The same, keeping the end — for paths.
    static std::string clip_text_head(const std::string& text, int max_glyphs);

    // ── Clipping ─────────────────────────────────────────────────────────────────────────────────

    void set_clip(int x, int y, int w, int h);
    void reset_clip();

    // So an early `return` inside a module cannot leak a clip into the next one.
    class ClipScope {
    public:
        ClipScope(Canvas& c, int x, int y, int w, int h)
            : c_(c), cx_(c.clipX_), cy_(c.clipY_), cw_(c.clipW_), ch_(c.clipH_) {
            c.set_clip(x, y, w, h);
        }
        ~ClipScope() { c_.set_clip(cx_, cy_, cw_, ch_); }
        ClipScope(const ClipScope&)            = delete;
        ClipScope& operator=(const ClipScope&) = delete;

    private:
        Canvas& c_;
        int     cx_, cy_, cw_, ch_;
    };

private:
    // `font_scale` multiplies the 5×5 cell.
    void draw_glyph(const Glyph& g, int x, int y, Argb color, int font_scale);

    std::vector<uint32_t> px_;
    int                   clipX_ = 0, clipY_ = 0, clipW_ = DESIGN_W, clipH_ = DESIGN_H;
};

}  // namespace pt::ui
