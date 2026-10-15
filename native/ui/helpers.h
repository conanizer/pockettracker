#pragma once

// The layout grid every screen is built on, and the cell painters the editors share.

#include <cstdint>
#include <string>

#include "canvas.h"
#include "theme.h"
#include "theme_rules.h"   // theme_polarity — the mascot reads the same one the roller does
#include "songcore/effects.h"
#include "songcore/model.h"

namespace pt::ui {

// Text is the 5×5 font at 3×; a row is 15 px of glyph plus 3 px above and below.
inline constexpr int FONT_SCALE   = 3;
inline constexpr int CHAR_SPACING = 2;
inline constexpr int ROW_HEIGHT   = 21;
inline constexpr int TEXT_PADDING = 3;

// One character slot, 17 px.
inline constexpr int CHAR_W = 5 * FONT_SCALE + CHAR_SPACING;

// The 620 px content column is centred in 640; modules are separated by 6 px.
inline constexpr int SCREEN_SPACER = 6;
inline constexpr int SIDE_SPACER   = 10;

using songcore::effect_name;
using songcore::effect_value_max;
using songcore::hex2;

// Uppercase hex, zero-padded to `digits` — never truncated, so a longer value prints in full.
inline std::string hex_padded(int64_t v, size_t digits) {
    static const char* H = "0123456789ABCDEF";
    std::string s;
    for (uint64_t u = static_cast<uint64_t>(v); u != 0; u >>= 4) s.insert(s.begin(), H[u & 0xF]);
    if (s.size() < digits) s.insert(0, digits - s.size(), '0');
    return s;
}
using songcore::note_name;

inline std::string hex1(int v) {
    static const char* H = "0123456789ABCDEF";
    return std::string(1, H[v & 0x0F]);
}

// Two decimal digits — the MIDI channel, 1..16 as devices number it.
inline std::string dec2(int v) {
    std::string s = std::to_string(v);
    return s.size() >= 2 ? s : "0" + s;
}

// Replace the alpha channel, keeping RGB.
inline Argb with_alpha(Argb c, float alpha) {
    const float a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    const Argb  v = static_cast<Argb>(a * 255.0f + 0.5f);   // round
    return (v << 24) | (c & 0x00FFFFFFu);
}

// ─── Modal chrome ────────────────────────────────────────────────────────────────────────────────

// A modal box's border, in the title colour. It grows outward, so a box needs `MODAL_BORDER - 1` px of
// screen beyond its rect.
inline constexpr int MODAL_BORDER = 3;

// A modal box: fill and border. The dim behind it is per modal (`modal_backdrop_active`).
inline void draw_modal_box(Canvas& c, int x, int y, int boxW, int boxH, const Theme& t) {
    c.fill_rect(x, y, boxW, boxH, t.background);
    // Ring 0 is on the box's own rect; the rest step outward.
    for (int r = 0; r < MODAL_BORDER; ++r)
        c.stroke_rect(x - r, y - r, boxW + 2 * r, boxH + 2 * r, t.textTitle);
}

// The full-canvas dim. ⚠️ A modal using it belongs in `modal_backdrop_active` too, or the letterbox
// bars stay bright.
inline void draw_modal_backdrop(Canvas& c) { c.fill_rect(0, 0, DESIGN_W, DESIGN_H, MODAL_BACKDROP); }

// ─── Row background ──────────────────────────────────────────────────────────────────────────────

// A grid row's background: the every-4th accent, else plain. Cursor and selection are per cell
// (`RowCells`), playback a marker per track.
inline Argb row_bg_color(int index, const Theme& t) {
    return (index % 4 == 0) ? t.rowEvery4th : t.background;
}

// ⚠️ The cursor's two inks are opposites: text inside its bar is `background` (the cell inverts), and
// text marking its row or column with no bar is `rowCursor`. Neither is a theme row of its own.
inline Argb cursor_cell_ink(const Theme& t) { return t.background; }
inline Argb cursor_mark_ink(const Theme& t) { return t.rowCursor; }

// Text inside a selected cell.
inline Argb selection_cell_ink(const Theme& t) { return t.rowEvery4th; }

// The mascot's colours. The art is a light figure, so on a light palette it is punched out of a patch
// of TXT TITLE instead. Polarity is `background`'s, never the panel's.
struct MascotInk {
    Argb figure;     // the lit pixels
    Argb backdrop;   // filled behind the sprite first, only when `inverted`
    bool inverted;
};
inline MascotInk mascot_ink(const Theme& t, Argb panel) {
    return (theme_polarity(t) < 0) ? MascotInk{panel, t.textTitle, true}
                                   : MascotInk{t.textTitle, panel, false};
}

// A column header, lit while the cursor is in `lo`..`hi` (an FX header spans two columns).
inline Argb header_color(int cursor_column, int lo, int hi, const Theme& t) {
    return (cursor_column >= lo && cursor_column <= hi) ? cursor_mark_ink(t) : t.textParam;
}

// ─── The playback marker ─────────────────────────────────────────────────────────────────────────

// The playhead: a `>` ahead of the cell, one per track, in `textPlayhead` as set.
inline void draw_playhead(Canvas& c, int x, int text_y, const Theme& t) {
    c.draw_text(">", x, text_y, t.textPlayhead, CHAR_SPACING, FONT_SCALE);
}

// Is a LIVE queue marker lit this frame? Slow: waiting for the chain's end; fast: the next phrase.
inline bool blink_on(int phase_ms, bool fast) {
    const int period = fast ? 200 : 600;
    return (phase_ms % period) * 2 < period;
}

// ─── Cells ───────────────────────────────────────────────────────────────────────────────────────

// A grid row's cells, left to right. The selection's background outranks the cursor's (the cursor is
// inside the block it drags) but the cursor's ink wins. An object so adjacent selected cells fill the
// gutter between them into one block. ⚠️ Cells come in column order, and each column's text is
// fixed-width or the highlight is ragged.
class RowCells {
  public:
    RowCells(Canvas& c, int text_y, const Theme& t) : c_(c), textY_(text_y), t_(t) {}

