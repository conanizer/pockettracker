#include "ui/modules/midi_settings.h"

#include <algorithm>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

constexpr int NAME_X  = 10;    // the label column
constexpr int VALUE_X = 156;   // the value column

// ⚠️ VALUE_X IS 156 HERE AND 210 ON PROJECT, AND THE 54px IS BOUGHT FOR ONE ROW: THE DEVICE NAME.
//
// This is B4.2's finding again — *a layout constraint is a data constraint* — except that this time the
// data belongs to the operating system and cannot be reshaped to fit. A Windows MIDI port is called
// things like "Microsoft GS Wavetable Synth" (28 characters); the panel is 510px and a glyph is 17, so
// even starting at the left margin only 29 fit and starting at PROJECT's value column only 17 do. The
// longest label on this screen is "PROG CHG" (8), so the column moves left to where that label ends and
// the name gets every pixel there is.
//
// It is still not always enough, and the row TRUNCATES rather than overflowing into the panel border.
// Head-first, because a port's distinguishing word is at the front far more often than at the back
// ("loopMIDI Port" vs "Microsoft GS…"). Two ports differing only in a trailing number is the case that
// costs, and it is accepted rather than solved: 20 characters is what there is.
constexpr int VALUE_MAX_CHARS = (MidiModule::WIDTH - VALUE_X - NAME_X) / CHAR_W;

// ─── The IN CH row's eight cells ─────────────────────────────────────────────────────────────────
//
// ⚠️ **THE PITCH IS 44px AND NOT A WHOLE NUMBER OF CHARACTERS, WHICH IS THE ONLY PLACE ON THIS SCREEN
// THAT LEAVES THE TEXT GRID — and it is a layout constraint deciding a data question again (B4.2).**
// A cell is two digits (34px with the trailing spacing dropped). Eight of them on the character grid
// with a one-space gap is a pitch of 51, which puts the last cell's right edge at 156 + 7*51 + 32 =
// 545 on a 510px panel: the map would not fit beside the other rows' value column, and the row would
// have to start at the left margin and lose its label. 44 keeps it in the value column with every
// other row, and 496 < 510 with room for the panel border.
//
// ⭐ **THE ALTERNATIVE WAS EIGHT ROWS, AND IT DOES NOT FIT EITHER**: `IN CH T1`…`T8` is 168px of extra
// height on a 392px panel that is already 272 deep with the map as one row — it would push PANIC and
// TEST off the bottom. §8.1's original sketch had it as one line for the same reason M8 does.
constexpr int MAP_CELL_X     = VALUE_X;
constexpr int MAP_CELL_PITCH = 44;

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** The OFFSET row's value: a sign, two digits and its unit — "+00 MS", "-25 MS". */
std::string offset_text(int ms) {
    const int a = ms < 0 ? -ms : ms;
    return std::string(ms < 0 ? "-" : "+") + dec2(a) + " MS";
}

/**
 * One device row's value: the name that is OPEN, or OFF plus how many ports there were.
 *
 * ⭐ Shared by OUTPUT and INPUT, and it has to be: the whole point of the "OFF  02 PORTS" wording
 * (B4.3) is telling *"no device picked"* apart from *"this machine has none"*, and an INPUT row that
 * answered that question differently from OUTPUT would be the second, quieter half of the same
 * confusion. A machine can easily have two outputs and no inputs — this desk did, until loopMIDI.
 */
std::string device_text(const std::vector<std::string>& names, int index) {
    const int count = static_cast<int>(names.size());
    if (count == 0) return "OFF  NO PORTS";

    const int idx     = clamp(index, 0, count - 1);
    const int devices = count - 1;   // "OFF" is index 0 and is not a device
    if (idx == 0) return devices > 0 ? "OFF  " + dec2(devices) + " PORTS" : "OFF  NO PORTS";

    return Canvas::clip_text(names[static_cast<size_t>(idx)], VALUE_MAX_CHARS);
}

/**
 * One IN CH cell: `--` for off, else the channel numbered the way a musician's gear numbers it.
 *
 * ⚠️ **STORED 0-15, SHOWN 1-16** — the instrument screen's CHAN row makes exactly the same promise
 * (instrument_editor.cpp), and the two must not disagree: a keyboard set to "channel 1" and a track
 * showing `01` have to be the same number, or the map is a puzzle. The +1 lives in the DRAWING on both
 * screens and nowhere else; the context and the .ptp carry the wire number.
 */
std::string map_cell_text(int channel) { return channel < 0 ? "--" : dec2(channel + 1); }

