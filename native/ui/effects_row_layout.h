#pragma once

// ─── The EFFECTS screen's row geometry ───────────────────────────────────────────────────────────
//
// The ONE table the cursor walks and the module draws, in the shape SETTINGS and PROJECT already have
// (ui/settings_row_layout.h) and for the reason those two grew it: a row's PLACE on screen and its
// NUMBER are two different things, and the delay section has now been added to from both ends.
//
// ⚠️ A ROW'S NUMBER IS ITS IDENTITY. `tools/testdata/units/p3-input.txt` records 321 EFFECTS cases by
// row NUMBER, so a row may be APPENDED but never INSERTED — which is why the delay-character rows are
// 8..11 and the reverb-character rows 12..15 while both draw among the rows numbered 1..7.
// EFFECTS_DISPLAY_LINES below is where the position is said, and it is the only place.
//
// ⚠️ Both sections draw in TWO COLUMNS, so a drawn LINE is not a row: six of the lines carry a cell on
// each side. That is why everything below is addressed by line, and why the cursor needs a sideways
// step as well as an up-and-down one.
//
// ⚠️⚠️ TWO ROWS HERE ARE CONDITIONAL, AND THEY ARE THE ONLY TWO. DCAY and DENS exist on the second
// reverb algorithm and have no counterpart in the first, so they are hidden when ALGO is OLD rather
// than drawn dead. Everything else on this screen is unconditional — the delay's character used to
// hide TONE and WOBL under the types that read them, and those cells are all independent now.
//
// ⚠️ Hidden rows are SKIPPED, NEVER RENUMBERED, exactly as in settings_row_layout.h: the cursor stores
// a row's VALUE and DCAY is 17 whether it is on screen or not. `effects_layout` and `effects_next_row`
// are the two functions that take the algorithm; `effects_cell_pos` and `effects_step_column` do not,
// because a row's place in the static table above does not depend on what is visible.

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
 * 12 TYPE    reverb preset     — APPENDED, draws first in the reverb section
 * 13 PRE     reverb
 * 14 WIDE    reverb
 * 15 MOD     reverb
 * 16 ALGO    reverb algorithm  — APPENDED, sits beside TYPE
 * 17 DCAY    reverb            — APPENDED, MVERB only
 * 18 DENS    reverb            — APPENDED, MVERB only
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
    REV_TYPE    = 12,
    REV_PRE     = 13,
    REV_WIDE    = 14,
    REV_MOD     = 15,
    REV_ALGO    = 16,
    REV_DECAY   = 17,
    REV_DENSITY = 18,
};

inline constexpr int EFFECTS_ROW_COUNT = 19;

/** The three sections, in the order they are drawn. Each gets a blank line and a header above it. */
enum class EffectsSection { MASTER = 0, REVERB = 1, DELAY = 2 };
inline constexpr int EFFECTS_SECTION_COUNT = 3;

