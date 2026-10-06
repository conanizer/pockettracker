#pragma once

// ─── The UI state ────────────────────────────────────────────────────────────────────────────────
//
// Everything the screens draw from that is NOT the project: cursors, the screen that is up, which
// phrase/chain/instrument is being looked at, the playheads read from songcore each frame.
// ⚠️ The draw path takes this struct and nothing else, so a field that is not here cannot reach the
// screen. A new screen adds its fields here; it gets no state of its own.

#include "screen.h"
#include "table-lanes.h"
#include "songcore/model.h"
#include "theme.h"
#include "ui/folder_config.h"
#include "ui/fx_helper.h"
#include "ui/map_picker.h"
#include "ui/modules/confirm_dialog.h"
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

// Only a POINTER to the dispatcher's clipboard is needed (for the "PHR:2x3" readout), so a forward
// declaration keeps clipboard.h out of every TU that includes this.
class Clipboard;

struct AppState {
    // ── The document ─────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ A POINTER to the one Project the host owns and the Sequencer reads; the UI edits it in place
    // (`host.edit_project()`). Never a copy — two mutable copies of a document is a desync. A headless screenshot
    // points it at a Project of its own.
    songcore::Project* project = nullptr;

    // ── Navigation ───────────────────────────────────────────────────────────────────────────────
    // The app boots on SONG.
    ScreenType currentScreen = ScreenType::SONG;

    /** Which column of the 5×5 screen grid a SHARED screen (PROJECT / MIXER / EFFECTS) was entered
     *  from, so R+UP out of MIXER returns there (ui/navigation.h). */
    int previousColumn = 2;

    /** On INSTRUMENT, reached via the pool's R+RIGHT: R+LEFT goes back to the pool, not to PHRASE. */
    bool instrumentFromPool = false;

    // The live cursor of the grid editors. SONG / CHAIN / PHRASE share it; on SONG `cursorColumn` IS
    // the track, 1-based (1..8). The other screens keep their own.
    int cursorRow    = 0;
    int cursorColumn = 1;

    int tableCursorRow    = 0;
    int tableCursorColumn = 1;  // starts on transpose
    int grooveCursorRow   = 0;
    // GROOVE. The tick grid and the side panel keep separate rows: the swing readout follows the TICK
    // cursor, so moving into the panel to set QNT must not move it. `grooveCursorColumn` says which
    // has the cursor (ui/modules/groove_editor.h).
    int grooveCursorColumn = 1;
    int groovePanelRow     = 0;
    int groovePanelColumn  = 0;  // the SAVE/LOAD row only
    /** The quantize pointer — how A+DPAD edits a groove step. Not a setting: never saved, and reset
     *  to OFF at app start and on project load. */
    int grooveQuantize     = 0;
    int scaleCursorRow    = 0;
    // Read on the SCALE screen's NAME row only. Not what `cursor_column()` answers for SCALE — the
    // selection and clipboard see that screen as one column of degrees.
    int scaleCursorColumn = 0;

    // INSTRUMENT walks the row-kind table in ui/instrument_row_layout.h, not a range.
    int instrumentCursorRow    = 0;
    int instrumentCursorColumn = 1;

    /** INST.POOL. The pool's ROW is `currentInstrument` itself; only the column lives here (0..4). */
    int poolCursorColumn = 0;

    // MODS: four slots drawn as two pairs, so the cursor is (pair, side, row):
    // `activeSlot = modSlots[pair * 2 + side]`.
    int modCursorRow  = 0;
    int modCursorPair = 0;  // 0 = MOD1+MOD2, 1 = MOD3+MOD4
    int modCursorSide = 0;  // 0 = left, 1 = right

    // MIXER: two ints but not a grid — which pair is which cell is `ui/mixer_cell_layout.h`.
    int mixerCursorColumn = 0;  // 0..7 = tracks, 8 = master
    int mixerMasterRow    = 0;  // 0 = volumes, 1 = sends / EQ, 2 = OTT|DUST, 3 = LIM

