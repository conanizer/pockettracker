#pragma once

// ─── The UI state ────────────────────────────────────────────────────────────────────────────────
//
// Everything the screens draw from that is NOT the project itself: where the cursor is, which screen
// is up, which phrase/chain/instrument is being looked at, and the playhead the 60 Hz loop last read
// out of songcore.
//
// On Android this state is scattered across `TrackerController` and ~60 `mutableStateOf` refs in
// MainActivity, and it has to be: Compose needs observable holders to know what to recompose. There
// is no recomposition here — the frame is redrawn from this struct — so the observers collapse into
// plain fields and the two halves become one struct. The FIELD NAMES are kept, because the Kotlin
// files are the executable spec for the port and a reviewer must be able to read them side by side.
//
// ⚠️ This struct GROWS with each session of the Phase 3 port. What is here now is what the screens
// that exist now read. It is not a design that got smaller — it is a design that has not arrived yet.

#include "screen.h"
#include "songcore/model.h"
#include "theme.h"
#include "ui/fx_helper.h"
#include "ui/modules/confirm_dialog.h"
#include "ui/modules/eq_editor.h"
#include "ui/modules/file_browser.h"
#include "ui/modules/qwerty_keyboard.h"
#include "ui/modules/sample_editor.h"
#include "ui/modules/settings_editor.h"
#include "ui/platform_caps.h"
#include "ui/selection.h"

#include <cstdint>
#include <string>

namespace pt::ui {

struct AppState {
    // ── The document ─────────────────────────────────────────────────────────────────────────────
    //
    // A POINTER, and there is exactly one Project behind it: the one SongcoreHost owns and the
    // Sequencer reads. The UI edits it in place through `host.edit_project()`.
    //
    // Not a copy, and this is the single most important line in the struct. Android can afford a
    // second Project (Compose needs its own observable object graph, and it pushes a JSON blob down to
    // songcore whenever it changes) — but two mutable copies of a document is a desync waiting to
    // happen, and there is no reason to take that risk on a platform where the UI and the sequencer
    // are the same program in the same address space. `ptshot` points this at a Project it owns
    // itself; the SDL shell points it at the host's.
    songcore::Project* project = nullptr;

    // ── Navigation ───────────────────────────────────────────────────────────────────────────────
    ScreenType currentScreen = ScreenType::PHRASE;

    /**
     * Which column of the 5×5 screen grid a SHARED screen was entered from (PROJECT / MIXER /
     * EFFECTS sit in every column and own none). It is what lets R+UP out of MIXER return you to the
     * main-row screen you came from rather than to a fixed default — see ui/navigation.h.
     */
    int previousColumn = 2;

    /** On INSTRUMENT, reached via the pool's R+RIGHT: R+LEFT goes back to the pool, not to PHRASE. */
    bool instrumentFromPool = false;

    // The live cursor of the grid editors. SONG / CHAIN / PHRASE share it — and on SONG,
    // `cursorColumn` IS the track, 1-based (1..8). TABLE, GROOVE, INSTRUMENT and the rest carry their
    // own, exactly as TrackerController does.
    int cursorRow    = 0;
    int cursorColumn = 1;

    int tableCursorRow    = 0;
    int tableCursorColumn = 1;  // starts on transpose
    int grooveCursorRow   = 0;

    // INSTRUMENT. Its rows are not a uniform grid — they are the row-kind table in
    // ui/instrument_row_layout.h, and the cursor walks that rather than a range.
    int instrumentCursorRow    = 0;
    int instrumentCursorColumn = 1;

    /** INST.POOL. The pool's ROW is `currentInstrument` itself, so only the column lives here (0..4). */
    int poolCursorColumn = 0;

    // MODS. Four slots drawn as two pairs of two, so the cursor is a (pair, side, row) triple rather
    // than a (row, column) pair: `activeSlot = modSlots[pair * 2 + side]`.
    int modCursorRow  = 0;
    int modCursorPair = 0;  // 0 = MOD1+MOD2, 1 = MOD3+MOD4
    int modCursorSide = 0;  // 0 = left, 1 = right

    // MIXER. Two ints, but NOT a grid: rows 2 and 3 exist only in column 8 (the master strip), and the
    // cursor reaches them by walking DOWN it. Every other (row, column) pair is unreachable, and the
    // module answers `none()` there rather than guessing — see ui/modules/mixer.h.
    int mixerCursorColumn = 0;  // 0..7 = tracks, 8 = master
    int mixerMasterRow    = 0;  // 0 = volumes, 1 = sends / EQ, 2 = OTT|DUST, 3 = LIM

    /** EFFECTS. Eight editable rows; the screen draws fifteen (headers and spacers between them). */
    int effectsCursorRow = 0;