/** The stored channel for a cursor COLUMN (1-based), or −1 for a column that names no track. */
int map_channel_at(const songcore::Project& p, int column) {
    const int track = column - 1;
    if (track < 0 || track >= static_cast<int>(p.midiInputChannels.size())) return -1;
    return p.midiInputChannels[static_cast<size_t>(track)];
}

}  // namespace

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void MidiModule::draw(Canvas& c, int x, int y, const MidiState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const int labelX = x + NAME_X;
    const int valueX = x + VALUE_X;

    c.draw_text("MIDI", labelX, y + TEXT_PADDING, t.textTitle, CHAR_SPACING, FONT_SCALE);

    const int firstRowY = y + TEXT_PADDING + ROW_HEIGHT + 14;
    const auto rowY = [&](MidiRow row) { return firstRowY + midi_row_offset_y(row, ROW_HEIGHT); };

    const auto on_row = [&](MidiRow row) { return s.cursorRow == static_cast<int>(row); };

    const auto row_of = [&](MidiRow row, const char* name, const std::string& value) {
        if (on_row(row)) c.fill_rect(x, rowY(row), WIDTH, ROW_HEIGHT, t.rowCursor);
        c.draw_text(name, labelX, rowY(row) + TEXT_PADDING,
                    on_row(row) ? t.textCursor : t.textParam, CHAR_SPACING, FONT_SCALE);
        c.draw_text(value, valueX, rowY(row) + TEXT_PADDING,
                    on_row(row) ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
    };

    // ── OUTPUT and INPUT — the ports, or WHY there is not one ────────────────────────────────────
    //
    // ⚠️ THE PORT COUNT RIDES INSIDE THE "OFF" TEXT, and the first draft had it as a separate `nn/nn`
    // counter drawn beside the label. ⭐ **A `ptshot` of this screen is what killed that** — the counter
    // started at 146px and the value column at 156, so the two printed on top of each other and the row
    // read as garbage. Nothing else could have caught it: the module compiled, ptdispatch drove every
    // row of it green, and both numbers were individually correct.
    //
    // The fix is B4.2's again — **a layout constraint is a data constraint** — and it improves the
    // screen rather than merely fitting it. "How many ports does this machine see?" is the question you
    // ask precisely WHEN the row reads OFF and you are trying to work out why; beside a named device it
    // is noise competing for the pixels that device's name needs. So the two states say different
    // things, and neither has to share a row with the other.
    row_of(MidiRow::OUTPUT, "OUTPUT", device_text(s.deviceNames,   s.deviceIndex));
    row_of(MidiRow::INPUT,  "INPUT",  device_text(s.inDeviceNames, s.inDeviceIndex));

    row_of(MidiRow::OFFSET,   "OFFSET",   offset_text(s.settings.midiOffsetMs));
    // ⚠️ The value says what the switch DOES, not merely that it is on — "ON  24 PPQN" is the whole of
    // this row's documentation, on a device with no manual and no tooltip. It is the same reasoning as
    // OUTPUT's port count above: a row has pixels to spare exactly when its value is the boring one.
    row_of(MidiRow::SYNC,     "SYNC",     s.settings.midiSyncOut ? "ON  24 PPQN" : "OFF");
    row_of(MidiRow::PROG_CHG, "PROG CHG", s.project.midiSendProgramChange ? "ON" : "OFF");

    // ── IN CH — the per-track input channel map (plan §7, §8.1's "TRACK INPUT MAP") ──────────────
    //
    // Eight cells, one per track, in track order. This is the row that answers "which of my tracks does
    // this keyboard play", and until it existed the answer was a field in the .ptp that had to be typed
    // by hand — `midiInputChannels` round-tripped from B1 and E1 was the first thing to READ it.
    //
    // ⚠️ **THE HEADER IS NOT DECORATION, IT IS WHAT MAKES THE ROW READABLE.** Without it the cells are
    // eight identical `--` and the only way to know which is track 5 is to count. It is drawn in the
    // blank row the group gap already reserves above this one, at the same cell x positions, so it
    // costs no extra height. ⭐ `ptshot` is the only tool that can see whether those two lines line up
    // — the same reason it caught the OUTPUT row's overprint.
    {
        const bool onMap    = on_row(MidiRow::IN_MAP);
        const int  mapY     = rowY(MidiRow::IN_MAP);
        const int  headerY  = mapY - ROW_HEIGHT;
        const int  cellX    = x + MAP_CELL_X;
        const int  tracks   = std::min(MIDI_IN_MAP_COLUMNS,
                                       static_cast<int>(s.project.midiInputChannels.size()));

        for (int i = 0; i < tracks; ++i) {
            // Centred over a two-digit cell: one glyph is CHAR_W narrower than the value above it.
            c.draw_text(std::to_string(i + 1), cellX + MAP_CELL_PITCH * i + CHAR_W / 2,
                        headerY + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
        }

        if (onMap) c.fill_rect(x, mapY, WIDTH, ROW_HEIGHT, t.rowCursor);
        c.draw_text("IN CH", labelX, mapY + TEXT_PADDING, onMap ? t.textCursor : t.textParam,
                    CHAR_SPACING, FONT_SCALE);

        for (int i = 0; i < tracks; ++i) {
            // ⚠️ The whole row lights up and only the SELECTED CELL's text takes the cursor colour —
            // the groove editor's convention, and the one every multi-column row in this app follows.
            const bool onCell = onMap && (s.cursorColumn == i + 1);
            const Argb col    = onCell ? t.textCursor : t.textValue;
            c.draw_text(map_cell_text(s.project.midiInputChannels[static_cast<size_t>(i)]),
                        cellX + MAP_CELL_PITCH * i, mapY + TEXT_PADDING, col, CHAR_SPACING,
                        FONT_SCALE);
        }
    }

    // The two action rows. Drawn like PROJECT's SYSTEM and EXIT, because they are the same kind of
    // thing: a row whose whole content is what A does on it.
    row_of(MidiRow::PANIC, "PANIC", "A: ALL NOTES OFF");
    row_of(MidiRow::TEST,  "TEST",  "A: C-4 CH 1");

    // ── The status readout ───────────────────────────────────────────────────────────────────────
    //
    // ⚠️ IT EXISTS BECAUSE PANIC AND TEST BOTH SUCCEED SILENTLY, AND SO DOES NEITHER OF THEM RUNNING.
    // That is the guardrail's "a handler whose correct behaviour is silence cannot be told from one
    // that never ran", sitting on a screen instead of in a log: the whole point of TEST is to answer
    // "is there a cable" on a machine where the answer is currently a guess, so the press has to say
    // out loud that it happened — and say NO PORT when it could not.
    if (!s.statusText.empty()) {
        const int statusY = firstRowY + midi_row_offset_y(MidiRow::TEST, ROW_HEIGHT) + ROW_HEIGHT * 2;
        c.draw_text(s.statusText, labelX, statusY + TEXT_PADDING, t.textTitle, CHAR_SPACING,
                    FONT_SCALE);
    }
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext MidiModule::cursor_context(const MidiState& s) const {
    if (s.cursorColumn == 0) return cc::read_only();   // the label — unreachable, as on PROJECT

    switch (static_cast<MidiRow>(s.cursorRow)) {
        case MidiRow::OUTPUT:
            // SETTINGS' OVERLAY row's context exactly: a cycle over a list the platform supplied, with
            // index 0 meaning "none". `enum_cycle` and not `index_cycle` — cursor.h explains at length
            // why those two are not interchangeable even though they behave identically.
            return cc::enum_cycle(s.deviceIndex, static_cast<int>(s.deviceNames.size()));

        case MidiRow::INPUT:
            return cc::enum_cycle(s.inDeviceIndex, static_cast<int>(s.inDeviceNames.size()));

        case MidiRow::IN_MAP: {
            // ⚠️ **−1 IS THE EMPTY VALUE, WHICH IS WHAT MAKES A+B MEAN "THIS TRACK LISTENS TO NOTHING"**
            // — the project's one empty convention (model.h), and the same context the instrument
            // screen's BANK/PROG/CC cells use (`midi_opt_context`). Channel 0 is a real channel (shown
            // `01`), so a cell that treated 0 as empty would make track 1's most likely setting
            // undialable.
            const int ch = map_channel_at(s.project, s.cursorColumn);
            return cc::hex_byte(ch, /*min=*/0, /*max=*/15, /*empty_value=*/-1,
                                /*can_delete=*/ch >= 0, /*can_insert=*/ch < 0);
        }

        case MidiRow::OFFSET: {
            // ⚠️ `empty_value` is forced OUT OF RANGE. `hex_byte`'s default is −1, and −1 is a perfectly
            // ordinary offset — one millisecond early. Left at the default, the context would report
            // `isEmpty` at that one value and A+DPAD would go dead on it: an offset you could dial past
            // but not away from, on the one screen whose purpose is dialling it.
            CursorContext c = cc::hex_byte(s.settings.midiOffsetMs, -99, 99,
                                           /*empty_value=*/-1000);
            c.largeStep = 10;   // A+UP/DOWN walks it in tens, like TEMPO
            return c;
        }

        case MidiRow::SYNC:
            return cc::toggle_binary(s.settings.midiSyncOut);

        case MidiRow::PROG_CHG:
            return cc::toggle_binary(s.project.midiSendProgramChange);

        // The action rows. Read-only to the generic edit path; plain A is the whole of their behaviour
        // and the dispatcher owns it, because it is the only layer that can reach a cable.
        case MidiRow::PANIC:
        case MidiRow::TEST:
            return cc::read_only();
    }
    return cc::none();
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

MidiInputResult MidiModule::handle_input(songcore::Project& project, SettingsValues& settings,
                                         int cursor_row, int cursor_column,
                                         const std::vector<std::string>& device_names,
                                         const std::vector<std::string>& in_device_names,
                                         const InputAction& action) const {
    MidiInputResult r;
    if (cursor_column == 0 || action.type == ActionType::NONE) return r;

    // ⚠️ **THE `SET_VALUE`-ONLY GUARD MOVED OFF THE FRONT OF THIS FUNCTION IN E3, AND THAT IS THE
    // INTERESTING PART OF THE DIFF.** Every row this screen had until now is a cycle or a number with
    // no "unset" state, so DELETE and INSERT_DEFAULT had nothing to do and the early return was free.
    // The IN CH cells have three states — off, and 0..15 — so they need all three actions, and a guard
    // that had stayed where it was would have made A+B on a mapped track do nothing at all: an
    // input channel you could dial past but never clear.
    const bool isSet = (action.type == ActionType::SET_VALUE);

    // The device rows: the module writes the NAME, not the index it was just handed — see the header.
    // The index is a fact about the list as it stood a moment ago; the name is the choice.
    const auto pick_device = [&](const std::vector<std::string>& names, std::string& field) {
        if (!isSet || names.empty()) return false;
        const int idx = clamp(action.value, 0, static_cast<int>(names.size()) - 1);
        const std::string& picked = names[static_cast<size_t>(idx)];
        if (picked == field) return false;
        field = picked;
        return true;
    };

    switch (static_cast<MidiRow>(cursor_row)) {
        case MidiRow::OUTPUT:
            r.deviceChanged = pick_device(device_names, settings.midiOutDevice);
            break;

        case MidiRow::INPUT:
            r.inDeviceChanged = pick_device(in_device_names, settings.midiInDevice);
            break;

        case MidiRow::IN_MAP: {
            // ⚠️ …and THIS dirties the SONG, for PROG CHG's reason below: `midiInputChannels` is a
            // `Project` field that emits into the .ptp. Which track answers a keyboard is part of how
            // the song is played, and it must travel with it.
            const int track = cursor_column - 1;
            if (track < 0 || track >= static_cast<int>(project.midiInputChannels.size())) break;
            int& ch = project.midiInputChannels[static_cast<size_t>(track)];

            const int before = ch;
            if (isSet)                                          ch = clamp(action.value, 0, 15);
            else if (action.type == ActionType::DELETE)         ch = -1;   // this track listens to none
            else if (action.type == ActionType::INSERT_DEFAULT) ch = 0;    // channel 1, shown 01

            r.projectModified = (ch != before);
            break;
        }

        case MidiRow::OFFSET: {
            if (!isSet) break;
            const int ms = clamp(action.value, -99, 99);
            if (ms != settings.midiOffsetMs) {
                settings.midiOffsetMs = ms;
                r.offsetChanged       = true;
            }
            break;
        }

        case MidiRow::SYNC: {
            if (!isSet) break;
            const bool on = action.value != 0;
            if (on != settings.midiSyncOut) {
                settings.midiSyncOut = on;
                r.syncChanged        = true;
            }
            break;
        }

        case MidiRow::PROG_CHG:
            // ⚠️ …and THIS one dirties the SONG, where the cable rows do not. It is a `Project` field
            // that emits into the .ptp, so changing it is an edit in exactly the sense the autosave and
            // the "unsaved work" dialog mean. OUTPUT, INPUT and OFFSET are settings.json's and must not
            // be — picking a cable is not composing.
            if (!isSet) break;
            project.midiSendProgramChange = (action.value != 0);
            r.projectModified             = true;
            break;

        case MidiRow::PANIC:
        case MidiRow::TEST:
            break;
    }

    return r;
}

}  // namespace pt::ui
