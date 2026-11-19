#pragma once

// A 640×480 software framebuffer and the primitives the whole UI is drawn with: filled and stroked
// rects, bitmap text, a clip. The shell scales the finished frame onto the display.

#include <cstdint>
#include <string>
#include <vector>

#include "bitmap_font.h"
#include "fonts/pt5x5.h"
#include "fonts/pt5x5base1.h"
#include "theme.h"

namespace pt::ui {

inline constexpr int DESIGN_W = 640;
inline constexpr int DESIGN_H = 480;

// The modal dim; the shell blends the same value over the letterbox bars.
inline constexpr Argb MODAL_BACKDROP = 0xCC000000;

enum class TextSize { NORMAL, SMALL, LARGE, BUTTON };

// How UI words are written: LOAD, load or Load. ⚠️ settings.json stores the number.
enum class TextCase { UPPER = 0, LOWER = 1, TITLE = 2 };

// A font as the UI draws it: the glyphs, and for each text size each font pixel a `scale` square,
// `gap` px after each glyph, and the baseline `baseline` px below the text's top. Every font keeps
// the 5×5's baselines, so a line of text sits where it always did whatever the font.
struct UiFont {
    struct Size {
        int               scale, gap, baseline;
        const BitmapFont* face = nullptr;   // another font's glyphs for this size; null = `glyphs`
    };

    const BitmapFont* glyphs;
    Size              normal;   // every screen
    Size              small;    // the EQ editor's frequency marks
    Size              large;    // the on-screen keyboard's keys
    Size              button;   // the keyboard's ABORT / APPLY

    constexpr Size size(TextSize s) const {
        switch (s) {
            case TextSize::SMALL:  return small;
            case TextSize::LARGE:  return large;
            case TextSize::BUTTON: return button;
            case TextSize::NORMAL: break;
        }
        return normal;
    }
    constexpr const BitmapFont& face(TextSize s) const {
        const BitmapFont* f = size(s).face;
        return f != nullptr ? *f : *glyphs;
    }
};

// The default: the 5×5 capitals with small letters, drawn at full size. The EQ marks borrow the
// small-letterless 5×5 at 2×.
inline constexpr UiFont UI_FONT_PT5X5{&fonts::PT5X5BASE1, {1, 0, 15}, {2, 2, 10, &fonts::PT5X5}, {1, 0, 18}, {1, 0, 15}};

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

    // The font every text call draws in; the 5×5 until set.
    void set_font(const UiFont& font) { font_ = &font; }

    // UTF-8, top-left at (x, y): values, names, help text — as written in Title, every letter capital
    // in UPPER and small in lower. A code point the font lacks draws blank and still advances.
    void text(const std::string& text, int x, int y, Argb color, TextSize size = TextSize::NORMAL);

    // `text` as written in every case: what is being typed, and the keys that type it.
    void text_as_typed(const std::string& text, int x, int y, Argb color, TextSize size = TextSize::NORMAL);

    // How `label` and `cased` write UI words; UPPER until set.
    void set_case(TextCase c) { case_ = c; }

    // A UI word — label, title, button, a value that is a word (ON, SAMPLER) — in the set case.
    void label(const std::string& text, int x, int y, Argb color, TextSize size = TextSize::NORMAL);

    // `text` in the set case, for a line that also holds a value: cased("PHRASE ") + hex2(id).
    // A number or hex value (0A, 7F) stays as written; a lone letter (N, CC A) and a button (A=YES,
    // SEL+B) stay capitals; a word in canvas.cpp's CASE_KEPT is spelt as listed (MIDI, SoundFont).
    std::string cased(const std::string& text) const;

    // No trailing gap — neither the size's own nor the one a face builds into its step, so a cursor
    // box hugs the text the same in every font.
    int text_width(const std::string& text, TextSize size = TextSize::NORMAL) const;

    // A letter's box and its pen step, for fields that put each character in a cursor cell.
    int glyph_width(TextSize size = TextSize::NORMAL) const {
        const BitmapFont& f = font_->face(size);
        const FontGlyph*  m = f.find('M');
        return (m != nullptr ? m->dx + m->w : f.defaultAdvance) * font_->size(size).scale;
    }
    int char_step(TextSize size = TextSize::NORMAL) const {
        return font_->face(size).advance('M') * font_->size(size).scale + font_->size(size).gap;
    }

    static int glyph_count(const std::string& text);   // code points

    // The byte offset where code point `glyph` starts; `text.size()` at or past the end.
    static size_t glyph_offset(const std::string& text, int glyph);

    // At most `max_glyphs` columns, a cut marked with `…` inside the budget. Never splits a code point.
    static std::string clip_text(const std::string& text, int max_glyphs);

    // The same, keeping the end — for paths.
    static std::string clip_text_head(const std::string& text, int max_glyphs);

    // `clip_text`, except that while `scrolls` (the cursor is on it) a name too long for its field
    // slides through it pixel by pixel, pausing at either end, and starts over from the front. The
    // result is as wide as the field; ⚠️ the next `text`/`label` that starts with it draws the whole
    // name shifted inside that width instead. One scrolling name per frame: a second one stays cut.
    std::string fit_text(const std::string& text, int max_glyphs, bool scrolls);

    // The clock scrolling text runs on; set before each frame is drawn. Never set, it stays still.
    void begin_frame(long long now_ms);

    // When the scrolling name next moves, or -1 when nothing scrolls: a frame must be drawn then,
    // though nothing else changed.
    long long scroll_due_ms() const { return scrollDueMs_; }

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
    // `text` in the set case: UPPER and lower change every letter, Title none.
    std::string as_shown(const std::string& text) const;

    // One glyph's box, top-left at (x, y).
    void draw_glyph(const BitmapFont& f, const FontGlyph& g, int x, int y, Argb color, int font_scale);

    std::vector<uint32_t> px_;
    const UiFont*         font_ = &UI_FONT_PT5X5;
    TextCase              case_ = TextCase::UPPER;
    int                   clipX_ = 0, clipY_ = 0, clipW_ = DESIGN_W, clipH_ = DESIGN_H;

    // The scrolling name: which one, since when, and whether this frame drew it.
    // `scrollShown_` is what fit_text returned, waiting for its draw: `scrollText_` instead, moved
    // `scrollPx_` left and cut to `scrollW_`.
    long long   nowMs_ = 0;
    std::string scrollText_;
    long long   scrollStartMs_ = 0;
    bool        scrollDrawn_   = false;
    long long   scrollDueMs_   = -1;
    std::string scrollShown_;
    int         scrollPx_ = 0;
    int         scrollW_  = 0;
};

}  // namespace pt::ui
