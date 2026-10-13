#pragma once

// ─── The EFFECTS screen's row geometry ───────────────────────────────────────────────────────────
//
// The ONE table the cursor walks and the module draws: a row's PLACE on screen and its NUMBER differ.
// ⚠️ A row's NUMBER is its identity — the recorded EFFECTS input cases address rows by number — so rows
// are APPENDED, never inserted. EFFECTS_DISPLAY_LINES is the only place position is stated.
// ⚠️ Both sections draw in TWO COLUMNS, so a drawn LINE is not a row; everything is addressed by line,
// and the cursor needs a sideways step. No row is conditional.
// ⚠️ A send's PRESET line is three cells — the name, SAVE, LOAD — and each is a row of its own: the
// screen has no cursor column, so the row is the only thing that can say which cell it is.

namespace pt::ui {

/**
 * The rows, by identity.
 *
 * 0  TYPE    the master bus effect, OTT or DUST
 * 1  DCAY    reverb, the tail's length
 * 2  DAMP    reverb
 * 3  INP EQ  reverb
 * 4  TIME    delay
 * 5  FDBK    delay
 * 6  REV     delay → reverb
 * 7  INP EQ  delay
 * 8  (name)  delay preset      — APPENDED, draws first in the delay section
 * 9  TONE    delay
 * 10 WOBL    delay
 * 11 PONG    delay
 * 12 (name)  reverb preset     — APPENDED, draws first in the reverb section
 * 13 PRE     reverb
 * 14 WIDE    reverb
 * 15 MOD     reverb
 * 16 SIZE    reverb room       — APPENDED
 * 17 ALGO    reverb algorithm  — APPENDED, sits beside SIZE under the preset line
 * 18 SAVE    reverb preset → a file   — APPENDED, on the preset line
 * 19 LOAD    reverb preset ← a file
 * 20 SAVE    delay preset → a file    — APPENDED, on the preset line
 * 21 LOAD    delay preset ← a file
 */
enum class EffectsRow {
    MASTER_TYPE = 0,
    REV_DECAY   = 1,
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
    REV_SIZE    = 16,
    REV_ALGO    = 17,
    REV_SAVE    = 18,
    REV_LOAD    = 19,
    DLY_SAVE    = 20,
    DLY_LOAD    = 21,
};

inline constexpr int EFFECTS_ROW_COUNT = 22;

/** The three sections, in the order they are drawn. Each gets a blank line and a header above it. */
enum class EffectsSection { MASTER = 0, REVERB = 1, DELAY = 2 };
inline constexpr int EFFECTS_SECTION_COUNT = 3;

constexpr EffectsSection effects_row_section(EffectsRow row) {
    switch (row) {
        case EffectsRow::MASTER_TYPE: return EffectsSection::MASTER;
        case EffectsRow::REV_DECAY:
        case EffectsRow::REV_SIZE:
        case EffectsRow::REV_ALGO:
        case EffectsRow::REV_SAVE:
        case EffectsRow::REV_LOAD:
        case EffectsRow::REV_DAMP:
        case EffectsRow::REV_EQ:
        case EffectsRow::REV_TYPE:
        case EffectsRow::REV_PRE:
        case EffectsRow::REV_WIDE:
        case EffectsRow::REV_MOD:     return EffectsSection::REVERB;
        default:                      return EffectsSection::DELAY;
    }
}

/**
 * One DRAWN line: one cell, two side by side, or a preset line's three.
 * The unused slots repeat the LAST cell, which keeps both walkers branchless: coming down the right
 * column onto a single cell lands on it, and onto a preset line lands on SAVE, the cell drawn there.
 */
struct EffectsDisplayLine {
    EffectsRow cell[3];
    int        count;