    void cell(const std::string& text, int x, bool is_cursor, bool is_selected, bool is_empty,
              Argb value_color) {
        const int left  = x - CHAR_SPACING;
        const int right = x + Canvas::text_width(text, CHAR_SPACING, FONT_SCALE) + CHAR_SPACING;

        if (is_cursor || is_selected) {
            // The gutter is claimed only between two selected cells.
            const int from = (is_selected && selectedRight_ >= 0) ? selectedRight_ : left;
            c_.fill_rect(from, textY_ - TEXT_PADDING, right - from, ROW_HEIGHT,
                         is_selected ? t_.rowSelection : t_.rowCursor);
        }

        selectedRight_ = is_selected ? right : -1;

        const Argb color = is_cursor     ? cursor_cell_ink(t_)
                           : is_selected ? selection_cell_ink(t_)
                           : is_empty    ? t_.textEmpty
                                         : value_color;
        c_.draw_text(text, x, textY_, color, CHAR_SPACING, FONT_SCALE);
    }

  private:
    Canvas&      c_;
    int          textY_;
    const Theme& t_;
    int          selectedRight_ = -1;   // −1: the cell to the left is not selected
};

// One cell with no neighbours.
inline void draw_cell(Canvas& c, const std::string& text, int x, int text_y, bool is_cursor,
                      bool is_selected, bool is_empty, Argb value_color, const Theme& t) {
    RowCells{c, text_y, t}.cell(text, x, is_cursor, is_selected, is_empty, value_color);
}

// One cell on a screen with no selection (parameters, buttons), with the grids' cursor.
inline void draw_cursor_cell(Canvas& c, const std::string& text, int x, int text_y, bool is_cursor,
                             Argb color, const Theme& t) {
    draw_cell(c, text, x, text_y, is_cursor, /*is_selected=*/false, /*is_empty=*/false, color, t);
}

// An EQ slot plus the ">" that says it opens the EQ editor (`show_arrow` false hides it).
inline void draw_eq_cell(Canvas& c, int value_x, int text_y, int eq_slot, bool is_cursor,
                         const Theme& t, bool show_arrow = true) {
    const std::string eq_str = (eq_slot < 0) ? "--" : hex2(eq_slot);

    // The cursor spans value and arrow, on the same edges as `RowCells`.
    if (is_cursor) {
        const std::string span = show_arrow ? eq_str + ">" : eq_str;
        const int         w = Canvas::text_width(span, CHAR_SPACING, FONT_SCALE) + 2 * CHAR_SPACING;
        c.fill_rect(value_x - CHAR_SPACING, text_y - TEXT_PADDING, w, ROW_HEIGHT, t.rowCursor);
    }

    const Argb value_color   = is_cursor      ? cursor_cell_ink(t)
                               : (eq_slot < 0) ? t.textEmpty
                                               : t.textValue;
    c.draw_text(eq_str, value_x, text_y, value_color, CHAR_SPACING, FONT_SCALE);
    if (show_arrow) {
        c.draw_text(">", value_x + 2 * CHAR_W, text_y, is_cursor ? cursor_cell_ink(t) : t.textValue,
                    CHAR_SPACING, FONT_SCALE);
    }
}

// "12.4 MB". Integer tenths: `%.1f` prints a comma under some locales.
inline std::string megabytes_str(int64_t bytes) {
    const int64_t tenths = (bytes * 10 + 524288) / 1048576;   // round to nearest tenth of a MiB
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
}

// Clear one FX slot (1..3) of a step.
inline void clear_effect(songcore::PhraseStep& step, int fx_slot) {
    switch (fx_slot) {
        case 1: step.fx1Type = 0x00; step.fx1Value = 0x00; break;
        case 2: step.fx2Type = 0x00; step.fx2Value = 0x00; break;
        case 3: step.fx3Type = 0x00; step.fx3Value = 0x00; break;
        default: break;
    }
}

}  // namespace pt::ui
