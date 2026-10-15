#pragma once

// The instrument editor: everything a voice reads at trigger time. A form, not a grid — which cell
// sits where is one table per type (`ui/instrument_row_layout.h`), and everything here switches on
// the cell that table names, never on a row number.
// TYPE's LOAD/EDIT act on the source; INST PRESET's SAVE/LOAD a whole .pti; PATCH picks inside an SF2.
//
// EXTERNAL has no source and no voice — its events leave over a cable, so every cell is a byte sent:
//   CHAN  1-16, stored 0-15
//   BANK  "----" = none; else 14-bit, sent as CC0/CC32 before the program (four digits, so a DUAL row)
//   PROG  "--" = none; else sent with the first note-on
//   VOL   scales note-on velocity · PAN sent as CC 10, de-duplicated per channel
//   LEN   gate length in ticks, 00 = gate-to-next (midi_out.h `end_note`)
//   CC A-D  number + default value, "--" when unused; the defaults ride every note-on

#include <string>
#include <vector>

#include "songcore/midi_map.h"
#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/instrument_row_layout.h"
#include "ui/theme.h"

namespace pt::ui {

// The model stores the string (filter, loop) or its index (slice).
inline const std::vector<std::string>& slice_modes() {
    static const std::vector<std::string> v{"OFF", "CUT", "TRU"};
    return v;
}
inline const std::vector<std::string>& filter_types() {
    static const std::vector<std::string> v{"off", "lp", "hp", "bp"};
    return v;
}
inline const std::vector<std::string>& loop_modes() {
    static const std::vector<std::string> v{"off", "fwd", "png", "osc"};
    return v;
}

struct InstrumentEditorState {
    const songcore::Instrument& instrument;
    int cursorRow    = 0;
    int cursorColumn = 1;

    // The SF2's preset list as the engine reads it; zeroes and "---" with no SoundFont.
    std::string sfPresetName{};
    int         sfPresetCount = 0;
    int         sfPresetIndex = 0;

    // LOOP may step onto `osc` (platform_caps.h `loopWindow`). Only the cursor asks, so a saved `osc`
    // still draws on every build.
    bool allowOscLoop = true;

    Theme theme = theme_classic();

    songcore::InstrumentType type() const { return instrument.instrumentType; }

    bool is_external() const {
        return instrument.instrumentType == songcore::InstrumentType::EXTERNAL;
    }
};

struct InstrumentInputResult {
    bool modified = false;

    // The PRESET row was stepped; only the engine has the SF2's list, so the dispatcher resolves it.
    bool presetIndexChanged = false;
    int  presetIndex        = 0;
};

class InstrumentEditorModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const InstrumentEditorState& s) const;

    CursorContext cursor_context(const InstrumentEditorState& s) const;

    // What the cell under the cursor is called, for MIDI learn. ⚠️ The scope is left at 0 for the
    // caller. ⚠️ The type selects the row map: a mapping stored as a row would move with the type.
    songcore::MapTarget map_target(const InstrumentEditorState& s) const;

    // Its own `instrumentType` selects the row map.
    InstrumentInputResult handle_input(songcore::Instrument& ins, int cursor_row, int cursor_column,
                                       const InputAction& action) const;

private:
    // The EQ cell gets the shared EQ painter.
    void draw_cell(Canvas& c, int label_x, int value_x, int text_y, InstrumentCell cell,
                   int cc_slot_index, bool is_cursor, const InstrumentEditorState& s) const;

    void draw_type_load_row(Canvas& c, int x, int y, int name_x, const songcore::Instrument& ins,
                            int cursor_row, int cursor_column, int this_row, const Theme& t) const;

    void draw_section_source_row(Canvas& c, int x, int y, int name_x, int cursor_row,
                                 int cursor_column, int this_row, const Theme& t) const;
};

}  // namespace pt::ui
