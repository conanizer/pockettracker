#pragma once

// ─── MIDI ────────────────────────────────────────────────────────────────────────────────────────
//
// The screen the MIDI plan's §8.1 asks for, and the increment (B4.3) that finally makes the EXTERNAL
// bus reachable without an environment variable. B4.1 gave the instrument a TYPE, B4.2 gave it a patch
// page — but the CABLE was still picked by `POCKETTRACKER_MIDI_OUT`, which means the feature shipped in
// a state where nobody could reach it. This is the row that ends that.
//
// It has NO Kotlin twin and never will: MIDI out did not exist before the port. So — like `ptmidi` and
// unlike every other module here — there is no golden to compare against and no "does it still match
// Kotlin" question to ask. What can be checked is the same thing `ptdispatch` checks everywhere else:
// that the cursor reaches every row, that each row's context is the one its value needs, and that a
// press changes what it claims to change.
//
// ── ⚠️ WHAT THIS MODULE DOES NOT OWN ─────────────────────────────────────────────────────────────
//
// **It never opens, closes or writes to a port.** It edits an index into a list of names it was HANDED,
// and the dispatcher — the only place that can reach `songcore::IMidiOut` — turns a changed index into
// `close()` + `open()`. That is `settings_editor.h`'s rule ("the module edits indices and flags; it
// does not know what a layout mode is") applied to the one seam where getting it wrong would be worse
// than untidy: enumerating devices means asking the OS, and pt-ui is the layer with no OS in it.
//
// It is also why OUTPUT's displayed value is `deviceNames[deviceIndex]` and not the setting string.
// ⭐ **The row shows what is OPEN, not what was WANTED.** A saved device that is not plugged in today
// resolves to index 0 and the row reads OFF — which is the truth, where painting the remembered name
// would be a screen quietly lying about whether a cable exists.

#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/modules/settings_editor.h"   // SettingsValues — OUTPUT and OFFSET live there (§7)
#include "ui/platform_caps.h"
#include "ui/settings_row_layout.h"
#include "ui/theme.h"

namespace pt::ui {

struct MidiState {
    /** PROG CHG lives on the project — it is what the SONG means, so it travels in the .ptp. */
    const songcore::Project& project;

    /** OUTPUT and OFFSET live in the settings — they describe this machine's cable. */
    const SettingsValues& settings;

    int cursorRow    = 0;   // a MidiRow
    int cursorColumn = 1;   // 1 only; column 0 is the label and is unreachable, as on PROJECT

    /**
     * The port list, ALWAYS with "OFF" at index 0 — the dispatcher builds it by asking the platform
     * and prepending that entry, so the module never has to special-case "no device" as a separate
     * state. Empty is impossible; a machine with no ports still gets `{"OFF"}`.
     */
    std::vector<std::string> deviceNames{"OFF"};
    int                      deviceIndex = 0;

    /** A one-shot readout under the actions — "PANIC SENT", "TEST SENT", "NO PORT". */
    std::string statusText{};

    PlatformCaps caps{};
    Theme        theme = theme_classic();
};

struct MidiInputResult {
    bool projectModified = false;   // PROG CHG — the only row that dirties the SONG
    bool deviceChanged   = false;   // OUTPUT — the dispatcher must now (re)open a port
    bool offsetChanged   = false;   // OFFSET — the dispatcher must push it to the consumer
};

class MidiModule {
  public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const MidiState& s) const;

    CursorContext cursor_context(const MidiState& s) const;

    /**
     * Writes into BOTH subjects, because this screen genuinely edits both: PROG CHG is the project's
     * and OUTPUT/OFFSET are the settings'. Splitting it into two calls would only move the decision of
     * which one a row belongs to out of the file that knows.
     *
     * PANIC and TEST are absent: they are plain-A ACTIONS and reach hardware, so they live in the
     * dispatcher exactly as SAVE / LOAD / NEW do on PROJECT.
     */
    MidiInputResult handle_input(songcore::Project& project, SettingsValues& settings,
                                 int cursor_row, int cursor_column,
                                 const std::vector<std::string>& device_names,
                                 const InputAction& action) const;
};

}  // namespace pt::ui
