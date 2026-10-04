#pragma once

// ─── Shared editor helpers ───────────────────────────────────────────────────────────────────────
//
// The layout constants every screen is built on, the row background, and the cell painters the
// editors share so they cannot drift apart. Hex formatters and effect names live in songcore
// (`hex2`, `effect_name`, `note_name`) — they are the data model's own vocabulary.

#include <cstdint>
#include <string>

#include "canvas.h"
#include "theme.h"
#include "theme_rules.h"   // theme_polarity — the mascot reads the same one the roller does
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

// The 620 px content column is centred in 640; modules are separated by 6 px.
inline constexpr int SCREEN_SPACER = 6;
inline constexpr int SIDE_SPACER   = 10;

using songcore::effect_name;
using songcore::effect_value_max;
using songcore::hex2;

/** Uppercase, zero-padded to eight digits — the SAMPLE EDITOR's frame positions. Pads but never
 *  truncates, so a longer value prints nine digits rather than the wrong eight. */
inline std::string hex8(int64_t v) {
    static const char* H = "0123456789ABCDEF";
    if (v == 0) return "00000000";
    std::string s;
    for (uint64_t u = static_cast<uint64_t>(v); u != 0; u >>= 4) s.insert(s.begin(), H[u & 0xF]);
    while (s.size() < 8) s.insert(s.begin(), '0');
    return s;
}
using songcore::note_name;

/** 1-digit uppercase hex of the low nibble. */
inline std::string hex1(int v) {
    static const char* H = "0123456789ABCDEF";
    return std::string(1, H[v & 0x0F]);
}

/** 4-digit uppercase hex — the EXTERNAL instrument's 14-bit BANK (0..16383), which `hex2` would mask.
 *  Pads but never truncates. */
inline std::string hex4(int v) {
    static const char* H = "0123456789ABCDEF";
    if (v == 0) return "0000";
    std::string s;
    for (unsigned u = static_cast<unsigned>(v); u != 0; u >>= 4) s.insert(s.begin(), H[u & 0xF]);
    while (s.size() < 4) s.insert(s.begin(), '0');
    return s;
}

/** 2-digit zero-padded DECIMAL — the MIDI channel, shown 1..16 as every device and manual numbers it.
 *  The one non-hex number on these screens. */
inline std::string dec2(int v) {
    std::string s = std::to_string(v);
    return s.size() >= 2 ? s : "0" + s;
}

// `darken` is in theme.h (a theme derives its fallbacks with it), included above.

/** Replace the alpha channel, keeping RGB — e.g. the sample editor's 10% slice highlight over the
 *  waveform (`fill_rect` blends src-over). */
inline Argb with_alpha(Argb c, float alpha) {
    const float a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    const Argb  v = static_cast<Argb>(a * 255.0f + 0.5f);   // round
    return (v << 24) | (c & 0x00FFFFFFu);
}

// ─── Modal chrome ────────────────────────────────────────────────────────────────────────────────

/**
 * The border every modal box wears: `MODAL_BORDER` px in the title colour.
 * ⚠️ It GROWS OUTWARD from the box — nothing inside moves — so a box needs `MODAL_BORDER - 1` px of
 * screen beyond its rect on every side.
 */
inline constexpr int MODAL_BORDER = 3;

/**
 * A modal box: the fill and the border — one look, one function.
 * ⚠️ The screen-wide dim is NOT here: whether a modal dims is per modal (`modal_backdrop_active`,
 * ui/app_state.h, which the shell also reads); the sample editor's confirm covers instead.
 */
inline void draw_modal_box(Canvas& c, int x, int y, int boxW, int boxH, const Theme& t) {
    c.fill_rect(x, y, boxW, boxH, t.background);
    // Ring 0 is on the box's own rect; the rest step outward.
    for (int r = 0; r < MODAL_BORDER; ++r)
        c.stroke_rect(x - r, y - r, boxW + 2 * r, boxH + 2 * r, t.textTitle);
}

/** The full-canvas dim under a modal that has one. ⚠️ Add the modal to `modal_backdrop_active` too, or
 *  the letterbox bars stay bright and the scrim stops at the 4:3 edge. */
