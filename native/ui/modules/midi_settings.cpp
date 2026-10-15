#include "ui/modules/midi_settings.h"

#include <algorithm>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

constexpr int NAME_X  = 10;    // the label column
constexpr int VALUE_X = 156;   // the value column

// The value column starts where "PROG CHG" ends, to leave room for long OS port names; a longer
// name is cut from the end — the distinguishing word is usually first.
constexpr int VALUE_MAX_CHARS = (MidiModule::WIDTH - VALUE_X - NAME_X) / CHAR_W;


// "+00 MS", "-25 MS". AUTO prints the derived number beside the word.
std::string offset_text(int ms, bool automatic) {
    const int a = ms < 0 ? -ms : ms;
    const std::string n = std::string(ms < 0 ? "-" : "+") + dec2(a) + " MS";
    return automatic ? "AUTO " + n : n;
}

// The open port's name, or OFF/AUTO plus the port count — "none picked" vs "this machine has none".
std::string device_text(const std::vector<std::string>& names, int index, const std::string& open_name) {
    const int count = static_cast<int>(names.size());
    if (count == 0) return "OFF  NO PORTS";

    const int         idx     = clamp(index, 0, count - 1);
    const int         devices = count > MIDI_FIRST_PORT ? count - MIDI_FIRST_PORT : 0;
    const std::string ports   = devices > 0 ? dec2(devices) + " PORTS" : "NO PORTS";
    if (idx == 0) return "OFF  " + ports;
    if (idx == 1) return Canvas::clip_text(open_name.empty() ? "AUTO  " + ports : "AUTO " + open_name,
                                           VALUE_MAX_CHARS);

    return Canvas::clip_text(names[static_cast<size_t>(idx)], VALUE_MAX_CHARS);
}

// The CTL CH cycle: ALL, then the sixteen channels. ⚠️ ALL is 16 on disk but 0 in the cycle.
constexpr int CTL_CH_OPTIONS = 17;
int ctl_ch_index(int stored) { return stored == songcore::MIDI_CTL_CH_ALL ? 0 : stored + 1; }
int ctl_ch_stored(int index) { return index <= 0 ? songcore::MIDI_CTL_CH_ALL : index - 1; }

// Which channels may carry a mapping knob — and, when none of the arriving knobs fit, where they
// are instead: the cable is the only thing that can tell the user.
std::string ctl_ch_text(const MidiState& s) {
    const int ch = s.settings.midiControlChannel;
    if (ch == songcore::MIDI_CTL_CH_ALL) return "ALL  MAPPED KNOBS";
    if (s.lastCcChannel >= 0 && s.lastCcChannel != ch)
        return dec2(ch + 1) + "  KNOBS ON " + dec2(s.lastCcChannel + 1);
    return dec2(ch + 1) + "  MAPPED KNOBS";
}

// One voice is MONO, more is POLY over that many tracks.
std::string keys_text(int voices) {
    return voices <= 1 ? "MONO" : "POLY " + std::to_string(voices);
}

