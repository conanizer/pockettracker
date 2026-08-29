#pragma once

// ─── Shared editor helpers ───────────────────────────────────────────────────────────────────────
//
// A 1:1 port of ui/EditorHelpers.kt: the layout constants every screen is built on, the row
// background priority, and the two cell painters (a grid cell, an EQ cell) that the editors share so
// they cannot drift apart from each other.
//
// The hex formatters and the effect names are NOT re-implemented here — songcore already carries
// them (`songcore::hex2`, `effect_name`, `effect_value_max`, `note_name`), because they are the
// data model's own vocabulary and the sequencer speaks it too. Kotlin has the same arrangement:
// `EditorHelpers.getEffectTypeName()` is a one-line alias for `EffectProcessor.effectName()`, "so the
// code↔name map lives in core and can't drift from the effect codes".

#include <cstdint>
#include <string>

#include "canvas.h"
#include "theme.h"
#include "songcore/effects.h"
#include "songcore/model.h"

namespace pt::ui {

// ─── Layout constants ────────────────────────────────────────────────────────────────────────────
// Text is the 5×5 font at 3× (15×15 px). A row is 21 px: 15 px of glyph + 3 px of padding above and
// below. Every screen in the app is laid out on this grid.
inline constexpr int FONT_SCALE   = 3;
inline constexpr int CHAR_SPACING = 2;
inline constexpr int ROW_HEIGHT   = 21;
inline constexpr int TEXT_PADDING = 3;

/** Width of one character slot as the editors advance it: 5*3 + 2 = 17 px. */
inline constexpr int CHAR_W = 5 * FONT_SCALE + CHAR_SPACING;

// Layout spacers (PixelPerfectRenderer): the 620px content column is centred in 640, modules are
// separated by 6px.
inline constexpr int SCREEN_SPACER = 6;
inline constexpr int SIDE_SPACER   = 10;

using songcore::effect_name;
using songcore::effect_value_max;
using songcore::hex2;

/**
 * Kotlin's `Long.toHex8()` — uppercase, zero-padded to eight digits. The SAMPLE EDITOR's frame
 * positions, which are the only values in the app too big for a hex byte.
 *
 * It PADS but never truncates, exactly as `padStart(8, '0')` does: a sample longer than 0xFFFFFFFF
 * frames (27 hours at 44.1 kHz) would print nine digits rather than the wrong eight.
 */
inline std::string hex8(int64_t v) {
    static const char* H = "0123456789ABCDEF";
    if (v == 0) return "00000000";
    std::string s;
    for (uint64_t u = static_cast<uint64_t>(v); u != 0; u >>= 4) s.insert(s.begin(), H[u & 0xF]);
    while (s.size() < 8) s.insert(s.begin(), '0');
    return s;
}
using songcore::note_name;

/** 1-digit uppercase hex — Kotlin's Int.toHex1(). Masks to the low nibble. */
inline std::string hex1(int v) {
    static const char* H = "0123456789ABCDEF";
    return std::string(1, H[v & 0x0F]);
}

/**
 * 4-digit uppercase hex — the EXTERNAL instrument's BANK, and nothing else.
 *
 * A bank select is 14 bits (CC0 MSB + CC32 LSB), so 0..16383, and `hex2` would have MASKED it: a bank
 * of 1024 drew as `00` in the first render of that screen. Pads but never truncates, as hex8 does.
 */
inline std::string hex4(int v) {
    static const char* H = "0123456789ABCDEF";
    if (v == 0) return "0000";
    std::string s;
    for (unsigned u = static_cast<unsigned>(v); u != 0; u >>= 4) s.insert(s.begin(), H[u & 0xF]);
    while (s.size() < 4) s.insert(s.begin(), '0');
    return s;
}

/**
 * 2-digit zero-padded DECIMAL — the MIDI channel on the EXTERNAL instrument, and nothing else.
 *
 * Every other number on these screens is hex, and that is deliberate: they are bytes. A MIDI channel
 * is not a byte the user thinks in — every device, cable and manual in the world calls it 1 to 16 —
 * so it is the one cell shown in the base its own world uses. Pads but never truncates.
 */
inline std::string dec2(int v) {
    std::string s = std::to_string(v);
    return s.size() >= 2 ? s : "0" + s;
}

// `darken` lives in theme.h — a theme's own fallback colours are derived with it, and theme.h cannot
// include this file. It is reachable from here, as it always was, because this file includes that one.

/**
 * REPLACE the alpha channel, keeping the RGB — Compose's `Color(x).copy(alpha = a)`, which the sample
 * editor's slice highlight uses to lay 10% of the cursor colour over the waveform without hiding it.
 * `Canvas::fill_rect` blends src-over, so a translucent fill is a real blend rather than a flat wash.
 */
inline Argb with_alpha(Argb c, float alpha) {
    const float a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    const Argb  v = static_cast<Argb>(a * 255.0f + 0.5f);   // round, as Compose does
    return (v << 24) | (c & 0x00FFFFFFu);
}

// ─── Row background ──────────────────────────────────────────────────────────────────────────────

/**
 * The standard row background for every grid editor (phrase, chain, song, table).
 * Priority: every-4th-row accent > default.
 *
 * ⚠️ NOTHING THAT PICKS CELLS IS A ROW HERE — not the cursor, not playback, and not the selection —
 * all for the same reason: a grid row holds up to ten cells and a row-wide colour cannot say which
 * of them is meant. A selection is a RECTANGLE, and it starts at column 1: painting its rows edge to
 * edge claims both the columns outside it and the row-number gutter, which no selection can reach.
 * So the cursor and the selection are backgrounds under the cells themselves (`RowCells`), and what
 * plays is a marker per track (`draw_playhead`), which can be absent, and can be in eight places at
 * once.
 */
inline Argb row_bg_color(int index, const Theme& t) {
    return (index % 4 == 0) ? t.rowEvery4th : t.background;
}

/**
 * A grid's column header, which is how the cursor says WHICH COLUMN now that it no longer paints a
 * whole row. `lo`..`hi` is a range because an FX header stands over two cursor columns: a name and
 * its value are one column to the reader and two to the cursor.
 */
inline Argb header_color(int cursor_column, int lo, int hi, const Theme& t) {
    return (cursor_column >= lo && cursor_column <= hi) ? t.textCursor : t.textParam;
}

// ─── The playback marker ─────────────────────────────────────────────────────────────────────────

/**
 * LGPT's and M8's playhead: a `>` in the gap ahead of the cell, one per track. It is one glyph wide
 * (CHAR_W), so `x` is the marker's own column and never the cell's.
 *
 * ⚠️ THE COLOUR IS `textPlayhead` AS TYPED. The lift that makes an old theme's ROW PLAY readable as
 * ink is a DEFAULT (`derive_borrowed_colors`), not a step in the drawing — so TXT PLAY set to the
 * selection colour really does hide the marker inside a selection.
 */
inline void draw_playhead(Canvas& c, int x, int text_y, const Theme& t) {
    c.draw_text(">", x, text_y, t.textPlayhead, CHAR_SPACING, FONT_SCALE);
}

/**
 * Is a LIVE queue marker lit on this frame's phase? SLOW is a queue waiting for its chain to end,
 * FAST one that lands on the next phrase boundary — so the two launch quantizations are told apart
 * by the RATE of the same marker rather than by a second glyph.
 *
 * ⚠️ `phase_ms` is handed in (ui/app_state.h): a drawing layer with no clock is what makes a blinking
 * marker reproducible in a screenshot.
 */
inline bool blink_on(int phase_ms, bool fast) {
    const int period = fast ? 200 : 600;
    return (phase_ms % period) * 2 < period;
}

// ─── Cells ───────────────────────────────────────────────────────────────────────────────────────

/**
 * A grid row's cells, painted left to right, with the standard colour priority per cell:
 * cursor > selection > empty > `value_color` (which varies per column: values, params, FX names).
 *
 * ⚠️ BOTH THE CURSOR'S BACKGROUND AND THE SELECTION'S ARE PAINTED HERE, per cell — `row_bg_color`
 * has neither.
 *
 * ⚠️ AND THE SELECTION OUTRANKS THE CURSOR IN THE BACKGROUND WHILE LOSING TO IT IN THE TEXT. That is
 * not a contradiction: the cursor is always inside the selection it is dragging, so a cursor
 * background would punch a hole in the one region the screen is trying to show as a block. The
 * region stays one unbroken colour and the cursor says where its edge is with its INK alone — which
 * means `rowCursor` is a colour seen only when no selection is up.
 *
 * ⚠️ AND A SELECTION IS ONE BLOCK, WHICH IS WHY THIS IS AN OBJECT AND NOT A FREE FUNCTION. The
 * gutter between two columns belongs to neither cell, and its width is not something a call site
 * could pass: it is the DISTANCE between two cells, and only something that has already drawn the
 * left one knows it. So a row hands its cells over in COLUMN ORDER and each selected cell whose
 * left neighbour was also selected begins its fill at that neighbour's right edge — a selected
 * range reads as a rectangle rather than as a line of separate chips, and it still stops at the
 * columns the selection actually covers. ⚠️ HANDED OVER OUT OF ORDER, the fill either runs backwards
 * over a cell already drawn or leaves the gap it was meant to close; every grid draws left to right.
 *
 * The width is DERIVED from the text rather than passed in, and that is exact rather than
 * approximate: every column of every grid is fixed-width in GLYPHS, because an empty cell prints a
 * placeholder the same length as a full one (`--`, `---`). So the derived width is uniform down a
 * column without a tenth argument at two dozen call sites, and it is the width of the string
 * actually drawn. ⚠️ A column whose text could change length would highlight raggedly — that is the
 * constraint this buys the brevity with, and it is the one thing to check before adding one.
 *
 * The horizontal margin is CHAR_SPACING, the font's own inter-glyph gap: the cell edge lands exactly
 * where the next character would have begun, which reads as one character cell and clears the
 * playhead marker sitting one CHAR_W to the left of the cell.
 */
class RowCells {
  public:
    RowCells(Canvas& c, int text_y, const Theme& t) : c_(c), textY_(text_y), t_(t) {}