inline void draw_modal_backdrop(Canvas& c) { c.fill_rect(0, 0, DESIGN_W, DESIGN_H, MODAL_BACKDROP); }

// ─── Row background ──────────────────────────────────────────────────────────────────────────────

/**
 * The standard row background for the grid editors: every-4th-row accent, else default.
 * ⚠️ Nothing that picks CELLS is a row colour here — not the cursor, playback or the selection: a row
 * holds up to ten cells, and a selection is a rectangle starting at column 1. Cursor and selection are
 * per-cell backgrounds (`RowCells`); what plays is a marker per track (`draw_playhead`).
 */
inline Argb row_bg_color(int index, const Theme& t) {
    return (index % 4 == 0) ? t.rowEvery4th : t.background;
}

// ─── The two cursor inks ─────────────────────────────────────────────────────────────────────────
//
// ⚠️⚠️ "The cursor colour" is TWO roles taking OPPOSITE colours:
//   * `cursor_cell_ink` — text INSIDE the cursor's bar (`rowCursor`), so it is `background`: the cell
//     inverts.
//   * `cursor_mark_ink` — text saying which row or column the cursor is in, with no bar: `rowCursor`
//     itself, so one accent says "you are here" everywhere.
// Neither is a theme-editor row: derived from the ground beside them, the pair cannot be set to
// something invisible, and `rowCursor` moves every cursor at once.

/** Text inside the cursor's bar. See above. */
inline Argb cursor_cell_ink(const Theme& t) { return t.background; }

/** Text marking the cursor's row or column, with no bar behind it. See above. */
inline Argb cursor_mark_ink(const Theme& t) { return t.rowCursor; }

/** Text inside a selected cell — `rowSelection` is the bar, `rowEvery4th` reads on it. */
inline Argb selection_cell_ink(const Theme& t) { return t.rowEvery4th; }

/**
 * The mascot's two colours. ⚠️ The art is a LIGHT figure; on a light palette it would read as a
 * negative, so the pair swaps and the figure is punched out of a patch of TXT TITLE.
 * ⚠️ Polarity is `background`'s, never the panel's — the two mascots stand on different panels.
 */
struct MascotInk {
    Argb figure;     ///< the colour the lit pixels take
    Argb backdrop;   ///< filled behind the sprite FIRST, and only when `inverted`
    bool inverted;
};
inline MascotInk mascot_ink(const Theme& t, Argb panel) {
    return (theme_polarity(t) < 0) ? MascotInk{panel, t.textTitle, true}
                                   : MascotInk{t.textTitle, panel, false};
}

/** A grid column header — how the cursor shows WHICH COLUMN. `lo`..`hi` because an FX header spans
 *  two cursor columns (name and value). */
inline Argb header_color(int cursor_column, int lo, int hi, const Theme& t) {
    return (cursor_column >= lo && cursor_column <= hi) ? cursor_mark_ink(t) : t.textParam;
}

// ─── The playback marker ─────────────────────────────────────────────────────────────────────────

/**
 * The playhead: a `>` in the gap ahead of the cell, one per track; `x` is the marker's own column.
 * ⚠️ Drawn in `textPlayhead` AS TYPED — the legibility lift is a default (`derive_borrowed_colors`), so
 * TXT PLAY set to the selection colour really does hide the marker inside a selection.
 */
inline void draw_playhead(Canvas& c, int x, int text_y, const Theme& t) {
    c.draw_text(">", x, text_y, t.textPlayhead, CHAR_SPACING, FONT_SCALE);
}

/**
 * Is a LIVE queue marker lit this frame? SLOW = waiting for its chain to end, FAST = the next phrase
 * boundary — the two quantizations told apart by blink rate. `phase_ms` is handed in, so screenshots
 * are reproducible.
 */
inline bool blink_on(int phase_ms, bool fast) {
    const int period = fast ? 200 : 600;
    return (phase_ms % period) * 2 < period;
}

// ─── Cells ───────────────────────────────────────────────────────────────────────────────────────
//
// ⚠️ A cursor is `rowCursor` behind and `background` in front, everywhere. A shade mixed from the
// accent answers to no theme row, so setting ROW CURSOR would change every grid but that cell.

