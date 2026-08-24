#pragma once

// ─── PHRASE EDITOR ───────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/PhraseEditorModule.kt — and the first one, so it is also the template
// every other module follows (linux-port-plan §4.7: "port as 1:1 named pairs, each implementing the
// same trio the Kotlin side already standardises").
//
//   draw()               paint the module at (x, y) on the canvas
//   cursor_context()     "what is under the cursor, and what can be done to it"
//   handle_input()       apply an InputAction to the data
//
// 16 steps × columns: Step | Note | Vol | Inst | FX1 | FX2 | FX3.  510×392 px.
//
// The Kotlin file is the executable spec. Two differences from it, both deliberate and both
// systematic across the port rather than particular to this module:
//
//   1. No `scale` parameter — the canvas IS the 640×480 design and the shell scales the frame
//      (canvas.h). Every coordinate below is therefore a design pixel, byte-for-byte what Kotlin
//      computes before it multiplies.
//   2. The state bundle holds a `const Phrase&` where Kotlin holds a `Phrase` — Compose gets a
//      snapshot to diff against; a redraw-from-scratch renderer needs no such thing.

#include <functional>

#include "songcore/automation.h"
#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/playhead.h"
#include "ui/theme.h"

namespace pt::ui {

struct PhraseEditorState {
    const songcore::Phrase& phrase;
    int                     cursorRow    = 0;
    int                     cursorColumn = 1;

    // The chain row and song cell this phrase is being looked at THROUGH — `C20/01 S01 T3` beside the
    // title. −1 on any of them means "no cell", which is what NAV = POOL always answers. See
    // ChainEditorState for why this is not decoration.
    int                     viaChain     = -1;
    int                     viaChainRow  = -1;
    int                     songRow      = -1;
    int                     songTrack    = -1;
    // ONE screen, EIGHT possible playheads: the same phrase placed on three tracks is being played
    // by three of them, at three different steps. A marker is drawn where `phraseId` matches the
    // phrase on display, so auditioning a phrase marks this screen and nothing upstream of it.
    TrackPlayhead           playheads[8] = {};
    bool                    selectionMode = false;
    std::function<bool(int, int)> isCellSelected = [](int, int) { return false; };
    Theme                   theme = theme_classic();

    /** How far up songcore::EFFECT_TYPES an FX-type cell may be stepped — see cc::effect_type. */
    int effectTypeCount = songcore::EFFECT_TYPE_COUNT;

    // The project this phrase belongs to, for AUS/AUF alone. A ramp may run from one phrase into a
    // later one of the same chain, so whether an AUS/AUF cell is doing anything is a question about
    // the CHAIN WALKS the phrase appears in, not about the phrase in front of you — and the phrase's
    // own id is what finds them (`find_ramp_cells`). Without it the editor pairs within the phrase.
    const songcore::Project* project = nullptr;
};

/** What `handle_input` did — the edits the caller must echo to the engine (note preview, etc.). */
struct PhraseInputResult {
    bool modified = false;
    bool hasNote  = false;   // lastEditedNote — Kotlin's `Note?`, minus the optional
    songcore::Note   lastEditedNote{};
    bool hasVolume = false;  // lastEditedVolume
    int  lastEditedVolume = 0;
    bool hasInstrument = false;  // lastEditedInstrument
    int  lastEditedInstrument = 0;
};

class PhraseEditorModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const PhraseEditorState& s) const;

    CursorContext cursor_context(const PhraseEditorState& s) const;

    /**
     * `instrument_controller.lastEditedInstrument` in Kotlin is written from here as a side effect;
     * that controller does not exist yet, so the edit is reported back in the result instead and the
     * caller stores it. Same information, no back-reference from a module into a controller.
     */
    PhraseInputResult handle_input(songcore::Phrase& phrase, int cursor_row, int cursor_column,
                                   const InputAction& action) const;

private:
    void draw_row(Canvas& c, int x, int y, int index, const songcore::PhraseStep& step,
                  const PhraseEditorState& s, const songcore::RampCells& rampCells,
                  int stepX, int noteX, int volX, int instX, int fx1NameX, int fx1ValueX,
                  int fx2NameX, int fx2ValueX, int fx3NameX, int fx3ValueX) const;
};

}  // namespace pt::ui
