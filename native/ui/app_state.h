#pragma once

// Everything the screens draw from that is not the project: cursors, the screen that is up, which
// items are being looked at, the playheads read each frame.
// ⚠️ The draw path takes this struct and nothing else; a new screen adds its fields here.

#include "screen.h"
#include "table-lanes.h"
#include "songcore/model.h"
#include "theme.h"
#include "ui/folder_config.h"
#include "ui/fx_helper.h"
#include "ui/map_picker.h"
#include "ui/modules/confirm_dialog.h"
#include "ui/modules/effects_editor.h"
#include "ui/modules/eq_editor.h"
#include "ui/modules/file_browser.h"
#include "ui/modules/qwerty_keyboard.h"
#include "ui/modules/render_dialog.h"
#include "ui/modules/sample_editor.h"
#include "ui/modules/midi_settings.h"   // AudioLoad
#include "ui/modules/theme_editor.h"
#include "ui/modules/settings_editor.h"
#include "ui/platform_caps.h"
#include "ui/playhead.h"
#include "ui/selection.h"

#include "songcore/midi_in.h"    // IMidiIn
#include "songcore/midi_out.h"   // IMidiOut

#include <cstdint>
#include <string>
#include <vector>

namespace pt::ui {

class Clipboard;   // only pointed at, for the readout

struct AppState {
    // ⚠️ The one Project the host owns, edited in place (`host.edit_project()`) — never a copy.
    songcore::Project* project = nullptr;

    // ── Navigation ───────────────────────────────────────────────────────────────────────────────
    ScreenType currentScreen = ScreenType::SONG;

    // The grid column a shared screen (PROJECT / MIXER / EFFECTS) was entered from, for R+UP back.
    int previousColumn = 2;

    // INSTRUMENT reached from the pool: R+LEFT goes back to the pool, not PHRASE.
    bool instrumentFromPool = false;

    // SONG / CHAIN / PHRASE share this cursor; on SONG `cursorColumn` is the track, 1..8.
    int cursorRow    = 0;
    int cursorColumn = 1;

    int tableCursorRow    = 0;
    int tableCursorColumn = 1;  // starts on transpose
    int grooveCursorRow   = 0;
    // GROOVE: the tick grid and the side panel keep separate rows, so the swing readout stays on the
    // tick cursor while QNT is set. `grooveCursorColumn` says which has the cursor.
    int grooveCursorColumn = 1;
    int groovePanelRow     = 0;
    int groovePanelColumn  = 0;  // the SAVE/LOAD row only
    // How A+DPAD edits a groove step. Never saved; OFF at start and on project load.
    int grooveQuantize     = 0;
    int scaleCursorRow    = 0;
    // SCALE's NAME row only; selection and clipboard see one column of degrees.
    int scaleCursorColumn = 0;

    // Walks the row table in ui/instrument_row_layout.h.
    int instrumentCursorRow    = 0;
    int instrumentCursorColumn = 1;

    // INST.POOL: the row is `currentInstrument` itself.
    int poolCursorColumn = 0;

    // MODS: four slots as two pairs; the slot is `pair * 2 + side`.
    int modCursorRow  = 0;
    int modCursorPair = 0;  // 0 = MOD1+MOD2, 1 = MOD3+MOD4
    int modCursorSide = 0;  // 0 = left, 1 = right

    // MIXER: not a grid (ui/mixer_cell_layout.h).
    int mixerCursorColumn = 0;  // 0..7 = tracks, 8 = master
    int mixerMasterRow    = 0;  // 0 = volumes, 1 = sends / EQ, 2 = OTT|DUST, 3 = LIM

    // EFFECTS: a row is one cell (ui/effects_row_layout.h).
    int effectsCursorRow = 0;

    // The shared cursor on leaving each of its three screens — their column counts differ.
    int songCursorRow = 0,   songCursorColumn = 1;
    int chainCursorRow = 0,  chainCursorColumn = 1;
    int phraseCursorRow = 0, phraseCursorColumn = 1;

    // PROJECT: column 0 is the label (ui/settings_row_layout.h).
    int projectCursorRow    = 0;
    int projectCursorColumn = 1;

    int midiCursorRow    = 0;
    int midiCursorColumn = 1;

    // MIDI MAPPING: one row per mapping plus ADD; clamped on the way in and after every edit.
    int midiMapCursorRow    = 0;
    int midiMapCursorColumn = 1;