    /** EFFECTS. Eight editable rows among fifteen drawn. */
    int effectsCursorRow = 0;

    /**
     * Where the shared cursor was on leaving each of the three screens that share it. Required, not an
     * optimisation: SONG has 8 columns, CHAIN 2, PHRASE 9, so a carried column could land outside the
     * new screen and the cursor would vanish.
     */
    int songCursorRow = 0,   songCursorColumn = 1;
    int chainCursorRow = 0,  chainCursorColumn = 1;
    int phraseCursorRow = 0, phraseCursorColumn = 1;

    // PROJECT. Rows 0..7 (0..8 with an EXIT row); column 0 is the label, so the cursor starts on 1
    // (ui/settings_row_layout.h).
    int projectCursorRow    = 0;
    int projectCursorColumn = 1;

    // MIDI. One column — ui/modules/midi_settings.h.
    int midiCursorRow    = 0;
    int midiCursorColumn = 1;

    // MIDI MAPPING. ⚠️ The row count is the SONG's (one per mapping, plus ADD); a cursor past the end
    // after a delete is clamped on the way in and after every edit.
    int midiMapCursorRow    = 0;
    int midiMapCursorColumn = 1;

    // SETTINGS. `settingsCursorRow` is a SettingsRow — the row's NUMBER (its identity), not its
    // position in this platform's filtered list.
    int settingsCursorRow    = 0;
    int settingsCursorColumn = 1;

    /** SONG shows 16 of its 256 rows: the first visible row, 0..240. */
    int songScrollPosition = 0;

    // Which slot of each pool is being edited.
    int currentPhrase     = 0;
    int currentChain      = 0;
    int currentInstrument = 0;
    int currentTable      = 0;
    int currentGroove     = 0;
    int currentScale      = 0;   // which of the 16 slots the SCALE screen is showing, 0-15

    // ── Playback (read back from songcore's playheads each frame) ────────────────────────────────
    //
    // ⚠️ Eight, one per track, −1 where a track has no position (ui/playhead.h). The eight run
    // independently, so there is no single "playback row".
    bool          isPlaying    = false;
    TrackPlayhead playheads[8] = {};

    // ── LIVE mode (SONG's launcher) ──────────────────────────────────────────────────────────────
    //
    // A per-session choice — saved nowhere. ⚠️ A READBACK of the sequencer's mode, refilled each frame;
    // input asks the host, never this, so screen and transport cannot disagree.
    bool     liveMode     = false;
    LiveQueue liveQueue[8] = {};

    /**
     * The blink phase for queue markers, 0..999 ms, set once a frame. Handed in rather than read, so
     * a headless screenshot (no clock) can set it and get the same pixels every time.
     */
    int blinkPhaseMs = 0;

    /**
     * The TABLE row an engine voice is on, one per FX column (−1 = not running). Read off the VOICE,
     * not the sequencer: a table runs on its own tic clock under a note that may outlive its step, and
     * each column advances on its own (ui/engine_feed.h).
     */
    int tablePlaybackRows[TABLE_LANES] = {-1, -1, -1};

    // ── The note monitor (right bar) ─────────────────────────────────────────────────────────────
    // What each track is SOUNDING, from the engine's voices — so a long sample still shows while it
    // rings out past its chain.
    songcore::Note trackNotes[8] = {};

    // ── The SoundFont preset list (INSTRUMENT screen, PRESET row) ────────────────────────────────
    //
    // Read back from the engine for `currentInstrument` — only the engine has opened the .sf2. Refreshed
    // once a frame (ui/engine_feed.h). With none loaded: 0 / 0 / "---", which a headless screenshot draws.
    std::string sfPresetName  = "---";
    int         sfPresetCount = 0;
    int         sfPresetIndex = 0;

    // ── The visualizer (right/top strip) ─────────────────────────────────────────────────────────
    // Filled by ui/engine_feed.h once a frame; null means silence (what a headless screenshot draws).
    const float* waveform       = nullptr;  // WAVEFORM_SIZE master samples
    const float* trackWaveforms = nullptr;  // TRACK_WAVEFORM_COUNT × WAVEFORM_SIZE, flat (OCTA)
    const float* spectrum       = nullptr;  // NUM_BARS magnitudes (SPECTRUM modes)