    void cell(const std::string& text, int x, bool is_cursor, bool is_selected, bool is_empty,
              Argb value_color) {
        const int left  = x - CHAR_SPACING;
        const int right = x + Canvas::text_width(text, CHAR_SPACING, FONT_SCALE) + CHAR_SPACING;

        if (is_cursor || is_selected) {
            // The gutter on the left is claimed only when the cells on BOTH sides of it are selected.
            const int from = (is_selected && selectedRight_ >= 0) ? selectedRight_ : left;
            c_.fill_rect(from, textY_ - TEXT_PADDING, right - from, ROW_HEIGHT,
                         is_selected ? t_.rowSelection : t_.rowCursor);
        }

        // −1 is "nothing selected on my left", and an unselected cell restores it: the next selected
        // cell then opens a fresh block at its own edge instead of reaching back across it.
        selectedRight_ = is_selected ? right : -1;

        const Argb color = is_cursor     ? t_.textCursor
                           : is_selected ? t_.textSelection
                           : is_empty    ? t_.textEmpty
                                         : value_color;
        c_.draw_text(text, x, textY_, color, CHAR_SPACING, FONT_SCALE);
    }

  private:
    Canvas&      c_;
    int          textY_;
    const Theme& t_;
    int          selectedRight_ = -1;
};

/** One cell with no neighbours — a grid with no selection to join up (GROOVE), or a row's gutter. */
inline void draw_cell(Canvas& c, const std::string& text, int x, int text_y, bool is_cursor,
                      bool is_selected, bool is_empty, Argb value_color, const Theme& t) {
    RowCells{c, text_y, t}.cell(text, x, is_cursor, is_selected, is_empty, value_color);
}

/**
 * An EQ slot value ("--" when unassigned, hex when set) plus the trailing ">" that signals the cell
 * opens the EQ editor. Shared by every EQ cell so they all look identical. The ">" never dims with
 * the value; `show_arrow=false` hides it (the instrument pool hides it on non-selected rows).
 */
inline void draw_eq_cell(Canvas& c, int value_x, int text_y, int eq_slot, bool is_cursor,
                         const Theme& t, bool show_arrow = true) {
    const std::string eq_str = (eq_slot < 0) ? "--" : hex2(eq_slot);
    const Argb value_color   = is_cursor      ? t.textCursor
                               : (eq_slot < 0) ? t.textEmpty
                                               : t.textValue;
    c.draw_text(eq_str, value_x, text_y, value_color, CHAR_SPACING, FONT_SCALE);
    if (show_arrow) {
        c.draw_text(">", value_x + 2 * CHAR_W, text_y, is_cursor ? t.textCursor : t.textValue,
                    CHAR_SPACING, FONT_SCALE);
    }
}

/**
 * A byte count as "12.4 MB" — the USED RAM readout, drawn on PROJECT and on INST.POOL.
 *
 * Integer math in tenths, never a float format: `%.1f` would print a comma under a locale that uses
 * one, and this font has no comma-as-decimal-point convention to fall back on. Written once because
 * two screens show the same quantity and a reader comparing them must not find two roundings.
 */
inline std::string megabytes_str(int64_t bytes) {
    const int64_t tenths = (bytes * 10 + 524288) / 1048576;   // round to nearest tenth of a MiB
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
}

// ─── Clears ──────────────────────────────────────────────────────────────────────────────────────

/** Clear one FX slot (1..3) of a step — EditorHelpers.clearEffect(). */
inline void clear_effect(songcore::PhraseStep& step, int fx_slot) {
    switch (fx_slot) {
        case 1: step.fx1Type = 0x00; step.fx1Value = 0x00; break;
        case 2: step.fx2Type = 0x00; step.fx2Value = 0x00; break;
        case 3: step.fx3Type = 0x00; step.fx3Value = 0x00; break;
        default: break;
    }
}

}  // namespace pt::ui
