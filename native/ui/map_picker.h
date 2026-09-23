#pragma once

// ─── The mapping DESTINATION picker ──────────────────────────────────────────────────────────────
//
// Opens on A+UP or A+DOWN over a mapping row's GROUP or PARAMETER cell, which A+LEFT/RIGHT already
// cycles — the FX column's bargain exactly: hold A, read, let go on the one you want.
//
//   A + DPAD    move
//   release A   point the mapping at the highlighted destination and close
//
// ─── The accordion ───────────────────────────────────────────────────────────────────────────────
//
// Three sections, one expanded at a time, the rest a heading each — the FX picker's shape, and the
// same rule for which one is open: ⭐ **THE OPEN SECTION IS THE SECTION THE CURSOR IS IN**, so it is
// not a second piece of state that could disagree with the cursor. Stepping off the bottom row of a
// section opens the one below it, off the top row the one above, and both wrap.
//
// ⚠️⚠️ **THE SECTIONS ARE COARSER THAN `MapGroup`, AND THAT IS DELIBERATE RATHER THAN A SECOND
// TABLE.** The ROW's two cells cycle five groups (TRK / MIX / REV / DLY / INS) because a cycle wants
// small steps and because the row prints the group in three characters — `TRK 03 VOL` says which bus
// in a way `GLB 03 VOL` cannot. A picker that shows everything at once wants the opposite: FEW
// headings. So the three here are a COARSENING of the five, derived in `map_picker_section`, not a
// second opinion about where a destination lives.
//
// ⚠️ **AND THE CELLS PRINT THE CATALOGUE'S FULL NAME, not the row's five-character one.** Merging
// REVERB and DELAY puts two cells called `WET` side by side, and TRACK beside MIX puts two called
// `VOL`. The row can afford the short name because the group cell beside it disambiguates; under a
// heading the name carries it alone.
//
// ⚠️ **A GROUP WIDER THAN A ROW ALSO STARTS ONE**, which is why the layout below is rows of cells
// rather than an index into a flat list: under SENDS, a row that ended on `REV MOD` and carried on
// with `DLY TIME` read as one list of thirteen rather than a reverb and a delay.
//
// This header is PURE — no canvas, no theme. The drawing is ui/modules/map_picker_overlay.h.

#include <vector>

#include "songcore/midi_map.h"

namespace pt::ui {

/** Cells across. Three, because the widest name is nine glyphs (`TRACK VOL`) and the box is modal. */
inline constexpr int MAP_PICKER_COLS = 3;

/** The picker's headings, coarser than `MapGroup` — see the note above. */
enum class MapSection { GLOBAL, SENDS, INSTRUMENT };
inline constexpr int MAP_SECTION_COUNT = 3;

inline const char* map_section_name(MapSection s) {
    switch (s) {
        case MapSection::GLOBAL:     return "GLOBAL";
        case MapSection::SENDS:      return "SENDS";
        case MapSection::INSTRUMENT: return "INSTRUMENT";
    }
    return "";
}

/** Which heading a catalogue group reads under. The coarsening, in one place. */
inline MapSection map_picker_section(songcore::MapGroup g) {
    switch (g) {
        case songcore::MapGroup::TRACK:
        case songcore::MapGroup::MASTER:     return MapSection::GLOBAL;
        case songcore::MapGroup::REVERB:
        case songcore::MapGroup::DELAY:      return MapSection::SENDS;
        case songcore::MapGroup::INSTRUMENT: return MapSection::INSTRUMENT;
    }
    return MapSection::GLOBAL;
}

using MapPickerRow = std::vector<const songcore::MapDest*>;

/**
 * One heading's cells, in the catalogue's own order, broken into rows: a row ends when it is full,
 * or when a group that had ROWS OF ITS OWN ends.
 *
 * ⚠️ **A GROUP TOO SMALL TO FILL A ROW DOES NOT BREAK ONE.** Splitting at every group put the single
 * track fader alone on a line and the limiter alone on another — two holes in a list of five. The
 * reverb's seven and the delay's six are the case the break exists for, and they earn it by being
 * more than a row wide. Derived here from the catalogue, so appending a destination lands in the
 * right run with no layout to keep in step with it.
 */
inline const std::vector<MapPickerRow>& map_section_layout(MapSection section) {
    static const std::vector<std::vector<MapPickerRow>> built = [] {
        std::vector<std::vector<MapPickerRow>> all(MAP_SECTION_COUNT);
        for (const songcore::MapDest& d : songcore::MAP_DESTS) {
            std::vector<MapPickerRow>& rows = all[static_cast<size_t>(map_picker_section(d.group))];
            const bool full = !rows.empty() && static_cast<int>(rows.back().size()) >= MAP_PICKER_COLS;
            const songcore::MapGroup prev = rows.empty() ? d.group : rows.back().back()->group;
            const bool breaks = prev != d.group &&
                                songcore::map_group_size(prev) >= MAP_PICKER_COLS;
            if (rows.empty() || full || breaks) rows.emplace_back();
            rows.back().push_back(&d);
        }
        return all;
    }();
    return built[static_cast<size_t>(section)];
}

/** How many destinations read under `s`, whatever shape the rows took. */
inline int map_section_size(MapSection s) {
    int n = 0;
    for (const MapPickerRow& r : map_section_layout(s)) n += static_cast<int>(r.size());
    return n;
}

inline int map_section_rows(MapSection s) {
    return static_cast<int>(map_section_layout(s).size());
}

/** Cells on `row` of `s` — a group's last row is usually short. */
inline int map_section_row_width(MapSection s, int row) {
    const std::vector<MapPickerRow>& rows = map_section_layout(s);
    if (row < 0 || row >= static_cast<int>(rows.size())) return 0;
    return static_cast<int>(rows[static_cast<size_t>(row)].size());
}

/** The tallest section, which is what the box is sized for so it never resizes under the reader. */
inline int map_picker_max_rows() {
    int most = 0;
    for (int i = 0; i < MAP_SECTION_COUNT; ++i) {
        const int r = map_section_rows(static_cast<MapSection>(i));
        if (r > most) most = r;
    }
    return most;
}

struct MapPickerState {
    bool isOpen    = false;
    int  section   = 0;   // a MapSection — the expanded one, and the one the cursor is in
    int  cursorRow = 0;
    int  cursorCol = 0;

