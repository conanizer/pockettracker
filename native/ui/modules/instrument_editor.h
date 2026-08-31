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
// INSTRUMENT TYPE — a SoundFont gains a PATCH row and loses the four sample-window rows, so every row
// below the source section shifts by one (`sf_offset`).
//
// The row geometry therefore lives in ONE place — ui/instrument_row_layout.h — which the cursor walks
// and this module draws. They must agree: a row added here without an entry there strands the cursor
// on a spacer or skips a live row. (Kotlin learned that the hard way; the table exists because the
// same geometry was once re-encoded at four movement sites.)
//
// ── ROWS ─────────────────────────────────────────────────────────────────────────────────────────
//
//   SAMPLER (16)                          SOUNDFONT (15)               EXTERNAL (12)
//    0  TYPE + LOAD + EDIT                 0  TYPE + LOAD               0  TYPE
//    1  NAME                               1  NAME                      1  NAME
//    2  ROOT + DETUNE + TIC                2  ROOT + DETUNE + TIC       2  CHAN + BANK
//    3  VOL + SLICE + PAN                  3  VOL + PAN                 3  PROG + LEN
//    4  ·spacer·                           4  ·spacer·                  4  ·spacer·
//    5  INST PRESET: SAVE | LOAD           5  INST PRESET: SAVE | LOAD  5  INST PRESET: SAVE | LOAD
//    6  ·spacer·                           6  PATCH                     6  ·spacer·
//    7  DRIVE + FILTER                     7  ·spacer·                  7  VOL + PAN
//    8  CRUSH + FREQ                       8  DRIVE + FILTER            8  CC A + value
//    9  DWNSMPL + RES                      9  CRUSH + FREQ              9  CC B + value
//   10  ·spacer·                          10  DWNSMPL + RES            10  CC C + value
//   11  REV + DEL                         11  ·spacer·                 11  CC D + value
//   12  EQ                                12  REV
//   13  LOOP + START                      13  DEL
//   14  LOOP ST + END                     14  EQ
//   15  LOOP END + REVERSE
//
// The TYPE row's LOAD/EDIT load and edit the SOURCE (sample or SF2); the INST PRESET row's SAVE/LOAD
// write and read the whole instrument as a .pti — the two used to share row 0 as one confusing pair of
// LOADs. The SF's PATCH row selects a patch inside the loaded SoundFont, a different thing again.
//
// ── EXTERNAL ─────────────────────────────────────────────────────────────────────────────────────
//
// EXTERNAL owns NO source and no voice: its events leave over a cable (songcore/midi_out.h) and no
// voice is raised for them, so none of the sampler's DSP applies — there is no signal of ours to
// drive, filter, crush or equalise. Every cell on it is a byte the device is actually sent:
//
//   CHAN  1-16, shown 1-based and stored 0-based (`midiChannel`) — the MIDI convention, both ways.
//   BANK  "----" = send nothing; else the 14-bit bank sent as CC0/CC32 ahead of the program. FOUR
//         hex digits, which is why it sits on a DUAL row rather than sharing a TRIPLE with PROG.
//   PROG  "--" = send nothing; else the program change sent with the patch's first note-on.
//   VOL   scales note-on VELOCITY (there is no gain stage on our side of the cable — LGPT does this).
//   PAN   is sent as CC 10, de-duplicated per channel.
//   LEN   gate length in TICKS, 00 = gate-to-next (LGPT's LEN). See midi_out.h's `end_note`.
//   CC A-D   number + default value, both "--" when unused; the defaults ride every note-on.
//
// Columns: 0 = the label, 1 = the first value, 2 = a button (LOAD / SAVE), 3 = the second value or
// button (EDIT / LOAD), and on the two TRIPLE rows additionally 5 = the third value.

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

    songcore::InstrumentType type() const { return instrument.instrumentType; }

    bool is_soundfont() const {
        return instrument.instrumentType == songcore::InstrumentType::SOUNDFONT;
    }
    bool is_external() const {
        return instrument.instrumentType == songcore::InstrumentType::EXTERNAL;
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

    /** Apply a resolved action to the instrument. Its own `instrumentType` selects the row map. */
    InstrumentInputResult handle_input(songcore::Instrument& ins, int cursor_row, int cursor_column,
                                       const InputAction& action) const;

private:
    /** One parameter, label + value, at the standard two columns. */
    void draw_parameter_row(Canvas& c, int y, int name_x, int value_x, const std::string& name,
                            const std::string& value, bool cursor_on_name, bool cursor_on_value,
                            const Theme& t) const;

    /** Two parameters. Cursor columns 1 and 3. */
    void draw_dual_row(Canvas& c, int y, int name_x, int value_x, const std::string& n1,
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

    void draw_name_row(Canvas& c, int y, int name_x, int value_x,
                       const InstrumentEditorState& s, int this_row, const Theme& t) const;

    void draw_section_source_row(Canvas& c, int x, int y, int name_x, int cursor_row,
                                 int cursor_column, int this_row, const Theme& t) const;

    /**
     * The whole EXTERNAL screen, drawn as its own pass rather than as branches inside the sampler's.
     *
     * It shares only the TYPE, NAME and INST PRESET rows with the other two, and every row below them
     * is a different parameter — so weaving it through `draw` would have put a third arm on nine `if
     * (sf)` tests and moved the two ORIGINAL layouts' code, for no shared logic. Separate pass, and
     * the two shipped types keep the exact path they had.
     */
    void draw_external(Canvas& c, int x, int y, const InstrumentEditorState& s) const;

    void draw_eq_row(Canvas& c, int y, int name_x, int value_x, int eq_slot,
                     const std::string& n2, const std::string& v2,
                     int cursor_row, int cursor_column, int this_row, const Theme& t) const;
};

}  // namespace pt::ui
