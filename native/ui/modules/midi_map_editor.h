#pragma once

// ─── MIDI MAPPING — the list ─────────────────────────────────────────────────────────────────────
//
// One row per mapping: the controller number, what its destination reads right now, the range it is
// driven across, and the destination itself. Reached by the MIDI screen's MAPPING row; B is the only
// way out, as it is from MIDI.
//
// ⚠️ **THE LIST GROWS — IT IS NEVER 128 ROWS.** `Project::midiMappings` is a count plus that many
// entries, so an untouched song carries none and this screen is its headers, the empty line and the
// row that adds the first one. A fixed 128-slot array would put 128 blanks in every .ptp and leave
// the screen hiding them.
//
// ⚠️ **THE DESTINATION IS TWO CELLS, NOT A LIST**: a GROUP (five of them) and a parameter within it
// (ten at the deepest). Twenty-eight names in one cycle would be a cell nobody could aim; two cells
// put the longest reach at ten steps and need nothing drawn that is not already a row.
//
// ⭐ **A ROW WHOSE DESTINATION HAS GONE IS KEPT, DIMMED, AND SAYS WHY** — the instrument slot cleared,
// or an id from a newer build. Dropping it silently means a mapping the user has to notice is missing
// before they can make it again.

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
 * ⚠️⚠️ **THESE ARE IN THE ORDER THE ROW IS DRAWN, AND THAT IS LOAD-BEARING.** LEFT and RIGHT step
 * this enum by one, so a member out of place sends the highlight the other way from the press — a
 * RIGHT that jumps over the scope cell and then comes back to it from the far side, which is exactly
 * how the scope cell became unfindable. Nothing stores these numbers (a cursor column is AppState,
 * never the `.ptp`), so the order is free to follow the drawing and must.
 *
 * ⚠️ SCOPE exists only where the destination has one (a track's fader, an instrument's cutoff). It
 * is SKIPPED there rather than renumbered — the columns are drawn in fixed places on every row, so a
 * destination without a scope leaves the gap rather than sliding its name left.
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
     * ⚠️ ADD is absent, exactly as PANIC and TEST are absent from the MIDI screen's: it is a plain-A
     * ACTION and it grows a vector the cursor is standing in, so the dispatcher — which owns the
     * cursor — performs it.
     */
    MidiMapInputResult handle_input(songcore::Project& project, int cursor_row, int cursor_column,
                                    const InputAction& action) const;
};

}  // namespace pt::ui