    // SETTINGS: the row is a SettingsRow (its number), not a position in the filtered list.
    int settingsCursorRow    = 0;
    int settingsCursorColumn = 1;

    // SONG's first visible row, 0..240 (16 of 256 shown).
    int songScrollPosition = 0;

    // Which slot of each pool is being edited.
    int currentPhrase     = 0;
    int currentChain      = 0;
    int currentInstrument = 0;
    int currentTable      = 0;
    int currentGroove     = 0;
    int currentScale      = 0;   // which of the 16 slots the SCALE screen is showing, 0-15

    // ── Playback, read back each frame ───────────────────────────────────────────────────────────
    // One playhead per track, each independent (ui/playhead.h).
    bool          isPlaying    = false;
    TrackPlayhead playheads[8] = {};

    // ── LIVE mode — never saved ──────────────────────────────────────────────────────────────────
    // ⚠️ A readback of the sequencer's mode; input asks the host, never this.
    bool     liveMode     = false;
    LiveQueue liveQueue[8] = {};

    // The queue markers' blink phase, 0..999 ms — handed in so a headless screenshot is repeatable.
    int blinkPhaseMs = 0;

    // The TABLE row a voice is on, per FX column (−1 = not running). Read off the voice: a table runs
    // on its own clock, each column separately (ui/engine_feed.h).
    int tablePlaybackRows[TABLE_LANES] = {-1, -1, -1};

    // What each track is sounding, from the engine's voices (the note monitor).
    songcore::Note trackNotes[8] = {};

    // The SoundFont preset of `currentInstrument`, read from the engine once a frame.
    std::string sfPresetName  = "---";
    int         sfPresetCount = 0;
    int         sfPresetIndex = 0;

    // ── The visualizer: filled once a frame; null is silence ─────────────────────────────────────
    const float* waveform       = nullptr;  // WAVEFORM_SIZE master samples
    const float* trackWaveforms = nullptr;  // TRACK_WAVEFORM_COUNT × WAVEFORM_SIZE, flat (OCTA)
    const float* spectrum       = nullptr;  // NUM_BARS magnitudes (SPECTRUM modes)

    // Bit N set once track N has had a note scheduled this phrase.
    int  trackMask         = 0;
    // The preview lane had audio last block (OCTA lights its scope only while stopped).
    bool previewLaneActive = false;

    // ── The MIXER's meters ───────────────────────────────────────────────────────────────────────
    // Read only while MIXER is up, every 60 ms: the read takes a mutex the audio callback also takes.
    // The peak hold counts `peaksVersion`, not frames.
    float    trackPeaks[16] = {};   // L/R per track
    float    masterPeaks[2] = {};
    float    sendPeaks[4]   = {};   // revL, revR, delL, delR
    unsigned peaksVersion   = 0;

    Selection selection{};

    bool selection_mode() const { return selection.active; }
    bool is_cell_selected(int row, int column) const {
        return selection.is_cell_selected(row, column);
    }

    // What L+R clears first — whichever was touched last; one thing per press.
    enum class Clearable { NONE, SELECTION, MUTE };
    Clearable lastClearable = Clearable::NONE;

    // The dispatcher's clipboard, for the readout; null with no input layer.
    const Clipboard* clipboard = nullptr;

    FxHelperState fxHelper{};     // ui/fx_helper.h
    MapPickerState mapPicker{};   // ui/map_picker.h

    FileBrowserState fileBrowser{};

    // What A does with the picked file.
    enum class BrowserPurpose {
        LOAD_SOURCE,        // a sample (or an .sf2 — the instrument's TYPE decides, at open time)
        LOAD_PRESET,        // a .pti into the current instrument slot
        LOAD_SAMPLE_EDITOR, // a .wav into the slot the SAMPLE EDITOR is open on — and back to it
        LOAD_PROJECT,       // a .ptp — the whole document (PROJECT's LOAD)
        LOAD_THEME,         // a .ptt — and back into the THEME EDITOR that raised the browser
        LOAD_SCALE,         // a .pts into the slot the SCALE screen is showing
        LOAD_GROOVE,        // a .ptg into the slot the GROOVE screen is showing
        LOAD_REVERB,        // a .ptr into EFFECTS' reverb section
        LOAD_DELAY          // a .ptd into EFFECTS' delay section
    };
    BrowserPurpose browserPurpose = BrowserPurpose::LOAD_SOURCE;

    // The screen the browser (or a full-screen overlay) returns to; every overlay open writes it.
    ScreenType previousScreen = ScreenType::PROJECT;

