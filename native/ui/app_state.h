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
#include "ui/selection.h"

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

    /**
     * The SETTINGS "CURSOR" row: REMEMBER restores each screen's last position, REFRESH (the default,
     * as on Android) resets it to the top-left editable cell on every entry.
     */
    bool cursorRemember = false;

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
    // "CHAIN CLONED" / "NO FREE PHRASES" — what L+B+A reports back.
    std::string statusMessage{};
    bool        statusSuccess = true;

    /** SETTINGS "NOTE PREVIEW": play the note an edit just wrote. On by default, as on Android. */
    bool notePreviewEnabled = true;

    // ── Theme ────────────────────────────────────────────────────────────────────────────────────
    Theme theme = theme_classic();
};

}  // namespace pt::ui