/**
 * A grid row's cells, painted left to right: cursor > selection > empty > `value_color`.
 *
 * ⚠️ The SELECTION outranks the cursor in the BACKGROUND but loses to it in the TEXT: the cursor is
 * always inside the selection it drags, so a cursor background would punch a hole in the block. Hence
 * the two inks are a shade apart, not opposites.
 * ⚠️ An object because a selection is ONE block: the gutter between two selected cells is filled by
 * starting a cell's fill at its left neighbour's right edge, which only something that drew the left
 * one knows. ⚠️ Cells must come in COLUMN ORDER.
 * The width is derived from the text: every grid column is fixed-width in glyphs (empty cells print
 * `--` / `---`). ⚠️ A column whose text could change length would highlight raggedly.
 * The margin is CHAR_SPACING, so the edge lands where the next glyph would begin and clears the
 * playhead one CHAR_W to the left.
 */
class RowCells {
  public:
    RowCells(Canvas& c, int text_y, const Theme& t) : c_(c), textY_(text_y), t_(t) {}

    void cell(const std::string& text, int x, bool is_cursor, bool is_selected, bool is_empty,
              Argb value_color) {
        const int left  = x - CHAR_SPACING;
        const int right = x + Canvas::text_width(text, CHAR_SPACING, FONT_SCALE) + CHAR_SPACING;

        if (is_cursor || is_selected) {
            // The gutter is claimed only when the cells on BOTH sides are selected.
            const int from = (is_selected && selectedRight_ >= 0) ? selectedRight_ : left;
            c_.fill_rect(from, textY_ - TEXT_PADDING, right - from, ROW_HEIGHT,
                         is_selected ? t_.rowSelection : t_.rowCursor);
        }

        // −1 = nothing selected on my left; an unselected cell resets it so the next block starts fresh.
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
    int          selectedRight_ = -1;
};

/** One cell with no neighbours — a grid with no selection (GROOVE), or a row's gutter. */
inline void draw_cell(Canvas& c, const std::string& text, int x, int text_y, bool is_cursor,
                      bool is_selected, bool is_empty, Argb value_color, const Theme& t) {
    RowCells{c, text_y, t}.cell(text, x, is_cursor, is_selected, is_empty, value_color);
}

/**
 * One cell on a screen with no selection — parameter screens and buttons — with the grids' cursor.
 * `color` is the resting colour the cursor's ink replaces. These screens mark the cursor's row on its
 * LABEL (`cursor_mark_ink`), which is not a cell unless the cursor can land on it.
 */
inline void draw_cursor_cell(Canvas& c, const std::string& text, int x, int text_y, bool is_cursor,
                             Argb color, const Theme& t) {
    draw_cell(c, text, x, text_y, is_cursor, /*is_selected=*/false, /*is_empty=*/false, color, t);
}

/** An EQ slot ("--" or hex) plus the ">" that says the cell opens the EQ editor. The ">" never dims;
 *  `show_arrow=false` hides it (the pool, on unselected rows). */
inline void draw_eq_cell(Canvas& c, int value_x, int text_y, int eq_slot, bool is_cursor,
                         const Theme& t, bool show_arrow = true) {
    const std::string eq_str = (eq_slot < 0) ? "--" : hex2(eq_slot);

    // ⚠️ The cursor background spans value AND arrow while they keep separate colours — a fill of its
    // own, measured on the same edges as `RowCells`, so EQ and grid cells match.
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

/** A byte count as "12.4 MB" (USED RAM on PROJECT and INST.POOL). Integer tenths, never `%.1f`, which
 *  prints a comma under some locales. */
inline std::string megabytes_str(int64_t bytes) {
    const int64_t tenths = (bytes * 10 + 524288) / 1048576;   // round to nearest tenth of a MiB
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
}

// ─── Clears ──────────────────────────────────────────────────────────────────────────────────────

/** Clear one FX slot (1..3) of a step. */
inline void clear_effect(songcore::PhraseStep& step, int fx_slot) {
    switch (fx_slot) {
        case 1: step.fx1Type = 0x00; step.fx1Value = 0x00; break;
        case 2: step.fx2Type = 0x00; step.fx2Value = 0x00; break;
        case 3: step.fx3Type = 0x00; step.fx3Value = 0x00; break;
        default: break;
    }
}

}  // namespace pt::ui