    // Where B goes from SETTINGS. ⚠️ Not `previousScreen`, which every browser open moves.
    // Only PROJECT → SYSTEM writes it.
    ScreenType settingsReturnScreen = ScreenType::PROJECT;

    ScreenType midiReturnScreen = ScreenType::PROJECT;   // the same for MIDI…
    ScreenType midiMapReturnScreen = ScreenType::MIDI;   // …and the mapping list

    // ── MIDI ─────────────────────────────────────────────────────────────────────────────────────
    // The output port; null with no backend, and every use is guarded.
    songcore::IMidiOut* midiOut = nullptr;

    // The port list after "OFF" and "AUTO", rebuilt on every entry to the screen (hot-plug).
    std::vector<std::string> midiDeviceNames{"OFF", "AUTO"};
    int                      midiDeviceIndex = 0;
    // The device actually open, "" for none — under AUTO it differs from the setting.
    std::string              midiOutOpenName;

    // The input port and its own list; may be null, every use guarded.
    songcore::IMidiIn*       midiIn = nullptr;
    std::vector<std::string> midiInDeviceNames{"OFF", "AUTO"};
    int                      midiInDeviceIndex = 0;
    std::string              midiInOpenName;

    // "PANIC SENT", "TEST SENT", "NO PORT".
    std::string midiStatusText;

    // The audio output latency in ms, for OFFSET's AUTO. Set by the shell, never saved.
    int midiAutoOffsetMs = 0;

    // The channel of the last CC in, or −1. Copied in `set_now`: the MIDI screen is built in two places.
    int midiInCcChannel = -1;

    // The tempo of an incoming MIDI clock, 0 while none arrives. Copied in `set_now`.
    double midiClockBpm = 0.0;

    // The audio callback's cost, copied in `set_now`.
    AudioLoad audioLoad{};

    QwertyKeyboardState qwerty{};

    // The sample editor's session, from open to close: selection, transients, pending pitch, the
    // waveform's min/max pairs. The audio itself is in the engine.
    SampleEditorState sampleEditor{};

    // One state for every confirm (ui/modules/confirm_dialog.h).
    ConfirmDialogState confirm{};

    // ── A load in flight — a strip across the top, never a modal (ui/modules/loading_strip.h) ──
    struct LoadingState {
        bool running = false;   // owns every button from the first moment
        bool shown = false;     // has outlasted LOADING_DELAY_MS, so the strip is up
        // 0..1, or < 0 when the file states no total (load_progress.h).
        float progress = -1.0f;
        std::string detail{};   // the file's or the project's name
        // Since the load opened; the only moving part without a percentage, so a hang looks different.
        int elapsedMs = 0;
        bool cancelRequested = false;   // B; the engine unwinds on the tick's return
    };
    LoadingState loading{};

    // ── The EQ editor: owns the D-pad, A, B and SELECT; START passes through ─────────────────────
    // ⚠️ `eq.caller` is captured on open: B+LEFT/RIGHT writes the new slot back into the cell that asked.
    EqEditorState eq{};

    // The spectrum of what the open EQ sits on (master, a send's input or an instrument — `eq.caller`
    // picks). Not the visualizer's `spectrum`, which is always the master.
    const float* eqSpectrum      = nullptr;
    int          eqSpectrumCount = 0;

    // The device rate, so the curve plots at the rate the bands were built at.
    int          eqSampleRate    = 44100;

    // ── The send editor: the reverb's or delay's cells in a box; START passes through ────────────
    // ⚠️ It stays open under the browser its LOAD raises and under its EQ editor — ask
    // `send_editor_up` / `send_editor_drawn`, not `isOpen`.
    SendEditorState sendEditor{};

    // The theme editor, over SETTINGS; START passes through, so the scope colours can be set against
    // a moving wave. Edits the live Theme.
    ThemeEditorState themeEditor{};

    // ── SETTINGS: what the screen edits, and what settings.json holds ────────────────────────────
    // `insertBefore` is read when the keyboard opens, so flipping it mid-word changes nothing.
    SettingsValues settings{};

    // ⚠️ No `settingsDirty` flag on purpose: the theme editor writes `theme` directly, so a flag loses
    // changes. The exit compares the bytes on disk instead (ui/settings_store.h).

    // What this platform can do — and so which SETTINGS rows and PROJECT actions exist.
    PlatformCaps caps{};