    /** Bit N set once track N has had a note scheduled this phrase — SongcoreHost::track_mask(). */
    int  trackMask         = 0;
    /** The preview lane had audio last block; OCTA lights its scope only while STOPPED. */
    bool previewLaneActive = false;

    // ── The MIXER's meters ───────────────────────────────────────────────────────────────────────
    //
    // Read only while the MIXER is up, every 60 ms: `getTrackPeaks` takes a mutex the AUDIO callback
    // also takes, so polling it on every screen at 60 Hz would contend with the audio thread.
    // ⚠️ The peak-HOLD counts `peaksVersion`, not frames (ui/modules/mixer.h).
    float    trackPeaks[16] = {};   // L/R per track
    float    masterPeaks[2] = {};
    float    sendPeaks[4]   = {};   // revL, revR, delL, delR
    unsigned peaksVersion   = 0;

    // ── Selection ────────────────────────────────────────────────────────────────────────────────
    // The L+B multi-tap CELL/ROW/SCREEN machine (ui/selection.h).
    Selection selection{};

    bool selection_mode() const { return selection.active; }
    bool is_cell_selected(int row, int column) const {
        return selection.is_cell_selected(row, column);
    }

    /**
     * Which of L+R's two rungs to try FIRST — whichever the user touched last. L+R undoes one thing per
     * press (mute/solo, or the selection), so dropping a channel out cannot also discard a selection;
     * a rung with nothing to clear falls through to the other.
     */
    enum class Clearable { NONE, SELECTION, MUTE };
    Clearable lastClearable = Clearable::NONE;

    // ── The clipboard's readout ──────────────────────────────────────────────────────────────────
    // Points at the dispatcher's clipboard, set by its constructor. Null with no input layer (headless screenshots),
    // and the readout is then absent.
    const Clipboard* clipboard = nullptr;

    // ── The FX-helper overlay ────────────────────────────────────────────────────────────────────
    // A+UP/DOWN on an FX-TYPE column opens it; releasing A commits (ui/fx_helper.h). While open it OWNS
    // the D-pad.
    FxHelperState fxHelper{};

    // ── The mapping DESTINATION picker ───────────────────────────────────────────────────────────
    // The same gesture over a mapping's GROUP or PARAMETER cell (ui/map_picker.h).
    MapPickerState mapPicker{};

    // ── The file browser, and why it was opened ──────────────────────────────────────────────────
    FileBrowserState fileBrowser{};

    /** What A will DO with the picked file — the browser only lists, sorts and hands back a path. */
    enum class BrowserPurpose {
        LOAD_SOURCE,        // a sample (or an .sf2 — the instrument's TYPE decides, at open time)
        LOAD_PRESET,        // a .pti into the current instrument slot
        LOAD_SAMPLE_EDITOR, // a .wav into the slot the SAMPLE EDITOR is open on — and back to it
        LOAD_PROJECT,       // a .ptp — the whole document (PROJECT's LOAD)
        LOAD_THEME,         // a .ptt — and back into the THEME EDITOR that raised the browser
        LOAD_SCALE,         // a .pts into the slot the SCALE screen is showing
        LOAD_GROOVE         // a .ptg into the slot the GROOVE screen is showing
    };
    BrowserPurpose browserPurpose = BrowserPurpose::LOAD_SOURCE;

    /** The screen the browser (or a full-screen overlay) returns to. Every overlay open writes it. */
    ScreenType previousScreen = ScreenType::PROJECT;

    /**
     * Where B goes from SETTINGS — ⚠️ deliberately NOT `previousScreen`, which every browser and sample
     * editor open moves; riding on it would land B wherever the last overlay came from.
     * Only PROJECT → SYSTEM writes it, so reaching SETTINGS by the nav grid leaves it at PROJECT — and
     * the way back from there is R+DPAD.
     */
    ScreenType settingsReturnScreen = ScreenType::PROJECT;

