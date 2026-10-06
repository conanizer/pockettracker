#pragma once

// ─── SETTINGS ────────────────────────────────────────────────────────────────────────────────────
//
// Which rows exist is a function of PlatformCaps (platform_caps.h); where they sit is
// settings_row_layout.h. Neither is re-derived here.
//
// ⚠️ THE MODULE EDITS INDICES AND FLAGS; IT DOES NOT KNOW WHAT A LAYOUT MODE IS. LAYOUT is an enum
// cycle over `layoutCount` options; what an index MEANS (fullscreen vs portrait, which .png, how
// loud a click is) is the platform's. The display strings are handed in as text; where they are
// empty the rows are not drawn.
//
// ⚠️ SINGLE A IS RESERVED FOR ACTIONS: every value row changes with A+DPAD; plain A acts only on
// THEME (opens the editor) and TEMPLATE (SAVE / CLEAR).

#include <string>
#include <vector>

#include "songcore/midi_map.h"   // MIDI_CTL_CH_ALL — what the CTL CH row starts on
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/platform_caps.h"
#include "ui/settings_row_layout.h"
#include "ui/theme.h"

namespace pt::ui {

/** SETTINGS > HELP. ⚠️ The value is what settings.json stores — append, never reorder. */
enum class HelpMode { OFF = 0, SHORT = 1, FULL = 2 };

/**
 * Everything SETTINGS edits. Lives in AppState, and the shell round-trips it through settings.json.
 *
 * The counts sit beside the indices because an enum cycle's RANGE is part of its cursor context, and
 * only the platform knows it: a device with no physical buttons offers fewer layouts, and a Themes
 * folder with no .png in it offers no overlay but "OFF".
 */
struct SettingsValues {
    // ── The device rows (Android; the shell hides them) ──────────────────────────────────────────
    int  layoutIndex       = 0;
    int  layoutCount       = 1;
    int  skinIndex         = 0;
    int  skinCount         = 0;   // 0 = this layout is not skinned → no second column on LAYOUT

    // ⚠️ The PERSISTED skin is the STABLE ID STRING, not `skinIndex` — an index is meaningless
    // without its list. The shell resolves it at boot (device_skin.h) and writes it back on a change.
    // Serialized as `portrait_skin`.
    std::string portraitSkin = "amiga-2";

    int  overlayIndex      = 0;   // 0 = "OFF"; 1.. = a file
    int  overlayCount      = 1;   // "OFF" + however many files
    int  overlayStrength   = 128;

    // ⚠️ The PERSISTED overlay is the STABLE ID STRING too, resolved at boot (shell/skin/overlay.h).
    // Serialized as `overlay_name`; "OFF" is no overlay.
    std::string overlayName = "OFF";

    // ⚠️ THESE DEFAULTS ARE THE FIRST LAUNCH ONLY — `load_settings` overwrites them from
    // settings.json. Button feedback is ON so a stranger's first press answers; LO, since it is a
    // confirmation, not a noise.
    bool buttonSoundEnabled = true;
    int  buttonSoundVolume  = 0x0F;
    bool buttonVibroEnabled = true;
    int  vibroPower         = 64;
    bool autosaveResumeAuto = false;

    // Which way round the pad's face buttons are PRINTED: 0 AUTO, 1 XBOX, 2 NINTENDO (ui::AbxyLayout).
    // AUTO is a no-op, so an install without the key is unchanged.
    int  abxyIndex          = 0;

    // Show the on-screen buttons even with a physical pad attached (a phone with a clip-on pad). Its
    // own value, not `layoutIndex`: the layout list changes length with a pad, so an index would mean
    // different things plugged and unplugged. FALSE keeps existing installs unchanged.
    bool touchButtonsWithPad = false;

    // ── METRONOME — drawn with the cluster above, but on every platform ──────────────────────────
    //
    // A click on every quarter note while the transport runs, made by the engine and never exported.
    // VOL (00..FF, as BTN SOUND's) is drawn whether or not the toggle is on, so it can be set first.
    bool metronomeEnabled = false;
    int  metronomeVolume  = 0x80;

    // ── HELP — what a tap of SELECT shows: 0 OFF, 1 SHORT, 2 FULL (`HelpMode`) ───────────────────
    // SHORT is the compact panel in the visualizer's box, FULL the whole-screen overlay.
    // ⚠️ This default reaches upgrading installs (no key yet), so it is the long-standing SELECT
    // behaviour.
    int  helpMode         = 1;

    // ── The rows every platform has ──────────────────────────────────────────────────────────────
    // BILINEAR by default: integer scaling fills only exact multiples of 640×480 and leaves the
    // editor ringed with black everywhere else.
    bool scalingBilinear    = true;
    bool insertBefore       = true;
    bool cursorRemember     = false;
    bool notePreviewEnabled = true;

    // FOLDER = REMEMBER / REFRESH. REMEMBER opens a SAMPLE load at the folder of the last one.
    // ⚠️ Only `rememberFolder` persists; `lastSampleFolder` is session-only and resets every launch.
    bool        rememberFolder   = false;
    std::string lastSampleFolder;   // runtime only — see settings_store.cpp (not saved)