    MapSection map_section() const { return static_cast<MapSection>(section); }

    /** The destination under the cursor — what a release of A commits. */
    const songcore::MapDest* selected() const {
        const std::vector<MapPickerRow>& rows = map_section_layout(map_section());
        if (cursorRow < 0 || cursorRow >= static_cast<int>(rows.size())) return nullptr;
        const MapPickerRow& r = rows[static_cast<size_t>(cursorRow)];
        if (cursorCol < 0 || cursorCol >= static_cast<int>(r.size())) return nullptr;
        return r[static_cast<size_t>(cursorCol)];
    }
};

/** Pull the cursor back onto a cell that exists — the row it arrives on may be a short one. */
inline void map_picker_clamp(MapPickerState& s) {
    if (s.section < 0) s.section = 0;
    if (s.section >= MAP_SECTION_COUNT) s.section = MAP_SECTION_COUNT - 1;
    const int rows = map_section_rows(s.map_section());
    if (rows == 0) { s.cursorRow = 0; s.cursorCol = 0; return; }
    if (s.cursorRow < 0) s.cursorRow = 0;
    if (s.cursorRow >= rows) s.cursorRow = rows - 1;
    const int width = map_section_row_width(s.map_section(), s.cursorRow);
    if (s.cursorCol < 0) s.cursorCol = 0;
    if (s.cursorCol >= width) s.cursorCol = width - 1;
}

/** Open with the cursor on `id`. An id this build does not know opens at the top. */
inline MapPickerState map_picker_opened_at(songcore::MapDestId id) {
    MapPickerState s;
    s.isOpen = true;
    if (const songcore::MapDest* self = songcore::map_dest(id)) {
        const MapSection sec = map_picker_section(self->group);
        const std::vector<MapPickerRow>& rows = map_section_layout(sec);
        for (size_t r = 0; r < rows.size(); ++r)
            for (size_t c = 0; c < rows[r].size(); ++c) {
                if (rows[r][c]->id != id) continue;
                s.section   = static_cast<int>(sec);
                s.cursorRow = static_cast<int>(r);
                s.cursorCol = static_cast<int>(c);
            }
    }
    map_picker_clamp(s);
    return s;
}

// ─── Navigation ──────────────────────────────────────────────────────────────────────────────────
//
// LEFT/RIGHT cycle the cursor's own row; UP/DOWN walk the rows, and stepping off either end of a
// section moves into the next — which is what OPENS it, since the open section is the cursor's.

inline void map_picker_move_up(MapPickerState& s) {
    if (s.cursorRow > 0) {
        --s.cursorRow;
    } else {
        s.section   = (s.section + MAP_SECTION_COUNT - 1) % MAP_SECTION_COUNT;
        s.cursorRow = map_section_rows(s.map_section()) - 1;
    }
    map_picker_clamp(s);
}

inline void map_picker_move_down(MapPickerState& s) {
    if (s.cursorRow + 1 < map_section_rows(s.map_section())) {
        ++s.cursorRow;
    } else {
        s.section   = (s.section + 1) % MAP_SECTION_COUNT;
        s.cursorRow = 0;
    }
    map_picker_clamp(s);
}

inline void map_picker_move_left(MapPickerState& s) {
    const int width = map_section_row_width(s.map_section(), s.cursorRow);
    if (width > 0) s.cursorCol = (s.cursorCol + width - 1) % width;
}

inline void map_picker_move_right(MapPickerState& s) {
    const int width = map_section_row_width(s.map_section(), s.cursorRow);
    if (width > 0) s.cursorCol = (s.cursorCol + 1) % width;
}

}  // namespace pt::ui