    /** Where B goes from MIDI, for the same reason. */
    ScreenType midiReturnScreen = ScreenType::PROJECT;

    /** …and from the mapping list, which is only ever reached from MIDI. */
    ScreenType midiMapReturnScreen = ScreenType::MIDI;

    // ── MIDI ─────────────────────────────────────────────────────────────────────────────────────
    //
    // The output port: the one platform object in this struct (the dispatcher's `FileSystem` is the
    // other in pt-ui). A pointer to songcore's five-method interface, so no SDL here; null with no
    // backend, and every use is guarded. Held by the UI because OUTPUT's option list comes from the OS
    // and changes while the app runs.
    songcore::IMidiOut* midiOut = nullptr;

    /**
     * The port list with "OFF" and "AUTO" prepended. ⚠️ Rebuilt on every entry to the screen
     * (`refresh_midi_devices()`) — MIDI is hot-pluggable.
     */
    std::vector<std::string> midiDeviceNames{"OFF", "AUTO"};
    int                      midiDeviceIndex = 0;
    /** The device actually open, "" for none — under AUTO it differs from the setting. */
    std::string              midiOutOpenName;

    /**
     * The INPUT port and its list — separate from the output's, as on every platform (a loopback port
     * appears in both under one name). ⚠️ May be null (e.g. libasound missing); every use is guarded.
     * Here because the dispatcher opens it at boot and reads this struct.
     */
    songcore::IMidiIn*       midiIn = nullptr;
    std::vector<std::string> midiInDeviceNames{"OFF", "AUTO"};
    int                      midiInDeviceIndex = 0;
    std::string              midiInOpenName;

    /** The MIDI screen's one-shot readout — "PANIC SENT", "TEST SENT", "NO PORT". */
    std::string midiStatusText;

    /**
     * The output latency the audio device reported, in ms — what the OFFSET row's AUTO uses. A platform
     * fact, written once by the shell and never saved; 0 until set.
     */
    int midiAutoOffsetMs = 0;

    /**
     * The channel the cable last carried a CC on, or −1. Copied off the host in `set_now`, because the
     * MIDI screen is built in two places (draw and cursor context) and must not be fed lazily by one.
     */
    int midiInCcChannel = -1;

    /** The audio callback's cost, copied off the host in `set_now`. */
    AudioLoad audioLoad{};

    // ── The QWERTY keyboard ─────────────────────────────────────────────────────────────────────
    // A true modal: while open it owns every button; `isOpen` is checked first in every handler.
    QwertyKeyboardState qwerty{};

    // ── The SAMPLE EDITOR ───────────────────────────────────────────────────────────────────────
    //
    // A SESSION, not a view: the selection, detected transients and pending pitch shift live here from
    // the editor opening to it closing. The audio is in the engine; this holds the waveform's min/max
    // pairs and the knobs.
    SampleEditorState sampleEditor{};

    // ── The confirm dialog ───────────────────────────────────────────────────────────────────────
    //
    // ONE state for every confirm, so "is a modal up?" has one answer (ui/modules/confirm_dialog.h).
    ConfirmDialogState confirm{};

    // ── LOADING ──────────────────────────────────────────────────────────────────────────────────
    //
    // A file load can outlast a frame. A strip across the top says so — never a modal; it dims nothing
    // (ui/modules/loading_strip.h).
    // ⚠️ `shown` is not `running`: almost every load is over in a tenth of a second, and a strip that
    // flashes is worse than none. A load RUNS from its first moment and is SHOWN only after
    // `LOADING_DELAY_MS` — no size prediction, so a slow device shows it exactly when it is slow.
    struct LoadingState {
        /** A load is in flight. Owns every button (the LOADING layer) from the first moment. */
        bool running = false;
        /** …and has outlasted the delay, so there is a strip on the screen. */
        bool shown = false;
        /** 0..1, or **< 0** when nothing in the file states a total — see load_progress.h. */
        float progress = -1.0f;
        /** What is being loaded: the file's name, or the project's. Empty draws the title alone. */
        std::string detail{};
        /** Milliseconds since the load opened. It raises `shown`, and is the strip's only moving part
         *  when there is no percentage — a working load and a hung one must look different. */
        int elapsedMs = 0;
        /** B has been pressed. The engine reads it through the tick's return and unwinds. */
        bool cancelRequested = false;
    };
    LoadingState loading{};

