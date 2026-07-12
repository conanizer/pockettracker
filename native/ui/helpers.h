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
using songcore::note_name;

/** 1-digit uppercase hex — Kotlin's Int.toHex1(). Masks to the low nibble. */
inline std::string hex1(int v) {
    static const char* H = "0123456789ABCDEF";
    return std::string(1, H[v & 0x0F]);
}

/** Multiply the RGB channels by `factor` (0..1 darker, >1 brighter); alpha preserved. Int.darken(). */
inline Argb darken(Argb c, float factor) {
    auto ch = [&](int shift) {
        const int v = static_cast<int>(static_cast<float>((c >> shift) & 0xFF) * factor);
        return static_cast<Argb>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    return (c & 0xFF000000u) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// ─── Row background ──────────────────────────────────────────────────────────────────────────────

/**
 * The standard row background for every grid editor (phrase, chain, song, table).
 * Priority: playing > selected > cursor > every-4th-row accent > default.
 */
inline Argb row_bg_color(int index, int cursor_row, int playback_row, bool is_playing,
                         bool is_selected, const Theme& t) {
    if (is_playing && index == playback_row) return t.rowPlayback;
    if (is_selected) return t.rowSelection;
    if (index == cursor_row) return t.rowCursor;
    if (index % 4 == 0) return t.rowEvery4th;
    return t.background;
}

// ─── Cells ───────────────────────────────────────────────────────────────────────────────────────

/**
 * One value cell in an editor grid row, with the standard colour priority:
 * cursor > selection > empty > `value_color` (which varies per column: values, params, FX names).
 */
inline void draw_cell(Canvas& c, const std::string& text, int x, int text_y, bool is_cursor,
                      bool is_selected, bool is_empty, Argb value_color, const Theme& t) {
    const Argb color = is_cursor     ? t.textCursor
                       : is_selected ? t.vizWave
                       : is_empty    ? t.textEmpty
                                     : value_color;
    c.draw_text(text, x, text_y, color, CHAR_SPACING, FONT_SCALE);
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