    /**
     * Where the shared cursor was when you last left each of the three screens that share it.
     *
     * Not an optimisation — a CORRECTNESS requirement, and the reason `go_to_screen` exists. The three
     * screens have different column counts (SONG 8, CHAIN 2, PHRASE 9), so carrying a live column
     * across a screen change can land the cursor outside the new screen's range: leave PHRASE on
     * column 9, arrive on CHAIN, and no cell matches the cursor — it vanishes. Kotlin saves and
     * restores per screen for exactly this reason (`saveCursorForScreen` / `restoreCursorForScreen`).
     */
    int songCursorRow = 0,   songCursorColumn = 1;
    int chainCursorRow = 0,  chainCursorColumn = 1;
    int phraseCursorRow = 0, phraseCursorColumn = 1;

    // PROJECT. Rows 0..6 (0..7 on the shell, which has an EXIT row); column 0 is the label and is
    // unreachable, so the cursor starts on 1 — see ui/settings_row_layout.h.
    int projectCursorRow    = 0;
    int projectCursorColumn = 1;

    // SETTINGS. `settingsCursorRow` is a SettingsRow — the row's NUMBER, which is its identity on
    // BOTH platforms, not its position in this platform's filtered list.
    int settingsCursorRow    = 0;
    int settingsCursorColumn = 1;

    /** SONG shows 16 of its 256 rows: the first visible row. TrackerController clamps it to 0..240. */
    int songScrollPosition = 0;

    // Which slot of each pool is being edited.
    int currentPhrase     = 0;
    int currentChain      = 0;
    int currentInstrument = 0;
    int currentTable      = 0;
    int currentGroove     = 0;

    // ── Playback (read back from songcore's playheads at 60 Hz) ──────────────────────────────────
    bool isPlaying          = false;
    int  playbackRow        = 0;  // phrase step, on the PHRASE screen
    int  playbackChainRow   = 0;
    int  playbackSongRow    = 0;

    /**
     * The TABLE row an engine voice is on — −1 when none is. Unlike the three above, this is not a
     * sequencer playhead: it is read off the VOICE, because a table advances on its own tic clock
     * under a note that may outlive the step that started it (ui/engine_feed.h).
     */
    int tablePlaybackRow = -1;

    // ── The note monitor (right bar) ─────────────────────────────────────────────────────────────
    // What each of the 8 tracks is SOUNDING, read from the engine's voice pool rather than from the
    // sequencer — so a long sample still shows while it rings out past the end of its chain.
    songcore::Note trackNotes[8] = {};

    // ── The SoundFont preset list (INSTRUMENT screen, PRESET row) ────────────────────────────────
    //
    // Read back from the engine for `currentInstrument`, because only the engine has opened the .sf2
    // and knows what is in it — the Project stores a bank and a preset NUMBER, not the list they index
    // into. Refreshed once a frame beside the note monitor (ui/engine_feed.h).
    //
    // With no SoundFont loaded these are 0 / 0 / "---", and that is what makes the screen drawable with
    // no engine at all: `ptshot` renders the PRESET row from exactly these three fields.
    std::string sfPresetName  = "---";
    int         sfPresetCount = 0;
    int         sfPresetIndex = 0;

    // ── The visualizer (right/top strip) ─────────────────────────────────────────────────────────
    // Filled by ui/engine_feed.h once a frame; null means silence, which is what `ptshot` draws with
    // (it has no engine at all — and that is the proof the UI does not need one).
    const float* waveform       = nullptr;  // WAVEFORM_SIZE master samples
    const float* trackWaveforms = nullptr;  // TRACK_WAVEFORM_COUNT × WAVEFORM_SIZE, flat (OCTA)
    const float* spectrum       = nullptr;  // NUM_BARS magnitudes (SPECTRUM modes)

    /** Bit N set once track N has had a note scheduled this phrase — SongcoreHost::track_mask(). */
    int  trackMask         = 0;
    /** The preview lane had audio last block; OCTA lights its scope only while STOPPED. */
    bool previewLaneActive = false;

    // ── The MIXER's meters ───────────────────────────────────────────────────────────────────────
    //
    // Read out of the engine ONLY while the MIXER is up, and only every 60 ms — both of which are
    // Kotlin's (its whole peak loop is a `LaunchedEffect(currentScreen)` gated on MIXER, ticking at
    // `delay(60)`). Neither is an optimisation for its own sake: `getTrackPeaks` takes the engine's
    // peak mutex, which the AUDIO CALLBACK also takes, so polling it at 60 Hz on every screen would be
    // contention with the audio thread bought for nothing.
    //
    // ⚠️ `peaksVersion` is what the peak-HOLD counts, not frames. See ui/modules/mixer.h.
    float    trackPeaks[16] = {};   // L/R per track
    float    masterPeaks[2] = {};
    float    sendPeaks[4]   = {};   // revL, revR, delL, delR
    unsigned peaksVersion   = 0;

