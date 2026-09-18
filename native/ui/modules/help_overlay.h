#pragma once

// ─── THE HELP OVERLAY — the full one ─────────────────────────────────────────────────────────────
//
// A modal box over the whole frame: the large mascot top-left, the topic's TITLE beside it, and the
// text flowing from beside the figure into the full width under it — what the thing is (the topic's
// body, or the compact panel's summary when it has none), then its controls under the mascot. ANY
// BUTTON CLOSES sits centred on the bottom row.
//
// ⚠️ **THE BOX IS AS TALL AS ITS TOPIC, AND NO TOPIC SAYS HOW TALL.** The height is derived from the
// rows `help_overlay_flow` uses — never shorter than the mascot — plus one blank row and the hint, and
// the box is centred on the canvas. A topic's text is edited, never its layout.
//
// ⚠️⚠️ **IT IS A MODAL, WHICH THE COMPACT PANEL IS NOT, AND ITS SAFETY RESTS ON THE OPPOSITE FACT.**
// The compact panel stands in a box that holds no cell, so it consumes nothing. This box covers cells,
// so the press that closes it is CONSUMED (ui/button_mapper.h) — otherwise a press meant to put help
// away would go on to edit a cell the user could not see.
//
// ⚠️ **THE TEXT IS RE-WRAPPED, NOT DRAWN LINE FOR LINE.** Beside the mascot a row holds 26 characters,
// under it 35, and a help line is written to 32 — so the lines are broken into words and flowed.
// Where the source breaks is kept where it means something:
//
//   • line1 is the TITLE up to its colon, and whatever follows the colon is a paragraph of its own —
//     so "NOTE: the pitch of a step" reads NOTE, then THE PITCH OF A STEP beneath it, never NOTE twice;
//   • line2 and line3 are ONE sentence split for the narrow compact panel, so they flow together, and
//     so do the body's lines, which are prose;
//   • every controls line starts a new row — each is one gesture — and wraps only if it has to.
//
// `help_overlay_flow` is the one place that decides where a word goes: the draw walks it, and a
// `static_assert` walks it over the whole table, so a topic whose box would grow past the canvas fails
// the build.
//
// Colours are the other modals': BACKGROUND ground and TXT TITLE frame (`draw_modal_box`), TXT TITLE for
// the mascot and the title, TXT VALUE for the text, TXT EMPTY for the controls and the closing hint.

#include "ui/canvas.h"
#include "ui/help_text.h"
#include "ui/helpers.h"
#include "ui/mascot_sprite_large.h"
#include "ui/theme.h"

