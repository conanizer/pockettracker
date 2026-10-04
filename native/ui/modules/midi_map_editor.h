#pragma once

// ─── MIDI MAPPING — the list ─────────────────────────────────────────────────────────────────────
//
// One row per mapping: the controller number, what its destination reads right now, the range it is
// driven across, and the destination itself. Reached by the MIDI screen's MAPPING row; B is the only
// way out, as it is from MIDI.
//
// ⚠️ THE LIST GROWS — IT IS NEVER 128 ROWS: an untouched song carries no mappings, and this screen
// is its headers, the empty line and the ADD row.
//
// The destination is TWO cells — a GROUP (five) and a parameter within it (ten at most) — so the
// longest reach is ten steps, not twenty-eight.
//
// ⭐ A row whose destination has gone (slot cleared, or an id from a newer build) is kept, dimmed,
// and says why.

#include <string>

#include "songcore/midi_map.h"
#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

/**
 * The cursor columns of one mapping row. Column 0 is unreachable on every screen here, and the
 * live value has no column of its own — it is read from the song and cannot be typed.
 *
 * ⚠️⚠️ IN THE ORDER THE ROW IS DRAWN — LEFT/RIGHT step this enum by one, so a member out of place
 * sends the highlight the wrong way and makes a cell unfindable. Nothing stores these numbers.
 *
 * ⚠️ SCOPE exists only where the destination has one; it is SKIPPED there, not renumbered.
 */
enum class MapCol {
    CC    = 1,
    MIN   = 2,
    MAX   = 3,
    GROUP = 4,
    SCOPE = 5,
    PARAM = 6,
};

inline constexpr int MIDI_MAP_COLUMN_COUNT = 6;

/** Does this destination carry a scope number the user can dial? */
inline bool map_scope_editable(songcore::MapScope s) { return s != songcore::MapScope::NONE; }

/**
 * The rightmost cursor column on the mapping at `row`, or 1 on the ADD row — which is a button and
 * has no cells at all.
 */
inline int midi_map_max_column(const songcore::Project& p, int row) {
    if (row < 0 || row >= static_cast<int>(p.midiMappings.size())) return 1;   // the ADD row
    return static_cast<int>(MapCol::PARAM);
}

/** Is the SCOPE cell drawn on this row at all? */
inline bool midi_map_scope_visible(const songcore::Project& p, int row) {
    if (row < 0 || row >= static_cast<int>(p.midiMappings.size())) return false;
    const songcore::MapDest* d = songcore::map_dest(p.midiMappings[static_cast<size_t>(row)].dest);
    return d != nullptr && map_scope_editable(d->scope);
}

/**
 * `column` brought onto a cell this row actually draws — clamped to the row's range, and stepped off
 * the SCOPE cell when this destination has none.
 *
 * ⚠️ `prefer` is which way to leave a hidden SCOPE: it is the direction the cursor was travelling, so
 * a RIGHT that lands on it continues right and a LEFT continues left. Arriving from a vertical move
 * passes +1 and lands on the name, which is the cell that is always there.
 */
inline int midi_map_clamp_column(const songcore::Project& p, int row, int column, int prefer = +1) {
    const int max = midi_map_max_column(p, row);
    if (column < 1) column = 1;
    if (column > max) column = max;
    if (column == static_cast<int>(MapCol::SCOPE) && !midi_map_scope_visible(p, row))
        column = prefer < 0 ? static_cast<int>(MapCol::GROUP) : static_cast<int>(MapCol::PARAM);
    return column;
}

/** How many rows the cursor walks: one per mapping, plus the ADD row at the bottom. */
inline int midi_map_row_count(const songcore::Project& p) {
    return static_cast<int>(p.midiMappings.size()) + 1;
}

struct MidiMapState {
    const songcore::Project& project;

    int cursorRow    = 0;   // 0..mappings.size(); the last one is ADD
    int cursorColumn = 1;   // a MapCol

    Theme theme = theme_classic();
};

struct MidiMapInputResult {
    bool modified   = false;   // the mappings live in the .ptp — an edit here dirties the SONG
    bool rowDeleted = false;   // …and the cursor may now be past the end
};

class MidiMapModule {
  public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const MidiMapState& s) const;

    CursorContext cursor_context(const MidiMapState& s) const;

    /**
     * ⚠️ ADD is not here: a plain-A action that grows the vector the cursor stands in, so the
     * dispatcher, which owns the cursor, performs it.
     */
    MidiMapInputResult handle_input(songcore::Project& project, int cursor_row, int cursor_column,
                                    const InputAction& action) const;
};

}  // namespace pt::ui