    constexpr EffectsDisplayLine(EffectsRow only) : cell{only, only, only}, count(1) {}
    constexpr EffectsDisplayLine(EffectsRow left, EffectsRow right)
        : cell{left, right, right}, count(2) {}
    constexpr EffectsDisplayLine(EffectsRow name, EffectsRow save, EffectsRow load)
        : cell{name, save, load}, count(3) {}
};

inline constexpr int EFFECTS_LINE_COUNT = 11;

/**
 * The order rows are DRAWN and the D-pad walks, independent of the enum VALUE (the identity).
 * Each send is its preset line (which writes the cells under it), then a pair of columns: character on
 * the LEFT, size and colour on the RIGHT.
 * ⚠️ Exception: the reverb's last line puts INP EQ on the LEFT so both sends' EQ cells — which open a
 * screen rather than hold a value — line up at the foot.
 * ⚠️ The panel holds ten lines and this is eleven, so the screen scrolls by one (title pinned). A new
 * row is cheapest beside a cell that is alone (the delay's INP EQ).
 */
inline constexpr EffectsDisplayLine EFFECTS_DISPLAY_LINES[EFFECTS_LINE_COUNT] = {
    {EffectsRow::MASTER_TYPE},

    {EffectsRow::REV_TYPE, EffectsRow::REV_SAVE, EffectsRow::REV_LOAD},
    {EffectsRow::REV_ALGO, EffectsRow::REV_SIZE},
    {EffectsRow::REV_PRE,  EffectsRow::REV_DECAY},
    {EffectsRow::REV_WIDE, EffectsRow::REV_DAMP},
    {EffectsRow::REV_EQ,   EffectsRow::REV_MOD},

    {EffectsRow::DLY_TYPE, EffectsRow::DLY_SAVE, EffectsRow::DLY_LOAD},
    {EffectsRow::DLY_PONG,   EffectsRow::DLY_TIME},
    {EffectsRow::DLY_TONE,   EffectsRow::DLY_FDBK},
    {EffectsRow::DLY_WOBBLE, EffectsRow::DLY_REV},
    {EffectsRow::DLY_EQ},
};

namespace detail {

/**
 * Checked off the table itself: every row drawn exactly once, and a line's cells all in the same
 * section (headers read the section off the LEFT cell).
 */
constexpr bool effects_lines_are_well_formed() {
    int seen[EFFECTS_ROW_COUNT] = {};
    for (int i = 0; i < EFFECTS_LINE_COUNT; ++i) {
        const EffectsDisplayLine& l = EFFECTS_DISPLAY_LINES[i];
        for (int c = 0; c < l.count; ++c) {
            seen[static_cast<int>(l.cell[c])]++;
            if (effects_row_section(l.cell[c]) != effects_row_section(l.cell[0])) return false;
        }
    }
    for (int i = 0; i < EFFECTS_ROW_COUNT; ++i)
        if (seen[i] != 1) return false;
    return true;
}

}  // namespace detail

static_assert(detail::effects_lines_are_well_formed(),
              "EFFECTS_DISPLAY_LINES must draw every row exactly once, and group only within a section");

/** Where a row sits on screen: which drawn line, and which of that line's cells (0 = leftmost). */
struct EffectsCellPos {
    int line;
    int column;
};

inline EffectsCellPos effects_cell_pos(int row) {
    for (int i = 0; i < EFFECTS_LINE_COUNT; ++i) {
        const EffectsDisplayLine& l = EFFECTS_DISPLAY_LINES[i];
        for (int c = 0; c < l.count; ++c)
            if (static_cast<int>(l.cell[c]) == row) return {i, c};
    }
    return {0, 0};
}

/**
 * Where every row and header lands, in LINES from the title: per section a blank line, a header, then
 * its lines. A paired line's two rows share one line number.
 * `only` lays out ONE section by itself (the send editor's box): its name is the title, so its lines
 * start straight under it. ⚠️ The other sections' rows then have no line — do not draw them.
 */
struct EffectsLayout {
    int rowLine[EFFECTS_ROW_COUNT];
    int sectionHeaderLine[EFFECTS_SECTION_COUNT];
    int lineCount;
};

inline EffectsLayout effects_layout(int only = -1) {
    EffectsLayout out{};
    int line    = 0;    // line 0 is the title
    int section = -1;
    for (int i = 0; i < EFFECTS_LINE_COUNT; ++i) {
        const EffectsDisplayLine& l          = EFFECTS_DISPLAY_LINES[i];
        const int                 rowSection = static_cast<int>(effects_row_section(l.cell[0]));
        if (only >= 0 && rowSection != only) continue;
        if (rowSection != section) {
            section = rowSection;
            if (only < 0) line += 2;                    // the blank line, then the header
            out.sectionHeaderLine[rowSection] = line;
        }
        line += 1;
        for (int c = 0; c < l.count; ++c) out.rowLine[static_cast<int>(l.cell[c])] = line;
    }
    out.lineCount = line + 1;
    return out;
}

/** The row one line up or down (+1 down, −1 up), keeping its side: a preset line's SAVE and LOAD both
 *  count as the right column. ⚠️ CLAMPS — the recorded EFFECTS cases expect it, unlike SETTINGS and
 *  PROJECT, which wrap. */
inline int effects_next_row(int from, int delta) {
    const EffectsCellPos at   = effects_cell_pos(from);
    const int            line = at.line + delta;
    if (line < 0 || line >= EFFECTS_LINE_COUNT) return from;   // the clamp
    return static_cast<int>(EFFECTS_DISPLAY_LINES[line].cell[at.column > 0 ? 1 : 0]);
}

/** The same step, stopping at the ends of the row's own SECTION — the send editor shows only one. */
inline int effects_next_row_in_section(int from, int delta) {
    const int to = effects_next_row(from, delta);
    return effects_row_section(static_cast<EffectsRow>(to)) ==
                   effects_row_section(static_cast<EffectsRow>(from))
               ? to
               : from;
}

/** A section's top-left cell — where a cursor entering it starts. */
inline int effects_section_first_row(EffectsSection section) {
    for (int i = 0; i < EFFECTS_LINE_COUNT; ++i)
        if (effects_row_section(EFFECTS_DISPLAY_LINES[i].cell[0]) == section)
            return static_cast<int>(EFFECTS_DISPLAY_LINES[i].cell[0]);
    return 0;
}

/** The row one cell left or right on its line. Stops at the line's ends. */
inline int effects_step_column(int from, int delta) {
    const EffectsCellPos      at = effects_cell_pos(from);
    const EffectsDisplayLine& l  = EFFECTS_DISPLAY_LINES[at.line];
    const int column = at.column + (delta < 0 ? -1 : +1);
    if (column < 0 || column >= l.count) return from;
    return static_cast<int>(l.cell[column]);
}

}  // namespace pt::ui