namespace pt::ui {

namespace help_overlay_geometry {

// Every x is on the canvas; every y is an OFFSET from the top of the box, whose height is the topic's.

inline constexpr int BOX_W = 620;
inline constexpr int BOX_X = (DESIGN_W - BOX_W) / 2;

/** Air inside the frame, on all four sides. */
inline constexpr int PAD = 12;

inline constexpr int MASCOT_X  = BOX_X + PAD;
inline constexpr int MASCOT_DY = PAD;

/** Beside the mascot: past it by the same PAD the mascot keeps from the frame. */
inline constexpr int BESIDE_X    = MASCOT_X + MASCOT_LARGE_W + PAD;
inline constexpr int BESIDE_ROWS = MASCOT_LARGE_H / ROW_HEIGHT;
inline constexpr int BESIDE_COLS = (BOX_X + BOX_W - PAD - BESIDE_X + CHAR_SPACING) / CHAR_W;

/** Under the mascot: the frame's own padding, and half a PAD of air below the figure's feet. */
inline constexpr int BELOW_X    = BOX_X + PAD;
inline constexpr int BELOW_DY   = MASCOT_DY + MASCOT_LARGE_H + PAD / 2;
inline constexpr int BELOW_COLS = (BOX_W - 2 * PAD + CHAR_SPACING) / CHAR_W;

/**
 * The air left over between the whole rows beside the mascot and the first row under it. It goes
 * under the TITLE instead, so the text beside the figure runs straight on into the text below it at
 * one row pitch, and the title stays level with the top of the figure.
 */
inline constexpr int TITLE_GAP = BELOW_DY - (MASCOT_DY + BESIDE_ROWS * ROW_HEIGHT);

constexpr int row_cols(int row) { return row < BESIDE_ROWS ? BESIDE_COLS : BELOW_COLS; }
constexpr int row_x(int row)    { return row < BESIDE_ROWS ? BESIDE_X : BELOW_X; }
constexpr int row_dy(int row) {
    if (row == 0) return MASCOT_DY;
    return row < BESIDE_ROWS ? MASCOT_DY + TITLE_GAP + row * ROW_HEIGHT
                             : BELOW_DY + (row - BESIDE_ROWS) * ROW_HEIGHT;
}

/**
 * The height of a box whose text uses `rows` rows, the title's counted: down to the lower of the
 * last row and the mascot's feet, one blank row, the hint, the bottom PAD.
 */
constexpr int box_h(int rows) {
    const int textBottom   = rows > 0 ? row_dy(rows - 1) + ROW_HEIGHT : 0;
    const int mascotBottom = MASCOT_DY + MASCOT_LARGE_H;
    const int contentEnd   = textBottom > mascotBottom ? textBottom : mascotBottom;
    return contentEnd + ROW_HEIGHT + ROW_HEIGHT + PAD;
}
constexpr int box_y(int boxH) { return (DESIGN_H - boxH) / 2; }
constexpr int hint_dy(int boxH) { return boxH - PAD - ROW_HEIGHT; }

/** The tallest box the frame can wear without drawing off the canvas. */
inline constexpr int MAX_BOX_H = DESIGN_H - 2 * (MODAL_BORDER - 1);

static_assert(TITLE_GAP >= 0, "the rows beside the help mascot run into the rows under it");

static_assert(BOX_X >= MODAL_BORDER - 1, "the help overlay's frame would draw off the canvas");
// ⚠️ Pinned in BOTH directions, as the compact panel pins HELP_MAX_CHARS: moving the mascot must not
// silently make a title run into the frame, nor leave the budget short of the room there is.
static_assert(HELP_TITLE_MAX_CHARS == BESIDE_COLS,
              "HELP_TITLE_MAX_CHARS is no longer the room beside the large mascot");

}  // namespace help_overlay_geometry

/**
 * Walk `e`'s text as the overlay lays it out, calling `emit(row, col, word, bytes, is_key)` for every
 * word, and return the number of rows used counting the title's — or a number past every box if a
 * single word is wider than its row. Columns are code points: an arrow is three bytes and one column.
 * `is_key` is true for the words of the controls list.
 */
template <class Emit>
constexpr int help_overlay_flow(const HelpEntry& e, Emit&& emit) {
    using namespace help_overlay_geometry;
    constexpr int TOO_WIDE = 1000;

    int  row   = 1;
    int  col   = 0;
    bool isKey = false;

    // Words of `s`, continuing the current row. Returns false on a word no row can hold.
    const auto flow = [&](const char* s) {
        if (s == nullptr) return true;
        int i = 0;
        while (s[i] != '\0') {
            if (s[i] == ' ') { ++i; continue; }
            const int start = i;
            int cols = 0;
            while (s[i] != '\0' && s[i] != ' ') {
                i += detail::help_arrow_at(s + i) ? 3 : 1;
                ++cols;
            }
            if (col > 0 && col + 1 + cols > row_cols(row)) { ++row; col = 0; }
            if (cols > row_cols(row)) return false;
            if (col > 0) ++col;
            emit(row, col, s + start, i - start, isKey);
            col += cols;
        }
        return true;
    };
    const auto end_paragraph = [&] { if (col > 0) { ++row; col = 0; } };

    // ⚠️ A null line is a BLANK ROW, but only between written ones — trailing nulls are simply a shorter
    // list, and must not push anything or count against the box.
    const auto last_written = [](const char* const* lines, int n) {
        int last = -1;
        for (int i = 0; i < n; ++i)
            if (lines[i] != nullptr && lines[i][0] != '\0') last = i;
        return last;
    };

    const int titleLen = help_title_length(e.line1);
    if (!flow(e.line1 + titleLen + (e.line1[titleLen] == ':' ? 1 : 0))) return TOO_WIDE;
    end_paragraph();

    const int lastBody = last_written(e.body, HELP_BODY_LINES);
    if (lastBody < 0) {
        if (!flow(e.line2) || !flow(e.line3)) return TOO_WIDE;
    } else {
        // Prose: the lines run on into one another, as line2 and line3 do.
        for (int i = 0; i <= lastBody; ++i) {
            if (e.body[i] != nullptr && e.body[i][0] != '\0') {
                if (!flow(e.body[i])) return TOO_WIDE;
            } else {
                end_paragraph();
                ++row;
            }
        }
    }
    end_paragraph();

    // The controls: one gesture to a row, never run on — and never beside the mascot, where 26 columns
    // would break most of them in two. A short description leaves air there instead.
    const int lastKey = last_written(e.keys, HELP_KEY_LINES);
    if (lastKey >= 0) {
        ++row;   // one blank row between what it is and how to use it
        if (row < BESIDE_ROWS) row = BESIDE_ROWS;
        isKey = true;
        for (int i = 0; i <= lastKey; ++i) {
            if (!flow(e.keys[i])) return TOO_WIDE;
            if (col > 0) end_paragraph();
            else         ++row;
        }
    }
    return row;
}

namespace detail {

struct HelpFlowNoEmit {
    constexpr void operator()(int, int, const char*, int, bool) const {}
};

constexpr bool help_overlay_fits() {
    for (const HelpEntry& e : HELP_ENTRIES)
        if (help_overlay_geometry::box_h(help_overlay_flow(e, HelpFlowNoEmit{})) >
            help_overlay_geometry::MAX_BOX_H)
            return false;
    return true;
}

}  // namespace detail

static_assert(detail::help_overlay_fits(),
              "a help topic's text, re-wrapped for the full overlay, makes a box taller than the canvas");

class HelpOverlayModule {
  public:
    /** Draw the backdrop, the box and `topic`. The caller decides whether it is up. */
    void draw(Canvas& c, HelpTopic topic, const Theme& t) const;
};

}  // namespace pt::ui
