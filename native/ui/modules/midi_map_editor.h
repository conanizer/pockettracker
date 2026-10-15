#pragma once

// The MIDI mapping list: per mapping the CC, the destination's live value, the range and the
// destination. B is the only way out. The list grows from nothing — headers, an empty line and ADD.
// The destination is two cells, a group and a parameter in it. A row whose destination has gone is
// kept, dimmed, and says why.

#include <string>

#include "songcore/midi_map.h"
#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

// One mapping row's cursor columns; the live value has none (it is read from the song).
// ⚠️ In drawn order: LEFT/RIGHT step this enum by one. SCOPE is skipped where there is none.
enum class MapCol {
    CC    = 1,
    MIN   = 2,
    MAX   = 3,
    GROUP = 4,
    SCOPE = 5,
    PARAM = 6,
};

inline constexpr int MIDI_MAP_COLUMN_COUNT = 6;

inline bool map_scope_editable(songcore::MapScope s) { return s != songcore::MapScope::NONE; }

// 1 on the ADD row, a button.
inline int midi_map_max_column(const songcore::Project& p, int row) {
    if (row < 0 || row >= static_cast<int>(p.midiMappings.size())) return 1;   // the ADD row
    return static_cast<int>(MapCol::PARAM);
}

inline bool midi_map_scope_visible(const songcore::Project& p, int row) {
    if (row < 0 || row >= static_cast<int>(p.midiMappings.size())) return false;
    const songcore::MapDest* d = songcore::map_dest(p.midiMappings[static_cast<size_t>(row)].dest);
    return d != nullptr && map_scope_editable(d->scope);
}

// `column` brought onto a cell this row draws. `prefer` is the travel direction, so a move landing on
// a hidden SCOPE continues past it; vertical moves pass +1.
inline int midi_map_clamp_column(const songcore::Project& p, int row, int column, int prefer = +1) {
    const int max = midi_map_max_column(p, row);
    if (column < 1) column = 1;
    if (column > max) column = max;
    if (column == static_cast<int>(MapCol::SCOPE) && !midi_map_scope_visible(p, row))
        column = prefer < 0 ? static_cast<int>(MapCol::GROUP) : static_cast<int>(MapCol::PARAM);
    return column;
}

// One per mapping, plus ADD.
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
    bool modified   = false;   // the mappings are the song's
    bool rowDeleted = false;   // the cursor may be past the end
};

class MidiMapModule {
  public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const MidiMapState& s) const;

    CursorContext cursor_context(const MidiMapState& s) const;

    // ⚠️ ADD is the dispatcher's: it grows the list the cursor stands in.
    MidiMapInputResult handle_input(songcore::Project& project, int cursor_row, int cursor_column,
                                    const InputAction& action) const;
};

}  // namespace pt::ui
