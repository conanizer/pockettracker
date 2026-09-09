#pragma once

// ─── The EFFECTS screen's row geometry ───────────────────────────────────────────────────────────
//
// The ONE table the cursor walks and the module draws, in the shape SETTINGS and PROJECT already have
// (ui/settings_row_layout.h) and for the reason those two grew it: a row's PLACE on screen and its
// NUMBER are two different things, and the delay section has now been added to from both ends.
//
// ⚠️ A ROW'S NUMBER IS ITS IDENTITY. `tools/testdata/units/p3-input.txt` records 321 EFFECTS cases by
// row NUMBER, so a row may be APPENDED but never INSERTED — which is why the four delay-character
// rows are 8..11 while they draw among the rows numbered 4..7. EFFECTS_DISPLAY_LINES below is where
// the position is said, and it is the only place.
//
// ⚠️ The delay's eight cells draw in TWO COLUMNS, so a drawn LINE is not a row: three of the delay's
// lines carry a cell on each side. That is why everything below is addressed by line, and why the
// cursor needs a sideways step as well as an up-and-down one.
//
// ⚠️ Unlike SETTINGS and PROJECT, no row here is conditional. The delay's character used to hide TONE
// and WOBL under the types that read them; the cells are all independent now, so they are all always
// there — which is the whole point of them.

namespace pt::ui {

/**
 * The rows, by identity.
 *
 * 0  TYPE    the master bus effect, OTT or DUST
 * 1  SIZE    reverb
 * 2  DAMP    reverb
 * 3  INP EQ  reverb
 * 4  TIME    delay
 * 5  FDBK    delay
 * 6  REV     delay → reverb
 * 7  INP EQ  delay
 * 8  TYPE    delay preset      — APPENDED, draws first in the delay section
 * 9  TONE    delay
 * 10 WOBL    delay
 * 11 PONG    delay
 */
enum class EffectsRow {
    MASTER_TYPE = 0,
    REV_SIZE    = 1,
    REV_DAMP    = 2,
    REV_EQ      = 3,
    DLY_TIME    = 4,
    DLY_FDBK    = 5,
    DLY_REV     = 6,
    DLY_EQ      = 7,
    DLY_TYPE    = 8,
    DLY_TONE    = 9,
    DLY_WOBBLE  = 10,
    DLY_PONG    = 11,
};

inline constexpr int EFFECTS_ROW_COUNT = 12;

/** The three sections, in the order they are drawn. Each gets a blank line and a header above it. */
enum class EffectsSection { MASTER = 0, REVERB = 1, DELAY = 2 };
inline constexpr int EFFECTS_SECTION_COUNT = 3;

constexpr EffectsSection effects_row_section(EffectsRow row) {
    switch (row) {
        case EffectsRow::MASTER_TYPE: return EffectsSection::MASTER;
        case EffectsRow::REV_SIZE:
        case EffectsRow::REV_DAMP:
        case EffectsRow::REV_EQ:      return EffectsSection::REVERB;
        default:                      return EffectsSection::DELAY;
    }
}

/**
 * One DRAWN line: a single cell, or two side by side.
 *
 * ⚠️ An UNPAIRED line answers with the SAME cell in either column, which is what makes both walkers
 * below branchless and total: stepping sideways on a single cell stays put, and coming DOWN the right
 * column onto a single cell lands on it rather than nowhere.
 */
struct EffectsDisplayLine {
    EffectsRow cell[2];
    bool       paired;

