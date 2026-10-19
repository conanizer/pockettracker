#pragma once

// MIDI: the cable (ports, offset, sync, control channel), PROG CHG, live keys, the mapping list,
// PANIC / TEST. Rows: `MidiRow` in settings_row_layout.h.
// ⚠️ It never touches a port: it edits an index into names it was handed, and the dispatcher turns
// that into close + open. The OUTPUT row shows what is open — a saved device not plugged in reads OFF.

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

// The audio callback's cost over the last second, for the debug BUF/CPU line: tenths of a percent of
// a block's duration; zero until a second has played.
struct AudioLoad {
    int blockFrames = 0;
    int sampleRate  = 0;
    int meanLoad    = 0;
    int worstLoad   = 0;
};

// A port list is OFF, AUTO, then the devices. AUTO takes the first plugged in and follows it through
// replugs; a device name is kept for that device alone.
constexpr int         MIDI_FIRST_PORT = 2;
inline constexpr char MIDI_PORT_OFF[]  = "OFF";
inline constexpr char MIDI_PORT_AUTO[] = "AUTO";

struct MidiState {
    // PROG CHG is the song's (.ptp).
    const songcore::Project& project;

    // OUTPUT and OFFSET are this machine's cable (settings).
    const SettingsValues& settings;

    // "OFF" and "AUTO" in front. ⚠️ In and out are separate lists: one port sits at different indices.
    const std::vector<std::string>& deviceNames;
    const std::vector<std::string>& inDeviceNames;

    // The device open in each direction, "" for none — what AUTO shows.
    std::string outOpenName{};
    std::string inOpenName{};

    // OFFSET under AUTO: the output latency the audio device reported at boot, in ms.
    int autoOffsetMs = 0;

    int cursorRow    = 0;   // a MidiRow
    int cursorColumn = 1;   // always 1: column 0 is the label

    int deviceIndex   = 0;
    int inDeviceIndex = 0;

    // "PANIC SENT", "TEST SENT", "NO PORT".
    std::string statusText{};

    // The channel the last CC came on, or −1; `CTL CH` prints it so the user learns the number.
    int lastCcChannel = -1;

    // An incoming clock's tempo, 0 while none arrives; `SYNC IN` prints it.
    double clockBpm = 0.0;

    // Debug builds only.
    AudioLoad audioLoad{};

    PlatformCaps caps{};
    Theme        theme = theme_classic();
};

// The offset the cable is sent with (derived under AUTO, else dialled), clamped. Every reader comes
// through here.
inline int midi_offset_in_force(const SettingsValues& s, int autoOffsetMs) {
    if (!s.midiOffsetAuto) return s.midiOffsetMs;
    return autoOffsetMs < -99 ? -99 : (autoOffsetMs > 99 ? 99 : autoOffsetMs);
}

struct MidiInputResult {
    bool projectModified = false;   // PROG CHG — dirties the song
    bool deviceChanged   = false;   // OUTPUT — the dispatcher (re)opens a port
    bool inDeviceChanged = false;   // INPUT  — likewise, and the sink with it
    bool offsetChanged   = false;   // OFFSET — pushed to the consumer
    bool syncChanged     = false;   // SYNC   — likewise; turning it OFF owes the device a Stop
    bool controlChannelChanged = false;  // CTL CH — the host is told
};

class MidiModule {
  public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const MidiState& s) const;

    CursorContext cursor_context(const MidiState& s) const;

    // Writes the project (PROG CHG) and the settings. PANIC and TEST are the dispatcher's.
    MidiInputResult handle_input(songcore::Project& project, SettingsValues& settings,
                                 int cursor_row, int cursor_column,
                                 const std::vector<std::string>& device_names,
                                 const std::vector<std::string>& in_device_names,
                                 const InputAction& action) const;
};

}  // namespace pt::ui