    // ── The EQ EDITOR ────────────────────────────────────────────────────────────────────────────
    //
    // A PARTIAL modal: it owns the D-pad, A, B and SELECT, but START passes through, so an instrument
    // audition can ring while a band is swept across it.
    // ⚠️ `eq.caller` is captured on OPEN: five cells raise the editor, and B+LEFT/RIGHT writes the new
    // slot back into whichever asked.
    EqEditorState eq{};

    /**
     * The spectrum of the signal the open EQ sits on — master bus, a send's input, or one instrument
     * (`eq.caller` picks; ui/engine_feed.h polls at 20 Hz while open). Not the visualizer's `spectrum`,
     * which is always the master bus.
     */
    const float* eqSpectrum      = nullptr;
    int          eqSpectrumCount = 0;

    // The engine's device rate, so the EQ curve plots at the rate the bands were built at.
    int          eqSampleRate    = 44100;

    // ── The THEME EDITOR ─────────────────────────────────────────────────────────────────────────
    //
    // A partial modal like the EQ editor: START reaches the transport, so VIZ BG / LINE / WAVE can be
    // dialled against a moving oscilloscope. Some colours preview by the editor drawing itself in them;
    // the rest only as swatches, since their pixels are on screens the overlay replaced. Raised only
    // from SETTINGS, and edits the app's single live Theme.
    ThemeEditorState themeEditor{};

    // ── SETTINGS ─────────────────────────────────────────────────────────────────────────────────
    //
    // Every value the SETTINGS screen edits — also the unit written to settings.json.
    // ⚠️ `settings.insertBefore` is read when the QWERTY keyboard OPENS, so flipping it mid-word cannot
    // change what the buttons mean.
    // `settings.cursorRemember` is consulted by go_to_screen: REMEMBER restores each screen's last
    // cursor, REFRESH (the default) resets it to the top-left editable cell.
    SettingsValues settings{};

    // ⚠️ There is deliberately NO `settingsDirty` flag. Not every edit path goes through the SETTINGS
    // screen (the THEME EDITOR mutates `theme` directly), so a flag loses changes. The exit calls
    // `save_settings_if_changed()`, which compares the bytes on disk with memory. Re-adding a flag
    // re-adds the bug (ui/settings_store.h).

    /** What this platform can do — and therefore which SETTINGS rows and PROJECT actions exist. */
    PlatformCaps caps{};

    // What the DEVICE rows' indices are called on this platform (only the platform knows index 2 is
    // "PORTRAIT"). Empty on the shell, which does not draw those rows (ui/modules/settings_editor.h).
    std::string layoutText{};
    std::string skinText{};
    std::string overlayText = "OFF";
    std::string audioOutText = "SYSTEM";   // the output playing now (SETTINGS > AUDIO OUT)

    /** USED RAM: sample + SoundFont PCM the engine is holding. Drawn on PROJECT and INST.POOL. */
    int64_t sampleRamBytes = 0;

    /** FREE RAM: physical memory the machine still has. 0 = the platform could not answer. */
    int64_t freeRamBytes = 0;

    // ── "Last edited" — what A,A and the insert defaults remember ────────────────────────────────
    //
    // A,A on SONG inserts the next unused chain after the one last touched; an inserted chain row
    // carries the transpose last dialled in. Without these every search starts at 0.
    int           lastEditedPhrase     = 0;
    int           lastEditedChain      = 0;
    int           lastEditedTable      = 0;
    int           lastEditedInstrument = 0;
    int           lastEditedTranspose  = 0;
    songcore::Note lastEditedNote      = songcore::Note::C4();
    int           lastEditedVolume     = 0x7F;

