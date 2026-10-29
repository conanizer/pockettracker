#pragma once

// SETTINGS. Which rows exist comes from PlatformCaps, where they sit from settings_row_layout.h.
// ⚠️ The module edits indices and flags; what an index means (which layout, which .png) is the
// platform's, handed in as text — an empty text hides the row.
// ⚠️ Plain A only acts (THEME opens the editor, TEMPLATE saves/clears); values change with A+DPAD.

#include <string>
#include <vector>

#include "songcore/midi_map.h"   // MIDI_CTL_CH_ALL — what the CTL CH row starts on
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/platform_caps.h"
#include "ui/settings_row_layout.h"
#include "ui/theme.h"

namespace pt::ui {

// SETTINGS > HELP. ⚠️ The value is what settings.json stores — append, never reorder.
enum class HelpMode { OFF = 0, SHORT = 1, FULL = 2 };

// ONE HAND's MS values. The file stores the number, so these may change freely.
inline constexpr int ONE_HAND_HOLD_STEPS[] = {150, 200, 250, 300, 400, 500, 700, 1000};
inline constexpr int ONE_HAND_HOLD_COUNT   = sizeof(ONE_HAND_HOLD_STEPS) / sizeof(int);

// The step a stored number reads as: the first at or above it.
inline int one_hand_hold_index(int ms) {
    for (int i = 0; i < ONE_HAND_HOLD_COUNT; ++i)
        if (ms <= ONE_HAND_HOLD_STEPS[i]) return i;
    return ONE_HAND_HOLD_COUNT - 1;
}

// Everything SETTINGS edits; the shell round-trips it through settings.json. The counts sit beside
// the indices because only the platform knows a cycle's range.
struct SettingsValues {
    // ── The device rows (Android; the shell hides them) ──────────────────────────────────────────
    int  layoutIndex       = 0;
    int  layoutCount       = 1;
    int  skinIndex         = 0;
    int  skinCount         = 0;   // 0 = this layout is not skinned → no second column on LAYOUT

    // ⚠️ What persists is the stable id string (`portrait_skin`), not `skinIndex`; the shell
    // resolves it at boot (device_skin.h).
    std::string portraitSkin = "amiga-2";

    int  overlayIndex      = 0;   // 0 = "OFF"; 1.. = a file
    int  overlayCount      = 1;   // "OFF" + however many files
    int  overlayStrength   = 128;

    // ⚠️ Persisted as the id string too (`overlay_name`); "OFF" is none.
    std::string overlayName = "OFF";

    // ⚠️ First launch only — `load_settings` overwrites these. Button feedback is on (LO) so a
    // first press answers.
    bool buttonSoundEnabled = true;
    int  buttonSoundVolume  = 0x0F;
    bool buttonVibroEnabled = true;
    int  vibroPower         = 64;
    bool autosaveResumeAuto = false;

    // How the pad's face buttons are printed: 0 AUTO, 1 XBOX, 2 NINTENDO (ui::AbxyLayout).
    int  abxyIndex          = 0;

    // On-screen buttons even with a pad attached. Its own flag: the layout list changes length with
    // a pad, so an index would change meaning.
    bool touchButtonsWithPad = false;

    // ── METRONOME (every platform): a click per quarter note while playing, never exported ──────
    bool metronomeEnabled = false;
    int  metronomeVolume  = 0x80;

    // ── HELP — what SELECT shows: 0 OFF, 1 SHORT (panel), 2 FULL (overlay) ───────────────────────
    // ⚠️ This default reaches upgrading installs, so it is the long-standing behaviour.
    int  helpMode         = 1;

    // ── The rows every platform has ──────────────────────────────────────────────────────────────
    // Bilinear: integer scaling leaves black borders at anything but exact multiples of 640×480.
    bool scalingBilinear    = true;
    bool insertBefore       = true;
    bool cursorRemember     = false;
    bool notePreviewEnabled = true;
    bool oneHanded          = false;
    // ONE HAND's MS: how long a button is held alone before letting go locks it. Any number loads;
    // the row steps through ONE_HAND_HOLD_STEPS.
    int  oneHandHoldMs      = 300;

    // FOLDER = REMEMBER / REFRESH: REMEMBER opens a sample load at the last one's folder.
    bool        rememberFolder   = false;
    std::string lastSampleFolder;   // not saved

    // NAV = POOL / SONG. POOL: B+LEFT/RIGHT scroll the chain and phrase pools; SONG: B+D-pad walks
    // the arrangement. ⚠️ This default reaches existing installs.
    bool navSongRelative = true;

    // ── MIDI — the MIDI screen's, kept here for settings.json; this machine's cable, not the song's
    // ⚠️ The device is the name string, never an index (port lists reorder). "OFF" = none, "AUTO" =
    // the first plugged in.
    std::string midiOutDevice = "AUTO";

    // Opened at boot by `InputDispatcher::boot_midi_in_port`.
    std::string midiInDevice = "AUTO";

    // Signed ms; positive = MIDI leaves later than the audio.
    // ⚠️ Read through `midi_offset_in_force()`: under AUTO this is only the last dialled value.
    int         midiOffsetMs  = 0;

    // AUTO: from the latency the audio device reports — one buffer, so close rather than exact.
    bool        midiOffsetAuto = true;

    // SYNC OUT: 24 PPQN clock, transport and song position. OFF by default: a synth set to external
    // sync sits silent without clock.
    bool        midiSyncOut   = false;

    // SYNC IN: `songcore::MidiSyncInMode` — OFF, TEMPO, FULL. Read by the frame loop every tick.
    int         midiSyncIn    = 0;

    // CTL CH: the channel a mapping knob must arrive on (midi_map.h). −1 = OFF, else 0-15.
    // ⚠️ ALL is safe only because a CC is claimed, not reserved: one driving no mapping routes as
    // before.
    int         midiControlChannel = songcore::MIDI_CTL_CH_ALL;
    // A live key: 1 = MONO on the SONG cursor's track, 2..8 = POLY over that many tracks from it.
    int         midiInVoices = 4;
    // OFF plays every live key at full strength.
    bool        midiVelocity = true;
    // AUDIO OUT: the name persists; index and count are the cycle over the shell's list (0 = SYSTEM).
    std::string audioOutput   = "SYSTEM";
    int         audioOutIndex = 0;
    int         audioOutCount = 1;

    // ⚠️ VISUALIZER lives on the theme (`Theme::visualizerType`), carried across a theme change.

    // ── Debug ────────────────────────────────────────────────────────────────────────────────────
    bool traceEnabled = false;
};

struct SettingsState {
    const SettingsValues& values;

    int cursorRow    = 0;   // a SettingsRow, not a position on screen
    int cursorColumn = 1;

    // What the current index names on this platform. Braced defaults keep aggregate init warning-free.
    std::string layoutText{};
    std::string skinText{};
    std::string overlayText = "OFF";
    std::string audioOutText = "SYSTEM";
    std::string appVersion{};
    std::string themeName   = "CLASSIC";

    PlatformCaps caps{};
    Theme        theme = theme_classic();
};

struct SettingsInputResult {
    bool modified = false;
};

class SettingsModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    // In VisualizerType order.
    static const std::vector<std::string>& visualizer_names();

    void draw(Canvas& c, int x, int y, const SettingsState& s) const;

    CursorContext cursor_context(const SettingsState& s) const;

    // Writes into `values` (and `theme`, for VISUALIZER).
    SettingsInputResult handle_input(SettingsValues& values, Theme& theme, const PlatformCaps& caps,
                                     int cursor_row, int cursor_column,
                                     const InputAction& action) const;
};

}  // namespace pt::ui
