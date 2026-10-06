// The MIDI screen and its mapping list: their buttons.

#include "ui/dispatch/dispatch_common.h"

#include "ui/navigation.h"

#include <algorithm>

namespace pt::ui {

// ─── The buttons ─────────────────────────────────────────────────────────────────────────────────

// B is the only way out of either: neither is on the nav grid.
GestureResult InputDispatcher::midi_screen(Gesture g) {
    switch (g) {
        case Gesture::A: midi_action();                  return GestureResult::TAKEN;
        case Gesture::B: leave_to(s_.midiReturnScreen);  return GestureResult::TAKEN;
        default:         return GestureResult::PASS;
    }
}

GestureResult InputDispatcher::midi_map_screen(Gesture g) {
    switch (g) {
        case Gesture::A: midi_map_action();                 return GestureResult::TAKEN;
        case Gesture::B: leave_to(s_.midiMapReturnScreen);  return GestureResult::TAKEN;

        // A mapping's GROUP and PARAMETER cells open the destination picker rather than stepping.
        case Gesture::A_UP:
        case Gesture::A_DOWN:
            if (!on_map_dest_cell()) return GestureResult::PASS;
            open_map_picker();
            return GestureResult::TAKEN;

        default: return GestureResult::PASS;
    }
}

// ─── What A does ─────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::midi_action() {
    switch (static_cast<MidiRow>(s_.midiCursorRow)) {
        case MidiRow::PANIC:
            // Every note-off owed, through the consumer (it knows what is sounding) — the same panic
            // SongcoreHost::stop() sends.
            host_.midi_out().panic();
            s_.midiStatusText = port_open() ? "PANIC SENT" : "NO PORT";
            break;

        case MidiRow::TEST: {
            // ⚠️ TEST writes to the port directly, bypassing the sequencer on purpose: a silent TEST
            // must mean "no cable", not "something, somewhere". The note-off follows at once, so this
            // screen cannot leave a note hanging.
            if (!port_open()) { s_.midiStatusText = "NO PORT"; break; }
            const uint8_t on[3]  = {0x90, 60, 100};   // C-4, channel 1, mf
            const uint8_t off[3] = {0x80, 60, 0};
            s_.midiOut->send(on, 3);
            s_.midiOut->send(off, 3);
            s_.midiStatusText = "TEST SENT";
            break;
        }

        // The door into the mapping list: only the cursor needs putting back inside the list.
        case MidiRow::MAPPING: {
            clamp_midi_map_cursor();
            s_.midiMapReturnScreen = s_.currentScreen;
            NavResult nav;
            nav.screen = ScreenType::MIDI_MAP;
            nav.column = s_.previousColumn;
            go_to_screen(s_, nav);
            break;
        }

        // OUTPUT / OFFSET / PROG CHG are A+DPAD cells — the app-wide rule that single A is for actions.
        default:
            break;
    }
}

void InputDispatcher::clamp_midi_map_cursor() {
    const songcore::Project& p    = host_.project();
    const int                rows = midi_map_row_count(p);   // always ≥ 1 — the ADD row
    s_.midiMapCursorRow    = std::clamp(s_.midiMapCursorRow, 0, rows - 1);
    s_.midiMapCursorColumn = midi_map_clamp_column(p, s_.midiMapCursorRow, s_.midiMapCursorColumn);
}

void InputDispatcher::midi_map_action() {
    // Only the ADD row answers a bare A; the cells above are A+DPAD cells.
    songcore::Project& p = host_.edit_project();
    if (s_.midiMapCursorRow != static_cast<int>(p.midiMappings.size())) return;
    if (static_cast<int>(p.midiMappings.size()) >= songcore::MIDI_MAP_MAX) return;

    // A new row starts on the catalogue's first destination (track 1's fader, which every project
    // has) across its whole range, so it points at something real from the first frame.
    songcore::MidiMapping m;
    m.controller = 0;
    m.dest       = static_cast<uint8_t>(songcore::MAP_DESTS[0].id);
    m.scopeIndex = 0;
    m.rangeMin   = songcore::MAP_DESTS[0].min;
    m.rangeMax   = songcore::MAP_DESTS[0].max;
    p.midiMappings.push_back(m);

    // The cursor stays put, so it is now on the new mapping with the ADD row one below.
    mark_modified();
}

// ─── The cursor and the edit ─────────────────────────────────────────────────────────────────────

CursorContext InputDispatcher::midi_context() const {
    MidiState ms{*s_.project, s_.settings, s_.midiDeviceNames, s_.midiInDeviceNames};
    ms.lastCcChannel  = s_.midiInCcChannel;
    ms.cursorRow      = s_.midiCursorRow;
    ms.cursorColumn   = s_.midiCursorColumn;
    ms.deviceIndex    = s_.midiDeviceIndex;
    ms.inDeviceIndex  = s_.midiInDeviceIndex;
    ms.autoOffsetMs   = s_.midiAutoOffsetMs;
    ms.caps           = s_.caps;
    return midi_.cursor_context(ms);
}

// ⚠️ MIDI edits BOTH: PROG CHG is a Project field (dirties the song, like TEMPO); OUTPUT, INPUT
// and OFFSET are settings.json's and must not.
bool InputDispatcher::midi_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    const MidiInputResult r =
        midi_.handle_input(p, s_.settings, s_.midiCursorRow, s_.midiCursorColumn,
                           s_.midiDeviceNames, s_.midiInDeviceNames, action);
    // The side effects the module cannot perform — it has no port.
    if (r.deviceChanged)   apply_midi_device();
    if (r.inDeviceChanged) apply_midi_in_device();
    if (r.offsetChanged)   host_.set_midi_offset_ms(
                               midi_offset_in_force(s_.settings, s_.midiAutoOffsetMs));
    if (r.syncChanged)     host_.set_midi_sync_out(s_.settings.midiSyncOut);
    if (r.controlChannelChanged)
        host_.set_midi_control_channel(s_.settings.midiControlChannel);
    return r.projectModified;
}

CursorContext InputDispatcher::midi_map_context() const {
    const Project& p = *s_.project;
    MidiMapState mm{p};
    mm.cursorRow    = s_.midiMapCursorRow;
    mm.cursorColumn = s_.midiMapCursorColumn;
    return midiMap_.cursor_context(mm);
}

// ⚠️ Mappings are the SONG's, so every edit dirties it. Nothing is pushed: a mapping moves no
// parameter until a knob turns.
bool InputDispatcher::midi_map_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    const MidiMapInputResult r = midiMap_.handle_input(p, s_.midiMapCursorRow,
                                                       s_.midiMapCursorColumn, action);
    // A delete leaves the cursor one past the end — the ADD row, where it should land.
    if (r.rowDeleted) clamp_midi_map_cursor();
    return r.modified;
}

}  // namespace pt::ui