    // ── The status line ──────────────────────────────────────────────────────────────────────────
    //
    // "SAVED" / "CHAIN CLONED" / "NO FREE PHRASES" — drawn over the visualizer header on every screen
    // (TrackerLayout::draw). SAVE, EXPORT and COMPACT have no other feedback.
    std::string statusMessage{};
    bool        statusSuccess = true;

    // ── HELP ON SELECT ───────────────────────────────────────────────────────────────────────────
    //
    // The compact help panel replacing the visualizer (SETTINGS > HELP = SHORT; ui/button_mapper.h).
    // ⚠️ NOT an overlay: it covers only the 620×70 strip, swallows no button and leaves the screen
    // usable, so the text follows the cursor. Not persisted.
    bool helpOpen = false;

    // The FULL help overlay (HELP = FULL). ⚠️ This one IS an overlay — every fact above is inverted: it
    // is on the layer stack and in `modal_backdrop_active`, and its closing press is consumed. Never
    // both flags at once.
    bool helpFull = false;

    // ── The render (PROJECT → EXPORT) ────────────────────────────────────────────────────────────
    //
    // Synchronous, on this thread, with the audio device paused; the frame is repainted from the
    // progress callback.
    bool  isRendering    = false;
    float renderProgress = 0.0f;

    // …and WHICH ROWS go into the file: EXPORT raises this panel, so one sketch in a project can be
    // exported (ui/modules/render_dialog.h).
    RenderDialogState renderDialog{};

    // ── Is there unsaved work? ───────────────────────────────────────────────────────────────────
    //
    // Bumped only by `InputDispatcher::mark_modified`, which every edit goes through; SAVE / LOAD / NEW
    // align the two. A clean project skips the NEW PROJECT? and EXIT? confirms.
    int projectVersion      = 0;
    int savedProjectVersion = 0;

    bool project_dirty() const { return projectVersion != savedProjectVersion; }

    /** The .ptp this project came from (or was last saved to). Empty until it has one. */
    std::string projectPath{};

    /** Set by EXIT. The shell's frame loop reads it and leaves. */
    bool shouldQuit = false;

    // ── Theme ────────────────────────────────────────────────────────────────────────────────────
    Theme theme = theme_default();

    // ── config.json — hand-edited default browse folders ────────────────────────────────────────────
    // Read once at boot, debug builds only. Empty otherwise: every category uses its built-in dir.
    FolderConfig folderConfig{};
};

/**
 * Is a modal up that paints the full-canvas MODAL_BACKDROP? The shell extends the dim into the
 * letterbox bars when it is.
 * ⚠️ Exactly those modals: qwerty, confirm, full help, render dialog, FX helper, map picker. The EQ and
 * theme editors REPLACE the module and leave the frame bright, so they are not here.
 */
inline bool modal_backdrop_active(const AppState& s) {
    // A load is not here: its strip dims nothing.
    return s.qwerty.isOpen || s.confirm.is_open() || s.fxHelper.isOpen || s.mapPicker.isOpen ||
           s.helpFull || s.renderDialog.isOpen;
}

/**
 * Does a FULL-SCREEN module own the frame (no furniture drawn)? One answer for `draw`'s early return
 * and `has_falling_meters` (layout.cpp) — if they disagree, the redraw loop pins at 60 Hz over a
 * static frame.
 * ⚠️ `!s.eq.isOpen`: the EQ editor opened from the sample editor replaces it and brings the furniture
 * back.
 */
inline bool full_screen_module(const AppState& s) {
    return s.currentScreen == ScreenType::FILE_BROWSER ||
           (s.currentScreen == ScreenType::SAMPLE_EDITOR && !s.eq.isOpen);
}

/** Keep `cursorRow` inside SONG's 16-row window. The state's own invariant — the D-pad, `go_to_screen`
 *  and the song pointer all move the song row. */
inline void scroll_song_to_row(AppState& s, int row) {
    if (row < s.songScrollPosition)            s.songScrollPosition = row;
    else if (row >= s.songScrollPosition + 16) s.songScrollPosition = row - 15;
}

}  // namespace pt::ui
