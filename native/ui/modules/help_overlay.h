#pragma once

// The full help overlay: a modal box with the large mascot top-left, the topic's title beside it, the
// text flowing from beside the figure into the full width under it, and ANY BUTTON CLOSES at the foot.
// The box is as tall as its topic (derived from `help_overlay_flow`, never shorter than the mascot).
// ⚠️ It covers cells, so the press that closes it is consumed (button_mapper.h) — or it would edit a
// cell the user could not see. The compact panel is the opposite case.
//
// The text is re-wrapped, not drawn line for line (26 columns beside the mascot, 35 under it):
//   • line1 is the title up to its colon; what follows the colon is a paragraph of its own;
//   • line2/line3, and the body's lines, are prose and flow together;
//   • every controls line starts a row (one gesture each) and wraps only if it must.
// `help_overlay_flow` alone decides where a word goes; a `static_assert` runs it over every topic, so a
// box that would outgrow the canvas fails the build.

#include "ui/canvas.h"
#include "ui/help_text.h"
#include "ui/helpers.h"
#include "ui/mascot_sprite_large.h"
#include "ui/theme.h"

namespace pt::ui {

namespace help_overlay_geometry {

// Every x is on the canvas; every y is an offset from the box's top.

inline constexpr int BOX_W = 620;
inline constexpr int BOX_X = (DESIGN_W - BOX_W) / 2;

inline constexpr int PAD = 12;

inline constexpr int MASCOT_X  = BOX_X + PAD;
inline constexpr int MASCOT_DY = PAD;

// Beside the mascot, past it by the same PAD.
inline constexpr int BESIDE_X    = MASCOT_X + MASCOT_LARGE_W + PAD;
inline constexpr int BESIDE_ROWS = MASCOT_LARGE_H / ROW_HEIGHT;
inline constexpr int BESIDE_COLS = (BOX_X + BOX_W - PAD - BESIDE_X + CHAR_SPACING) / CHAR_W;

// Under the mascot, half a PAD below its feet.
inline constexpr int BELOW_X    = BOX_X + PAD;
inline constexpr int BELOW_DY   = MASCOT_DY + MASCOT_LARGE_H + PAD / 2;
inline constexpr int BELOW_COLS = (BOX_W - 2 * PAD + CHAR_SPACING) / CHAR_W;

// The leftover air between the rows beside the mascot and the first under it goes under the title,
// so the text runs on at one row pitch.
inline constexpr int TITLE_GAP = BELOW_DY - (MASCOT_DY + BESIDE_ROWS * ROW_HEIGHT);

constexpr int row_cols(int row) { return row < BESIDE_ROWS ? BESIDE_COLS : BELOW_COLS; }
constexpr int row_x(int row)    { return row < BESIDE_ROWS ? BESIDE_X : BELOW_X; }
constexpr int row_dy(int row) {
    if (row == 0) return MASCOT_DY;
    return row < BESIDE_ROWS ? MASCOT_DY + TITLE_GAP + row * ROW_HEIGHT
                             : BELOW_DY + (row - BESIDE_ROWS) * ROW_HEIGHT;
}

// The box for `rows` text rows (title counted): down to the last row or the mascot's feet, a blank
// row, the hint, the bottom PAD.
constexpr int box_h(int rows) {
    const int textBottom   = rows > 0 ? row_dy(rows - 1) + ROW_HEIGHT : 0;
    const int mascotBottom = MASCOT_DY + MASCOT_LARGE_H;
    const int contentEnd   = textBottom > mascotBottom ? textBottom : mascotBottom;
    return contentEnd + ROW_HEIGHT + ROW_HEIGHT + PAD;
}
constexpr int box_y(int boxH) { return (DESIGN_H - boxH) / 2; }
constexpr int hint_dy(int boxH) { return boxH - PAD - ROW_HEIGHT; }

// The tallest box whose frame stays on the canvas.
inline constexpr int MAX_BOX_H = DESIGN_H - 2 * (MODAL_BORDER - 1);

static_assert(TITLE_GAP >= 0, "the rows beside the help mascot run into the rows under it");

static_assert(BOX_X >= MODAL_BORDER - 1, "the help overlay's frame would draw off the canvas");
// ⚠️ Pinned both ways: moving the mascot must not let a title hit the frame, nor waste room.
static_assert(HELP_TITLE_MAX_CHARS == BESIDE_COLS,
              "HELP_TITLE_MAX_CHARS is no longer the room beside the large mascot");

}  // namespace help_overlay_geometry

// Walks `e`'s text as laid out, calling `emit(row, col, word, bytes, is_key)` per word; returns the
// rows used (title counted), or a huge number if a word fits no row. Columns are code points.
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

    // ⚠️ A null line is a blank row only between written ones; trailing nulls are a shorter list.
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

    // The controls: one gesture a row, never beside the mascot (26 columns would split most).
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
    // The backdrop, the box and `topic`; the caller decides whether it is up.
    void draw(Canvas& c, HelpTopic topic, const Theme& t) const;
};

}  // namespace pt::ui
