#pragma once

// ─── MIDI ────────────────────────────────────────────────────────────────────────────────────────
//
//
// The cable (ports, offset, sync, control channel), PROG CHG, how a live keyboard plays, the mapping
// list, and the PANIC / TEST actions. Rows: settings_row_layout.h `MidiRow`.
// ── ⚠️ WHAT THIS MODULE DOES NOT OWN ─────────────────────────────────────────────────────────────
//
// It never opens, closes or writes to a port. It edits an index into a list of names it was HANDED;
// the dispatcher, the only place that reaches `songcore::IMidiOut`, turns a changed index into
// `close()` + `open()`. Enumerating devices means asking the OS, and pt-ui has no OS in it.
//
// ⭐ The OUTPUT row shows what is OPEN, not what was WANTED: a saved device not plugged in today
// resolves to index 0 and reads OFF.

#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/modules/settings_editor.h"   // SettingsValues — OUTPUT and OFFSET live there
#include "ui/platform_caps.h"
#include "ui/settings_row_layout.h"
#include "ui/theme.h"

namespace pt::ui {

/**
 * The audio callback's cost over the last second, for the debug build's BUF/CPU line. Loads are in
 * tenths of a percent of the block's own duration; all zero until the device has played a second.
 */
struct AudioLoad {
    int blockFrames = 0;
    int sampleRate  = 0;
    int meanLoad    = 0;
    int worstLoad   = 0;
};

// A port list is OFF, AUTO, then the devices. AUTO takes the first device plugged in and follows
// it through unplug/replug; a device NAME is kept for that device alone.
constexpr int         MIDI_FIRST_PORT = 2;
inline constexpr char MIDI_PORT_OFF[]  = "OFF";
inline constexpr char MIDI_PORT_AUTO[] = "AUTO";

struct MidiState {
    /** PROG CHG lives on the project — it is what the SONG means, so it travels in the .ptp. */
    const songcore::Project& project;

    /** OUTPUT and OFFSET live in the settings — they describe this machine's cable. */
    const SettingsValues& settings;

    /**
     * The port lists, always with "OFF" and "AUTO" in front, so "no device" is not a separate state.
     * ⚠️ IN AND OUT ARE SEPARATE LISTS: a port that is both sits at a different index in each.
     * By reference: this struct is rebuilt every frame and every press.
     */
    const std::vector<std::string>& deviceNames;
    const std::vector<std::string>& inDeviceNames;

    /** The device actually open in each direction, "" for none — what the AUTO entry shows. */
    std::string outOpenName{};
    std::string inOpenName{};

    /**
     * What the OFFSET row uses while AUTO is on: the output latency the audio device reported at
     * boot, in milliseconds. A platform fact, so it arrives the same way the port lists above do —
     * pt-ui has no audio backend to ask.
     */
    int autoOffsetMs = 0;

    int cursorRow    = 0;   // a MidiRow
    int cursorColumn = 1;   // always 1: column 0 is the row LABEL and is unreachable, as on PROJECT

    /** Indices into the two lists above. */
    int deviceIndex   = 0;
    int inDeviceIndex = 0;

    /** A one-shot readout under the actions — "PANIC SENT", "TEST SENT", "NO PORT". */
    std::string statusText{};

    /**
     * The channel the cable last carried a CC on, or −1. `CTL CH` prints it while OFF, so the
     * controller tells the user the number the row asks for.
     */
    int lastCcChannel = -1;

    /** Drawn on a debug build only — it is a diagnostic, not a setting. */
    AudioLoad audioLoad{};

    PlatformCaps caps{};
    Theme        theme = theme_classic();
};

/**
 * The offset the cable is actually sent with — derived under AUTO, dialled otherwise — clamped to
 * the row's range. ⭐ Every reader goes through here, so none has to remember the flag.
 */
inline int midi_offset_in_force(const SettingsValues& s, int autoOffsetMs) {
    if (!s.midiOffsetAuto) return s.midiOffsetMs;
    return autoOffsetMs < -99 ? -99 : (autoOffsetMs > 99 ? 99 : autoOffsetMs);
}

struct MidiInputResult {
    bool projectModified = false;   // PROG CHG — the row that dirties the SONG
    bool deviceChanged   = false;   // OUTPUT — the dispatcher must now (re)open a port
    bool inDeviceChanged = false;   // INPUT  — likewise, and the sink goes with it
    bool offsetChanged   = false;   // OFFSET — the dispatcher must push it to the consumer
    bool syncChanged     = false;   // SYNC   — likewise; and turning it OFF owes the device a Stop
    bool controlChannelChanged = false;  // CTL CH — the host must be told which channel carries knobs
};

class MidiModule {
  public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const MidiState& s) const;

    CursorContext cursor_context(const MidiState& s) const;

    /**
     * Writes into BOTH subjects: PROG CHG is the project's, OUTPUT/OFFSET the settings'. PANIC and
     * TEST are absent — plain-A actions that reach hardware, so the dispatcher's.
     */
    MidiInputResult handle_input(songcore::Project& project, SettingsValues& settings,
                                 int cursor_row, int cursor_column,
                                 const std::vector<std::string>& device_names,
                                 const std::vector<std::string>& in_device_names,
                                 const InputAction& action) const;
};

}  // namespace pt::ui
