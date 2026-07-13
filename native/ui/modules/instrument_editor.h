#pragma once

// ─── INSTRUMENT EDITOR ───────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/InstrumentModule.kt — the screen where an instrument becomes a SOUND
// rather than an arrangement. Everything the voice reads at trigger time is dialled in here: the root
// and detune it plays at, its volume and pan, the drive / crush / downsample / filter it runs through,
// the sample window and loop it plays out of, its reverb and delay sends and its EQ slot.
//
// ── IT IS NOT A GRID, AND THAT IS THE WHOLE DIFFICULTY ───────────────────────────────────────────
//
// The five editors that came before are 16 rows by N columns, and their cursor is a pair of integers
// inside a rectangle. This screen is a FORM: rows hold one, two or three parameters, some rows are
// unreachable spacers, some are buttons rather than values, and THE ROW LIST ITSELF DEPENDS ON THE
// INSTRUMENT TYPE — a SoundFont gains a PRESET row and loses the four sample-window rows, so every row
// below the source section shifts by one (`sf_offset`).
//
// The row geometry therefore lives in ONE place — ui/instrument_row_layout.h — which the cursor walks
// and this module draws. They must agree: a row added here without an entry there strands the cursor
// on a spacer or skips a live row. (Kotlin learned that the hard way; the table exists because the
// same geometry was once re-encoded at four movement sites.)
//
// ── ROWS ─────────────────────────────────────────────────────────────────────────────────────────
//
//   SAMPLER (16)                          SOUNDFONT (15)
//    0  TYPE + LOAD + SAVE                 0  TYPE + LOAD + SAVE
//    1  NAME                               1  NAME
//    2  ROOT + DETUNE + TIC                2  ROOT + DETUNE + TIC
//    3  VOL + SLICE + PAN                  3  VOL + PAN
//    4  ·spacer·                           4  ·spacer·
//    5  SMPL: LOAD | EDIT                  5  SF: LOAD
//    6  ·spacer·                           6  PRESET
//    7  DRIVE + FILTER                     7  ·spacer·
//    8  CRUSH + FREQ                       8  DRIVE + FILTER
//    9  DWNSMPL + RES                      9  CRUSH + FREQ
//   10  ·spacer·                          10  DWNSMPL + RES
//   11  REV + DEL                         11  ·spacer·
//   12  EQ                                12  REV
//   13  LOOP + START                      13  DEL
//   14  LOOP ST + END                     14  EQ
//   15  LOOP END + REVERSE
//
// Columns: 0 = the label, 1 = the first value, 2 = a button (LOAD), 3 = the second value or button,
// and on the two TRIPLE rows additionally 5 = the third value.

#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

/** The three string-valued cycles. The model stores the STRING (filter, loop) or its index (slice). */
inline const std::vector<std::string>& slice_modes() {
    static const std::vector<std::string> v{"OFF", "CUT", "TRU"};
    return v;
}
inline const std::vector<std::string>& filter_types() {
    static const std::vector<std::string> v{"off", "lp", "hp", "bp"};
    return v;
}
inline const std::vector<std::string>& loop_modes() {
    static const std::vector<std::string> v{"off", "fwd", "png"};
    return v;
}

struct InstrumentEditorState {
    const songcore::Instrument& instrument;
    int cursorRow    = 0;
    int cursorColumn = 1;

    // The SF2's preset list, read back from the engine (AppState::sfPreset*). Zeroes and "---" when
    // there is no SoundFont — which is what lets ptshot draw this screen with no engine at all.
    std::string sfPresetName{};
    int         sfPresetCount = 0;
    int         sfPresetIndex = 0;

    Theme theme = theme_classic();

    bool is_soundfont() const {
        return instrument.instrumentType == songcore::InstrumentType::SOUNDFONT;
    }
};

struct InstrumentInputResult {
    bool modified = false;

    /**
     * The PRESET row was stepped. The module does NOT apply it — the new bank+preset live in the SF2's
     * own preset list, which only the ENGINE has opened, so the dispatcher resolves the index through
     * `SongcoreHost::set_sf_preset_by_index`. Keeping the module a pure function of the Project is what
     * lets `tools/ptinput` measure every other row of this screen against the Kotlin original.
     */
    bool presetIndexChanged = false;
    int  presetIndex        = 0;
};

class InstrumentEditorModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const InstrumentEditorState& s) const;

    CursorContext cursor_context(const InstrumentEditorState& s) const;

    /** Apply a resolved action to the instrument. `is_soundfont` selects the row map. */
    InstrumentInputResult handle_input(songcore::Instrument& ins, int cursor_row, int cursor_column,
                                       const InputAction& action) const;

private:
    void draw_row_bg(Canvas& c, int x, int y, const Theme& t) const;

    /** One parameter, label + value, at the standard two columns. */
    void draw_parameter_row(Canvas& c, int x, int y, int name_x, int value_x, const std::string& name,
                            const std::string& value, bool cursor_on_name, bool cursor_on_value,
                            const Theme& t) const;

    /** Two parameters. Cursor columns 1 and 3. */
    void draw_dual_row(Canvas& c, int x, int y, int name_x, int value_x, const std::string& n1,
                       const std::string& v1, const std::string& n2, const std::string& v2,
                       int cursor_row, int cursor_column, int this_row, const Theme& t) const;

    /** Three parameters, at fixed offsets rather than the two standard columns. Cursor 1 / 3 / 5. */
    void draw_triple_row(Canvas& c, int x, int y, int name_x, const std::string& n1,
                         const std::string& v1, const std::string& n2, const std::string& v2,
                         const std::string& n3, const std::string& v3, int cursor_row,
                         int cursor_column, int this_row, const Theme& t) const;

    void draw_type_load_row(Canvas& c, int x, int y, int name_x, int value_x,
                            const songcore::Instrument& ins, int cursor_row, int cursor_column,
                            int this_row, const Theme& t) const;

    void draw_name_row(Canvas& c, int x, int y, int name_x, int value_x,
                       const InstrumentEditorState& s, int this_row, const Theme& t) const;

    void draw_section_source_row(Canvas& c, int x, int y, int name_x, int cursor_row,
                                 int cursor_column, int this_row, bool is_soundfont,
                                 const Theme& t) const;

    void draw_eq_row(Canvas& c, int x, int y, int name_x, int value_x, int eq_slot, int cursor_row,
                     int cursor_column, int this_row, const Theme& t) const;
};

}  // namespace pt::ui
