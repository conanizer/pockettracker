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

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** The OFFSET row's value: a sign, two digits and its unit — "+00 MS", "-25 MS". */
std::string offset_text(int ms) {
    const int a = ms < 0 ? -ms : ms;
    return std::string(ms < 0 ? "-" : "+") + dec2(a) + " MS";
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

    // ── OUTPUT — the port, or WHY there is not one ───────────────────────────────────────────────
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
    {
        const int count   = static_cast<int>(s.deviceNames.size());
        const int idx     = clamp(s.deviceIndex, 0, count - 1);
        const int devices = count - 1;   // "OFF" is index 0 and is not a device

        std::string value;
        if (idx == 0) {
            value = devices > 0 ? "OFF  " + dec2(devices) + " PORTS" : "OFF  NO PORTS";
        } else {
            value = s.deviceNames[static_cast<size_t>(idx)];
            if (static_cast<int>(value.size()) > VALUE_MAX_CHARS)
                value = value.substr(0, static_cast<size_t>(VALUE_MAX_CHARS));
        }
        row_of(MidiRow::OUTPUT, "OUTPUT", value);
    }

    row_of(MidiRow::OFFSET,   "OFFSET",   offset_text(s.settings.midiOffsetMs));
    row_of(MidiRow::PROG_CHG, "PROG CHG", s.project.midiSendProgramChange ? "ON" : "OFF");

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

        case MidiRow::OFFSET: {
            // ⚠️ `empty_value` is forced OUT OF RANGE. `hex_byte`'s default is −1, and −1 is a perfectly
            // ordinary offset — one millisecond early. Left at the default, the context would report
            // `isEmpty` at that one value and A+DPAD would go dead on it: an offset you could dial past
            // but not away from, on the one screen whose purpose is dialling it.
            CursorContext c = cc::hex_byte(s.settings.midiOffsetMs, -99, 99,
                                           /*empty_value=*/-1000);
            c.largeStep = 10;   // A+LEFT/RIGHT walks it in tens, like TEMPO
            return c;
        }

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
                                         const InputAction& action) const {
    MidiInputResult r;
    if (cursor_column == 0 || action.type != ActionType::SET_VALUE) return r;

    switch (static_cast<MidiRow>(cursor_row)) {
        case MidiRow::OUTPUT: {
            // ⚠️ The module writes the NAME, not the index it was just handed — see the header. The
            // index is a fact about the list as it stood a moment ago; the name is the choice.
            if (device_names.empty()) break;
            const int idx = clamp(action.value, 0, static_cast<int>(device_names.size()) - 1);
            const std::string& picked = device_names[static_cast<size_t>(idx)];
            if (picked != settings.midiOutDevice) {
                settings.midiOutDevice = picked;
                r.deviceChanged        = true;
            }
            break;
        }

        case MidiRow::OFFSET: {
            const int ms = clamp(action.value, -99, 99);
            if (ms != settings.midiOffsetMs) {
                settings.midiOffsetMs = ms;
                r.offsetChanged       = true;
            }
            break;
        }

        case MidiRow::PROG_CHG:
            // ⚠️ …and THIS one dirties the SONG, where the two above do not. It is a `Project` field
            // that emits into the .ptp, so changing it is an edit in exactly the sense the autosave and
            // the "unsaved work" dialog mean. OUTPUT and OFFSET are settings.json's and must not be —
            // picking a cable is not composing.
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