    // ── Selection ────────────────────────────────────────────────────────────────────────────────
    // The L+B multi-tap CELL/ROW/SCREEN machine (ui/selection.h). The grid editors have asked these
    // two questions since S1; until S3 they were stubbed to "no selection".
    Selection selection{};

    bool selection_mode() const { return selection.active; }
    bool is_cell_selected(int row, int column) const {
        return selection.is_cell_selected(row, column);
    }

    // ── The FX-helper overlay ────────────────────────────────────────────────────────────────────
    // A+UP/DOWN on an FX-TYPE column opens it; releasing A commits the highlighted effect
    // (ui/fx_helper.h). While it is open it OWNS the D-pad — the cursor underneath must not move.
    FxHelperState fxHelper{};

    // ── The file browser, and why it was opened (S6a) ────────────────────────────────────────────
    FileBrowserState fileBrowser{};

    /**
     * What the A button will DO with the file the user picks. The browser itself has no idea — it
     * lists, it sorts, it hands back a path.
     *
     * ⚠️ Android answers this question with TWO fields and no type at all: `previousScreen` (a
     * ScreenType) plus `instrumentFileBrowserAction`, a **String** compared against the literals
     * `"LOAD_PRESET"`, `"LOAD_SOURCE"`, `"LOAD_SAMPLE_EDITOR"` and `"LOAD_THEME"` — with a silent
     * `else` arm for every typo. An enum is the same information with the failure mode removed.
     */
    enum class BrowserPurpose {
        LOAD_SOURCE,        // a sample (or an .sf2 — the instrument's TYPE decides which, at open time)
        LOAD_PRESET,        // a .pti into the current instrument slot
        LOAD_SAMPLE_EDITOR, // a .wav into the slot the SAMPLE EDITOR is open on — and back to the editor
        LOAD_PROJECT        // a .ptp — the whole document, from PROJECT's LOAD button (S7)
    };
    BrowserPurpose browserPurpose = BrowserPurpose::LOAD_SOURCE;

    /** The screen the browser (or a full-screen overlay) will return to when it closes. */
    ScreenType previousScreen = ScreenType::INSTRUMENT;

    // ── The QWERTY keyboard ─────────────────────────────────────────────────────────────────────
    // The app's first true modal: while it is open it owns every button, and `isOpen` is checked
    // before any other arm in every handler that can reach it.
    QwertyKeyboardState qwerty{};

    // ── The SAMPLE EDITOR (S6b) ─────────────────────────────────────────────────────────────────
    //
    // The one screen whose state is a SESSION rather than a view. Everything else in this struct is a
    // cursor position — throw it away and you lose your place. Throw this away and you lose the
    // selection you spent a minute dialling in, the transients you just detected, and the pending
    // pitch shift you have not baked yet. It is created fresh when INSTRUMENT's EDIT opens the editor,
    // and it lives until the editor closes.
    //
    // The AUDIO is not in here — it is in the engine, where the twelve operations already were. What
    // this holds is the 620 min/max pairs the waveform draws from, and the state of the knobs.
    SampleEditorState sampleEditor{};

    // ── The confirm dialog (S7) ──────────────────────────────────────────────────────────────────
    //
    // The port's second true modal, and — unlike Android's four separate `show*Dialog` booleans — ONE
    // state, so the "is a modal up?" question every handler must ask has exactly one answer to check.
    // See ui/modules/confirm_dialog.h for why that is worth a file.
    ConfirmDialogState confirm{};

    // ── The EQ EDITOR (S8) ───────────────────────────────────────────────────────────────────────
    //
    // The port's third modal, and the first PARTIAL one: it owns the D-pad, A, B and SELECT, but START
    // deliberately passes THROUGH to the screen underneath. That is not an oversight in Kotlin — it is
    // what lets you hold an instrument audition ringing and sweep a band across it, which is the only
    // way to hear what an EQ is doing. Every other modal in the app swallows everything.
    //
    // ⚠️ `eq.caller` is captured when the editor OPENS and is never re-read: five different cells raise
    // it, and B+LEFT/RIGHT inside it has to write the new slot back into whichever field asked.
    EqEditorState eq{};

    /**
     * The spectrum of the signal the OPEN EQ sits on — the master bus, a send's input, or one
     * instrument's voices; `eq.caller` picks which, and ui/engine_feed.h polls it at 20 Hz (Kotlin's
     * own cadence) only while the editor is up.
     *
     * Separate from `spectrum` above, which is the VISUALIZER's and is always the master bus. Same
     * engine, two different questions — and pointing the EQ at the master bus would draw a curve over a
     * signal the band is not even in.
     */
    const float* eqSpectrum      = nullptr;
    int          eqSpectrumCount = 0;