    // NAV = POOL / SONG. POOL: B+LEFT/RIGHT scroll the 00..FF chain and phrase pools. SONG: B+D-pad
    // walks the ARRANGEMENT, so the chain and phrase on screen are whatever the song cell names.
    //
    // ⚠️ THIS DEFAULT REACHES EXISTING INSTALLS, not just fresh ones: a settings.json without the key
    // falls through to it. Under SONG a phrase not placed in the song is unreachable; the NAV row is
    // the way back.
    bool navSongRelative = true;

    // ── MIDI ─────────────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ THESE ARE NOT ON THE SETTINGS SCREEN — they belong to the MIDI screen and live here because
    // settings.json round-trips this struct. They describe THIS MACHINE'S CABLE; what the SONG means
    // by MIDI (channel, bank, program, CC slots…) travels in the .ptp.
    //
    // ⚠️ THE DEVICE IS THE NAME STRING, NEVER AN INDEX: a port list reorders whenever anything is
    // plugged in. "OFF" is no device, "AUTO" takes the first one plugged in. The default reaches only
    // a new install (settings.json stores the key).
    std::string midiOutDevice = "AUTO";

    // The INPUT port: same kind, same rule, same "OFF" and "AUTO". Read at boot, where it OPENS the
    // port (`InputDispatcher::boot_midi_in_port`) — one owner of which port is open.
    std::string midiInDevice = "AUTO";

    // Signed ms; positive = MIDI leaves LATER than the audio. A message leaves when its block is
    // handed to the device, so the cable runs ahead of the speakers by the output latency.
    // ⚠️ READ IT THROUGH `midi_offset_in_force()`, NEVER DIRECTLY: under AUTO this is the value the
    // user dialled last, not the one in force.
    int         midiOffsetMs  = 0;

    // AUTO: derive the offset from what the audio device says it holds. ⚠️ It sees one buffer; a
    // driver queuing more behind it is not in it, so this lands close rather than exact.
    bool        midiOffsetAuto = true;

    // SYNC OUT — 24 PPQN clock, Start/Stop/Continue and song position.
    // ⚠️ A setting, not the project's: whether a drum machine is waiting for the tempo is a fact
    // about the desk. Default OFF: a synth set to external sync sits silent until it gets clock.
    bool        midiSyncOut   = false;

    // CTL CH — the channel a knob must arrive on to be a MAPPING knob (midi_map.h). −1 = OFF, else
    // 0-15 (shown 01-16). A setting, while the mappings are the song's.
    // ⚠️ Without it one knob does two jobs: a CC is already routed to the track naming its channel.
    // ⚠️⚠️ ALL by default is safe only because a CC is CLAIMED, NOT RESERVED (midi_map.h): one that
    // drives a mapping is consumed, one that drives nothing routes as before.
    int         midiControlChannel = songcore::MIDI_CTL_CH_ALL;
    // How a live key plays: 1 = MONO on the SONG cursor's track, 2..8 = POLY over that many tracks
    // from it. The instrument is the one the UI is on.
    int         midiInVoices = 4;
    // OFF plays every live key at full strength, whatever the keyboard sends.
    bool        midiVelocity = true;
    // AUDIO OUT. The NAME is the choice and what settings.json keeps; the index and count are the
    // row's cycle over the list the platform supplies (0 = SYSTEM), filled in by the shell.
    std::string audioOutput   = "SYSTEM";
    int         audioOutIndex = 0;
    int         audioOutCount = 1;

    // ⚠️ VISUALIZER is NOT here: it lives on the THEME (`Theme::visualizerType`) and is carried
    // across a theme change — the palette is the theme's, the visualizer the user's. Hence `Theme&`.

    // ── Debug ────────────────────────────────────────────────────────────────────────────────────
    bool traceEnabled = false;
};

struct SettingsState {
    const SettingsValues& values;

    int cursorRow    = 0;   // a SettingsRow — the row's NUMBER, not its position on this platform
    int cursorColumn = 1;

    // Text the module paints but does not own: what the current index NAMES on this platform.
    // Braced defaults on all four, or aggregate init warns under -Wmissing-field-initializers.
    std::string layoutText{};
    std::string skinText{};
    std::string overlayText = "OFF";
    std::string audioOutText = "SYSTEM";
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

    /** SCOPE / FLAT / OCTA / OCTA.F / SPECT / SPCT.P — the six, in VisualizerType order. */
    static const std::vector<std::string>& visualizer_names();

    void draw(Canvas& c, int x, int y, const SettingsState& s) const;

    CursorContext cursor_context(const SettingsState& s) const;

    /** Writes straight into `values` (and, for VISUALIZER, into `theme`). */
    SettingsInputResult handle_input(SettingsValues& values, Theme& theme, const PlatformCaps& caps,
                                     int cursor_row, int cursor_column,
                                     const InputAction& action) const;
};

}  // namespace pt::ui
