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

    // The live cursor of the grid editors (SONG / CHAIN / PHRASE share it; TABLE, INSTRUMENT and the
    // rest carry their own, exactly as TrackerController does).
    int cursorRow    = 0;
    int cursorColumn = 1;

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

    // ── Selection ────────────────────────────────────────────────────────────────────────────────
    // The multi-tap CELL/ROW/SCREEN selection and the clipboard land with the dispatcher; the grid
    // editors already ask these two questions, so they exist now and answer "no selection".
    bool selectionMode = false;
    bool is_cell_selected(int /*row*/, int /*column*/) const { return false; }

    // ── Theme ────────────────────────────────────────────────────────────────────────────────────
    Theme theme = theme_classic();
};

}  // namespace pt::ui