    constexpr EffectsDisplayLine(EffectsRow only) : cell{only, only}, paired(false) {}
    constexpr EffectsDisplayLine(EffectsRow left, EffectsRow right)
        : cell{left, right}, paired(true) {}
};

inline constexpr int EFFECTS_LINE_COUNT = 9;

/**
 * The order the rows are DRAWN and the D-pad walks — decoupled from the enum VALUE above, which stays
 * each row's identity.
 *
 * The delay reads as a pair of columns under TYPE: the three cells that shape the repeats themselves
 * on the left, the three that place and colour them on the right, and the two that are neither (the
 * preset, the EQ) across the top and the bottom. TYPE leads because it is the one that writes others.
 */
inline constexpr EffectsDisplayLine EFFECTS_DISPLAY_LINES[EFFECTS_LINE_COUNT] = {
    {EffectsRow::MASTER_TYPE},

    {EffectsRow::REV_SIZE},
    {EffectsRow::REV_DAMP},
    {EffectsRow::REV_EQ},

    {EffectsRow::DLY_TYPE},
    {EffectsRow::DLY_PONG,   EffectsRow::DLY_TIME},
    {EffectsRow::DLY_TONE,   EffectsRow::DLY_FDBK},
    {EffectsRow::DLY_WOBBLE, EffectsRow::DLY_REV},
    {EffectsRow::DLY_EQ},
};

namespace detail {

/**
 * The two things every walker here assumes, checked off the table itself rather than trusted: every
 * row is drawn exactly once, and a paired line's two cells belong to the same section (the header
 * walk reads the section off the LEFT cell alone, so a split pair would draw one of them under the
 * wrong heading).
 */
constexpr bool effects_lines_are_well_formed() {
    int seen[EFFECTS_ROW_COUNT] = {};
    for (int i = 0; i < EFFECTS_LINE_COUNT; ++i) {
        const EffectsDisplayLine& l = EFFECTS_DISPLAY_LINES[i];
        seen[static_cast<int>(l.cell[0])]++;
        if (l.paired) {
            seen[static_cast<int>(l.cell[1])]++;
            if (effects_row_section(l.cell[0]) != effects_row_section(l.cell[1])) return false;
        }
    }
    for (int i = 0; i < EFFECTS_ROW_COUNT; ++i)
        if (seen[i] != 1) return false;
    return true;
}

}  // namespace detail

static_assert(detail::effects_lines_are_well_formed(),
              "EFFECTS_DISPLAY_LINES must draw every row exactly once, and pair only within a section");

/** Where a row sits on screen: which drawn line, and which of that line's two columns. */
struct EffectsCellPos {
    int line;
    int column;
};

inline EffectsCellPos effects_cell_pos(int row) {
    for (int i = 0; i < EFFECTS_LINE_COUNT; ++i) {
        const EffectsDisplayLine& l = EFFECTS_DISPLAY_LINES[i];
        if (static_cast<int>(l.cell[0]) == row) return {i, 0};
        if (l.paired && static_cast<int>(l.cell[1]) == row) return {i, 1};
    }
    return {0, 0};
}

/**
 * Where every row and every header lands, counted in LINES from the screen's title.
 *
 * The screen draws a title, then for each section a blank line, a header, and the section's lines.
 * A paired line's two rows share one line number, because they share one line.
 */
struct EffectsLayout {
    int rowLine[EFFECTS_ROW_COUNT];
    int sectionHeaderLine[EFFECTS_SECTION_COUNT];
    int lineCount;
};

inline EffectsLayout effects_layout() {
    EffectsLayout out{};
    int line    = 0;    // line 0 is the "EFFECTS" title
    int section = -1;
    for (int i = 0; i < EFFECTS_LINE_COUNT; ++i) {
        const EffectsDisplayLine& l          = EFFECTS_DISPLAY_LINES[i];
        const int                 rowSection = static_cast<int>(effects_row_section(l.cell[0]));
        if (rowSection != section) {
            section = rowSection;
            line += 2;                                  // the blank line, then the header
            out.sectionHeaderLine[rowSection] = line;
        }
        line += 1;
        out.rowLine[static_cast<int>(l.cell[0])] = line;
        if (l.paired) out.rowLine[static_cast<int>(l.cell[1])] = line;
    }
    out.lineCount = line + 1;
    return out;
}

/**
 * The next row's VALUE one line up or down (+1 = down, −1 = up), keeping the column it is in. ⚠️ It
 * CLAMPS rather than wrapping, which is what this screen has always done and what the recorded
 * EFFECTS cases expect — unlike SETTINGS and PROJECT, whose rows wrap.
 */
inline int effects_next_row(int from, int delta) {
    const EffectsCellPos at   = effects_cell_pos(from);
    const int            line = at.line + delta;
    if (line < 0 || line >= EFFECTS_LINE_COUNT) return from;   // the clamp
    return static_cast<int>(EFFECTS_DISPLAY_LINES[line].cell[at.column]);
}

/**
 * The row's VALUE one column left or right. It SNAPS, the way SETTINGS' two columns do: there are
 * only ever two, so a step and a snap are the same move. On a single-cell line it stays put.
 */
inline int effects_step_column(int from, int delta) {
    const EffectsCellPos at = effects_cell_pos(from);
    return static_cast<int>(EFFECTS_DISPLAY_LINES[at.line].cell[delta < 0 ? 0 : 1]);
}

}  // namespace pt::ui