// "BUF 10.7MS  CPU 8% MAX 27%". ⚠️ 29 glyphs fit; the 999 clamp and the missing space before MS
// keep the widest reading inside.
std::string audio_load_text(const AudioLoad& a) {
    if (a.blockFrames <= 0 || a.sampleRate <= 0) return "BUF --  CPU --";
    const int tenthsMs = (a.blockFrames * 10000 + a.sampleRate / 2) / a.sampleRate;
    const std::string buf = tenthsMs >= 1000 ? std::to_string(tenthsMs / 10)
                                             : std::to_string(tenthsMs / 10) + "." + std::to_string(tenthsMs % 10);
    const auto pct = [](int load) { return std::to_string(std::min((load + 5) / 10, 999)) + "%"; };
    return "BUF " + buf + "MS  CPU " + pct(a.meanLoad) + " MAX " + pct(a.worstLoad);
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
        c.draw_text(name, labelX, rowY(row) + TEXT_PADDING,
                    on_row(row) ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING, FONT_SCALE);
        draw_cursor_cell(c, value, valueX, rowY(row) + TEXT_PADDING, on_row(row), t.textValue, t);
    };

    // ── OUTPUT and INPUT: the port count rides inside the OFF text, where it is wanted ───────────
    row_of(MidiRow::OUTPUT, "OUTPUT", device_text(s.deviceNames,   s.deviceIndex,   s.outOpenName));
    row_of(MidiRow::INPUT,  "INPUT",  device_text(s.inDeviceNames, s.inDeviceIndex, s.inOpenName));

    row_of(MidiRow::OFFSET,   "OFFSET",   offset_text(midi_offset_in_force(s.settings, s.autoOffsetMs),
                                                      s.settings.midiOffsetAuto));
    // The value says what the switch does ("ON  24 PPQN").
    row_of(MidiRow::SYNC,     "SYNC",     s.settings.midiSyncOut ? "ON  24 PPQN" : "OFF");
    // CTL CH says where arriving knobs are when they miss it: a state the user cannot leave alone.
    row_of(MidiRow::CTL_CH,   "CTL CH", ctl_ch_text(s));
    row_of(MidiRow::PROG_CHG, "PROG CHG", s.project.midiSendProgramChange ? "ON" : "OFF");
    row_of(MidiRow::KEYS,     "KEYS",     keys_text(s.settings.midiInVoices));
    row_of(MidiRow::VELOCITY, "VELOCITY", s.settings.midiVelocity ? "ON" : "OFF");

    // The action rows read what A does; MAPPING carries the count.
    {
        const int n = static_cast<int>(s.project.midiMappings.size());
        row_of(MidiRow::MAPPING, "MAPPING", n > 0 ? "A: " + dec2(n) + " MAPPED" : "A: NONE YET");
    }
    row_of(MidiRow::PANIC, "PANIC", "A: ALL NOTES OFF");
    row_of(MidiRow::TEST,  "TEST",  "A: C-4 CH 1");

    // ── Status: PANIC and TEST succeed silently, so the press says it happened (or NO PORT) ──────
    if (!s.statusText.empty()) {
        const int statusY = firstRowY + midi_row_offset_y(MidiRow::TEST, ROW_HEIGHT) + ROW_HEIGHT * 2;
        c.draw_text(s.statusText, labelX, statusY + TEXT_PADDING, t.textTitle, CHAR_SPACING,
                    FONT_SCALE);
    }

    // Debug: the device buffer's length and the callback's average and worst share of it over the
    // last second. At 100 % the device runs dry.
    if (s.caps.debug) {
        const int lineY = firstRowY + midi_row_offset_y(MidiRow::TEST, ROW_HEIGHT) + ROW_HEIGHT * 3;
        c.draw_text(audio_load_text(s.audioLoad), labelX, lineY + TEXT_PADDING, t.textParam,
                    CHAR_SPACING, FONT_SCALE);
    }
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext MidiModule::cursor_context(const MidiState& s) const {
    if (s.cursorColumn == 0) return cc::read_only();   // the label — unreachable, as on PROJECT

    switch (static_cast<MidiRow>(s.cursorRow)) {
        case MidiRow::OUTPUT:
            // Index 0 = none. `enum_cycle`, not `index_cycle` (cursor.h).
            return cc::enum_cycle(s.deviceIndex, static_cast<int>(s.deviceNames.size()));

        case MidiRow::INPUT:
            return cc::enum_cycle(s.inDeviceIndex, static_cast<int>(s.inDeviceNames.size()));

        case MidiRow::OFFSET: {
            // ⚠️ `empty_value` out of range: −1 is an ordinary offset.
            CursorContext c = cc::hex_byte(midi_offset_in_force(s.settings, s.autoOffsetMs), -99, 99,
                                           /*empty_value=*/-1000);
            c.largeStep = 10;   // A+UP/DOWN walks it in tens, like TEMPO
            // ⚠️ AUTO is not empty: the dial starts from the derived number. A+B returns to AUTO.
            c.capabilities.canDelete = !s.settings.midiOffsetAuto;
            return c;
        }

        case MidiRow::SYNC:
            return cc::toggle_binary(s.settings.midiSyncOut);

        // Shows 01..16 over a stored 0..15. ⚠️ A cycle of seventeen, not a byte with an empty state —
        // an empty value the write-back rejects leaves A+D-PAD dead.
        case MidiRow::CTL_CH:
            return cc::enum_cycle(ctl_ch_index(s.settings.midiControlChannel), CTL_CH_OPTIONS);

        // MONO, then POLY 2..8; stored as the voice count.
        case MidiRow::KEYS:
            return cc::enum_cycle(clamp(s.settings.midiInVoices, 1, 8) - 1, 8);

        case MidiRow::PROG_CHG:
            return cc::toggle_binary(s.project.midiSendProgramChange);

        case MidiRow::VELOCITY:
            return cc::toggle_binary(s.settings.midiVelocity);

        // The action rows: plain A is the dispatcher's.
        case MidiRow::MAPPING:
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

    // Not an early return: OFFSET answers DELETE (A+B gives the row back to AUTO).
    const bool isSet = (action.type == ActionType::SET_VALUE);

    // The device rows write the name, not the index: the name is the choice.
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

        case MidiRow::OFFSET: {
            if (isSet) {
                const int ms = clamp(action.value, -99, 99);
                // ⚠️ A nudge landing on the stored number must still turn AUTO off.
                if (ms != settings.midiOffsetMs || settings.midiOffsetAuto) {
                    settings.midiOffsetMs   = ms;
                    settings.midiOffsetAuto = false;
                    r.offsetChanged         = true;
                }
            } else if (action.type == ActionType::DELETE && !settings.midiOffsetAuto) {
                // A+B gives the row back to the device.
                settings.midiOffsetAuto = true;
                r.offsetChanged         = true;
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

        case MidiRow::CTL_CH: {
            if (!isSet) break;
            settings.midiControlChannel =
                ctl_ch_stored(clamp(action.value, 0, CTL_CH_OPTIONS - 1));
            r.controlChannelChanged = true;
            break;
        }

        // Read by the frame loop every tick, so nothing is pushed from here.
        case MidiRow::KEYS:
            if (isSet) settings.midiInVoices = clamp(action.value, 0, 7) + 1;
            break;

        // Read by the frame loop every tick, like KEYS.
        case MidiRow::VELOCITY:
            if (isSet) settings.midiVelocity = (action.value != 0);
            break;

        case MidiRow::PROG_CHG:
            // ⚠️ …and this one dirties the song: it is a `Project` field. The cable rows are settings.
            if (!isSet) break;
            project.midiSendProgramChange = (action.value != 0);
            r.projectModified             = true;
            break;

        case MidiRow::MAPPING:
        case MidiRow::PANIC:
        case MidiRow::TEST:
            break;
    }

    return r;
}

}  // namespace pt::ui