    // ── SETTINGS (S7) ────────────────────────────────────────────────────────────────────────────
    //
    // Every value the SETTINGS screen edits, in one struct — which is also the unit the shell writes
    // to settings.json. On Android these are ~16 separate `mutableStateOf` refs plus SharedPreferences
    // (Compose leaves no choice); here the screen, the persistence and the code that READS a setting
    // all name the same field.
    //
    // ⚠️ `settings.insertBefore` is read by the QWERTY keyboard when it OPENS, not while it is open,
    // so flipping the setting mid-word cannot change what the buttons mean under the user's thumb.
    // ⚠️ `settings.cursorRemember` is what go_to_screen consults: REMEMBER restores each screen's last
    // cursor, REFRESH (the default, as on Android) resets it to the top-left editable cell on entry.
    SettingsValues settings{};

    /**
     * A setting changed and settings.json has not caught up. The shell writes on exit rather than on
     * every keystroke: holding A+UP on the overlay strength fires an edit every 100 ms (the key-repeat
     * interval), and a file write per repeat is an SD card being hammered for a value still moving.
     */
    bool settingsDirty = false;

    /** What this platform can do — and therefore which SETTINGS rows and PROJECT actions exist. */
    PlatformCaps caps{};

    // What the DEVICE rows' indices NAME on this platform — text the settings module paints but does
    // not own, because only the platform knows that layout index 2 is "PORTRAIT". All empty on the
    // shell, which does not draw those rows at all. (This is the seam that keeps `DeviceAdapter` out
    // of the port: see ui/modules/settings_editor.h.)
    std::string layoutText{};
    std::string skinText{};
    std::string overlayText = "OFF";

    /** PROJECT's debug-only USED RAM readout: sample + SoundFont PCM the engine is holding. */
    int64_t sampleRamBytes = 0;

    // ── "Last edited" — the memory that makes A,A and the insert defaults useful ─────────────────
    //
    // TrackerController's `lastEdited*`. Not cosmetic: A,A on SONG inserts the next unused chain
    // *after the one you last touched*, and a chain row inserted on CHAIN carries the transpose you
    // last dialled in. Without them, every insert would start its search at 0 and hand you a slot
    // nowhere near the one you were working on.
    int           lastEditedPhrase     = 0;
    int           lastEditedChain      = 0;
    int           lastEditedTable      = 0;
    int           lastEditedInstrument = 0;
    int           lastEditedTranspose  = 0;
    songcore::Note lastEditedNote      = songcore::Note::C4();
    int           lastEditedVolume     = 0x7F;

    // ── The status line ──────────────────────────────────────────────────────────────────────────
    //
    // "SAVED" / "CHAIN CLONED" / "NO FREE PHRASES" — what an action reports back. Drawn as a GLOBAL
    // overlay on the visualizer header (TrackerLayout::draw), so that every screen can report without
    // spending an editor row on it. Kotlin does the same, at PixelPerfectRenderer:444.
    //
    // ⚠️ S3 ADDED THESE TWO FIELDS AND NOTHING EVER DREW THEM. The dispatcher has been setting them
    // at 22 sites since the clipboard landed, so every "CHAIN CLONED" and every "NO FREE PHRASES"
    // this port has ever produced went straight into the void — a bug found the only way it could be,
    // by porting the screen whose actions have NO other feedback at all: SAVE, EXPORT and COMPACT say
    // nothing else, and a save that reports nothing is a save you cannot trust.
    std::string statusMessage{};
    bool        statusSuccess = true;

    // ── The render (PROJECT → EXPORT) ────────────────────────────────────────────────────────────
    //
    // The shell renders SYNCHRONOUSLY, on this thread, with the audio device paused — see the shell's
    // export action. Android needs a coroutine because Compose would ANR; a single-threaded frame loop
    // simply stops, and repaints itself from the progress callback. So these are written from inside
    // the render, and read by the frame it forces.
    bool  isRendering    = false;
    float renderProgress = 0.0f;

    // ── Is there unsaved work? ───────────────────────────────────────────────────────────────────
    //
    // TrackerController's `projectVersion` / `savedProjectVersion`. The counter is bumped in exactly
    // one place — `InputDispatcher::mark_modified`, which every edit in the app already funnels
    // through — and the SAVE / LOAD / NEW actions align the two. It is what gates the NEW PROJECT?
    // and EXIT? confirms: a clean project needs no question asked.
    int projectVersion      = 0;
    int savedProjectVersion = 0;

    bool project_dirty() const { return projectVersion != savedProjectVersion; }

    /** The .ptp this project came from (or was last saved to). Empty until it has one. */
    std::string projectPath{};

    /** Set by EXIT. The shell's frame loop reads it and leaves. */
    bool shouldQuit = false;

    // ── Theme ────────────────────────────────────────────────────────────────────────────────────
    Theme theme = theme_classic();
};

}  // namespace pt::ui