    // The DEVICE rows' value names, known only to the platform; empty where the rows are not drawn.
    std::string layoutText{};
    std::string skinText{};
    std::string overlayText = "OFF";
    std::string audioOutText = "SYSTEM";   // the output playing now (SETTINGS > AUDIO OUT)
    std::string appVersion{};              // empty in the tools

    // Sample + SoundFont PCM the engine holds (USED RAM).
    int64_t sampleRamBytes = 0;

    // The machine's free memory (FREE RAM); 0 = unknown.
    int64_t freeRamBytes = 0;

    // ── "Last edited": where A,A searches from, and the defaults an insert takes ─────────────────
    int           lastEditedPhrase     = 0;
    int           lastEditedChain      = 0;
    int           lastEditedTable      = 0;
    int           lastEditedInstrument = 0;
    int           lastEditedTranspose  = 0;
    songcore::Note lastEditedNote      = songcore::Note::C4();
    int           lastEditedVolume     = 0x7F;
    // The FX command last entered on PHRASE or TABLE, as a code, with its value.
    int           lastEditedFxType     = 0;
    int           lastEditedFxValue    = 0;

    // A is down. While it is, a CHD value under the cursor names its chord in the status line's place.
    bool aHeld = false;

    // "SAVED", "NO FREE PHRASES"… over the visualizer header on every screen.
    std::string statusMessage{};
    bool        statusSuccess = true;

    // The compact help panel (HELP = SHORT): covers only the strip and swallows no button, so the text
    // follows the cursor.
    bool helpOpen = false;

    // The full help overlay (HELP = FULL): a layer, dims the frame, consumes its closing press. Never
    // both flags at once.
    bool helpFull = false;

    // EXPORT renders synchronously with the audio paused; the frame is repainted from the progress.
    bool  isRendering    = false;
    float renderProgress = 0.0f;

    // COLLECT copies synchronously too; its percentage is repainted the same way.
    bool  isCollecting    = false;
    float collectProgress = 0.0f;

    // Which rows go into the file (ui/modules/render_dialog.h).
    RenderDialogState renderDialog{};

    // Unsaved work: bumped only by `mark_modified`; SAVE / LOAD / NEW align the two.
    int projectVersion      = 0;
    int savedProjectVersion = 0;

    bool project_dirty() const { return projectVersion != savedProjectVersion; }

    // The .ptp this project came from or was last saved to; empty until it has one.
    std::string projectPath{};

    // Set by EXIT; the shell's frame loop leaves.
    bool shouldQuit = false;

    Theme theme = theme_default();

    // config.json's browse folders — debug builds only; empty means the built-in ones.
    FolderConfig folderConfig{};
};

// Does the send editor hold the buttons? Not under the file browser its LOAD raises.
inline bool send_editor_up(const AppState& s) {
    return s.sendEditor.isOpen && s.currentScreen != ScreenType::FILE_BROWSER;
}

// …and is its box drawn? Not under the EQ editor its INP EQ raises.
inline bool send_editor_drawn(const AppState& s) { return send_editor_up(s) && !s.eq.isOpen; }

inline bool stretch_editor_up(const AppState& s) {
    return s.currentScreen == ScreenType::SAMPLE_EDITOR && s.sampleEditor.stretchOpen;
}

// Is a modal up that dims the whole canvas? The shell then dims the letterbox bars too.
// The EQ and theme editors replace the module and leave the frame bright, so they are not here.
inline bool modal_backdrop_active(const AppState& s) {
    // A load is not here: its strip dims nothing.
    return s.qwerty.isOpen || s.confirm.is_open() || s.fxHelper.isOpen || s.mapPicker.isOpen ||
           s.helpFull || s.renderDialog.isOpen || send_editor_drawn(s) || stretch_editor_up(s);
}

// Does a full-screen module own the frame (no furniture)? ⚠️ `draw` and `has_falling_meters` both ask
// here — if they disagreed, the redraw would pin at 60 Hz. The EQ editor over the sample editor
// brings the furniture back.
inline bool full_screen_module(const AppState& s) {
    return s.currentScreen == ScreenType::FILE_BROWSER ||
           (s.currentScreen == ScreenType::SAMPLE_EDITOR && !s.eq.isOpen);
}

// Keep `row` inside SONG's 16-row window.
inline void scroll_song_to_row(AppState& s, int row) {
    if (row < s.songScrollPosition)            s.songScrollPosition = row;
    else if (row >= s.songScrollPosition + 16) s.songScrollPosition = row - 15;
}

}  // namespace pt::ui
