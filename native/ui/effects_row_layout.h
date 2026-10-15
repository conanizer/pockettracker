#pragma once

// The EFFECTS screen's rows: the one table the cursor walks and the module draws. A row's place on
// screen (EFFECTS_DISPLAY_LINES) and its number differ.
// ⚠️ A row's number is its identity in the recorded input cases: append, never insert.
// Lines hold up to three cells, each a row of its own: the screen has no cursor column.

namespace pt::ui {

enum class EffectsRow {
    MASTER_TYPE = 0,   // OTT or DUST
    REV_DECAY   = 1,
    REV_DAMP    = 2,
    REV_EQ      = 3,
    DLY_TIME    = 4,
    DLY_FDBK    = 5,
    DLY_REV     = 6,   // delay → reverb
    DLY_EQ      = 7,
    DLY_TYPE    = 8,   // the preset's name
    DLY_TONE    = 9,
    DLY_WOBBLE  = 10,
    DLY_PONG    = 11,
    REV_TYPE    = 12,  // the preset's name
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

// In drawn order; each gets a blank line and a header.
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

// One drawn line: one cell, a pair, or a preset line's name, SAVE, LOAD. Unused slots repeat the last
// cell, so coming down the right column lands on what is drawn there.
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

// The drawn order. Each send: its preset line, then character on the left, size and colour on the
// right — except the reverb's INP EQ, on the left to line up with the delay's.
// The panel holds ten lines and this is eleven, so the screen scrolls by one.
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

// Every row drawn once, and a line's cells in one section.
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

// Where a row sits: its drawn line, and which cell of it.
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

// Every row's and header's line, counted from the title. `only` lays out one section alone (the send
// editor's box): the other sections' rows then have no line.
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

// The row one line up or down, keeping its side; SAVE and LOAD count as the right. Clamps.
inline int effects_next_row(int from, int delta) {
    const EffectsCellPos at   = effects_cell_pos(from);
    const int            line = at.line + delta;
    if (line < 0 || line >= EFFECTS_LINE_COUNT) return from;
    return static_cast<int>(EFFECTS_DISPLAY_LINES[line].cell[at.column > 0 ? 1 : 0]);
}

// The same, kept inside the row's section (the send editor).
inline int effects_next_row_in_section(int from, int delta) {
    const int to = effects_next_row(from, delta);
    return effects_row_section(static_cast<EffectsRow>(to)) ==
                   effects_row_section(static_cast<EffectsRow>(from))
               ? to
               : from;
}

// A section's top-left cell.
inline int effects_section_first_row(EffectsSection section) {
    for (int i = 0; i < EFFECTS_LINE_COUNT; ++i)
        if (effects_row_section(EFFECTS_DISPLAY_LINES[i].cell[0]) == section)
            return static_cast<int>(EFFECTS_DISPLAY_LINES[i].cell[0]);
    return 0;
}

// One cell left or right on the line; stops at its ends.
inline int effects_step_column(int from, int delta) {
    const EffectsCellPos      at = effects_cell_pos(from);
    const EffectsDisplayLine& l  = EFFECTS_DISPLAY_LINES[at.line];
    const int column = at.column + (delta < 0 ? -1 : +1);
    if (column < 0 || column >= l.count) return from;
    return static_cast<int>(l.cell[column]);
}

}  // namespace pt::ui