constexpr EffectsSection effects_row_section(EffectsRow row) {
    switch (row) {
        case EffectsRow::MASTER_TYPE: return EffectsSection::MASTER;
        case EffectsRow::REV_SIZE:
        case EffectsRow::REV_DAMP:
        case EffectsRow::REV_EQ:
        case EffectsRow::REV_TYPE:
        case EffectsRow::REV_PRE:
        case EffectsRow::REV_WIDE:
        case EffectsRow::REV_MOD:
        case EffectsRow::REV_ALGO:
        case EffectsRow::REV_DECAY:
        case EffectsRow::REV_DENSITY: return EffectsSection::REVERB;
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

inline constexpr int EFFECTS_LINE_COUNT = 11;

/**
 * The order the rows are DRAWN and the D-pad walks — decoupled from the enum VALUE above, which stays
 * each row's identity.
 *
 * Both sends read as a pair of columns under a TYPE, and mostly the same way round: the cells that
 * shape the effect's own character on the LEFT, the cells that size and colour it on the RIGHT. TYPE
 * leads each because it is the one that writes the others.
 *
 * ⚠️ The reverb's last line is the deliberate exception — INP EQ on the LEFT and MOD on the right —
 * so that both sends' EQ cells sit in the same column, at the foot of the screen. An EQ cell is the
 * only one here that opens another screen rather than holding a value, and it is easier to find when
 * the two of them line up than when each obeys its own section's grouping.
 *
 * ⚠️ ALGO sits beside the reverb's TYPE rather than on a line of its own, which is why the reverb
 * section is still four lines. The two belong together: TYPE picks a set of values and ALGO picks
 * what reads them, and they are the only two cells here that are not a number.
 *
 * ⚠️⚠️ **TEN DRAWN LINES IS EXACTLY WHAT THE PANEL HOLDS, AND THE ELEVENTH SCROLLS IT.** Measured, not
 * estimated: with ALGO on OLD the cursor reaches the delay's INP EQ with the screen still, and with
 * the DCAY line shown it scrolls by one row to get there. That is graceful — the title stays pinned,
 * the clip below keeps the rows off it, and every cell is still reachable — but it is a behaviour the
 * screen did not have before, so **the next line added is not free.** (An earlier version of this
 * comment claimed three lines of headroom. There were none.)
 *
 * A row added to either section costs a LINE only if it has no partner — so the cheap place to add one
 * is beside a cell that is currently alone, and the DELAY's TYPE is the one that is left.
 */
inline constexpr EffectsDisplayLine EFFECTS_DISPLAY_LINES[EFFECTS_LINE_COUNT] = {
    {EffectsRow::MASTER_TYPE},

    {EffectsRow::REV_TYPE, EffectsRow::REV_ALGO},
    {EffectsRow::REV_PRE,  EffectsRow::REV_SIZE},
    {EffectsRow::REV_WIDE, EffectsRow::REV_DAMP},
    {EffectsRow::REV_DECAY, EffectsRow::REV_DENSITY},
    {EffectsRow::REV_EQ,   EffectsRow::REV_MOD},

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

/**
 * Is this row on screen at all?
 *
 * ⚠️ `algo` is the project's `reverbAlgo`. The two MVERB-only cells are the ONLY conditional rows on
 * this screen; everything else answers true whatever is passed.
 */
constexpr bool effects_row_visible(EffectsRow row, int algo) {
    if (row == EffectsRow::REV_DECAY || row == EffectsRow::REV_DENSITY) return algo == 1;
    return true;
}

/** ⚠️ A line is hidden only when BOTH its cells are — the one conditional line pairs two of them. */
constexpr bool effects_line_visible(int line, int algo) {
    const EffectsDisplayLine& l = EFFECTS_DISPLAY_LINES[line];
    if (effects_row_visible(l.cell[0], algo)) return true;
    return l.paired && effects_row_visible(l.cell[1], algo);
}

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

/**
 * ⚠️ A hidden line contributes NOTHING — not its height and not a gap, unlike SETTINGS, whose hidden
 * rows still leave the air before them. There is one conditional line here and it sits in the middle
 * of a block, so a gap left behind would read as a missing row rather than as spacing.
 *
 * ⚠️ A hidden row's `rowLine` stays 0. Nothing should ask, because nothing draws it — but 0 is the
 * title's line, which is inside the panel, so a caller that did ask gets a harmless answer instead of
 * an index off the end.
 */
inline EffectsLayout effects_layout(int algo) {
    EffectsLayout out{};
    int line    = 0;    // line 0 is the "EFFECTS" title
    int section = -1;
    for (int i = 0; i < EFFECTS_LINE_COUNT; ++i) {
        if (!effects_line_visible(i, algo)) continue;
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
 *
 * ⚠️ Hidden lines are stepped OVER, so the walk cannot stop on one and cannot be stopped BY one: with
 * ALGO on OLD, DAMP's line and INP EQ's line are neighbours. ⚠️ Starting FROM a hidden row still
 * moves — a cursor left stranded there by a project load can walk itself out rather than being stuck.
 */
inline int effects_next_row(int from, int delta, int algo) {
    const EffectsCellPos at   = effects_cell_pos(from);
    const int            step = delta < 0 ? -1 : 1;
    for (int line = at.line + step; line >= 0 && line < EFFECTS_LINE_COUNT; line += step) {
        if (!effects_line_visible(line, algo)) continue;
        return static_cast<int>(EFFECTS_DISPLAY_LINES[line].cell[at.column]);
    }
    return from;   // the clamp
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
