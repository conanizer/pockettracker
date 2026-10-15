#pragma once

// The phrase editor: 16 steps of Note | Vol | Inst | FX1 | FX2 | FX3. Like every module: draw(),
// cursor_context() (what is under the cursor and what can be done to it), handle_input(). Coordinates
// are 640×480 design pixels; the shell scales the frame.

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

    // The chain row and song cell this phrase is seen through (`C20/01 S01 T3`); −1 = none, as
    // always under NAV = POOL.
    int                     viaChain     = -1;
    int                     viaChainRow  = -1;
    int                     songRow      = -1;
    int                     songTrack    = -1;
    // Up to eight playheads (one phrase on three tracks plays at three steps); a marker is drawn
    // where `phraseId` is the phrase on display.
    TrackPlayhead           playheads[8] = {};
    bool                    selectionMode = false;
    std::function<bool(int, int)> isCellSelected = [](int, int) { return false; };
    Theme                   theme = theme_classic();

    // How far up EFFECT_TYPES an FX-type cell may step (cc::effect_type).
    int effectTypeCount = songcore::EFFECT_TYPE_COUNT;

    // For AUS/AUF only: a ramp can run into a later phrase of the chain, so whether a cell acts depends
    // on the chain walks this phrase is in (`find_ramp_cells`). Null pairs within the phrase.
    const songcore::Project* project = nullptr;

    // The instrument an empty note cell gets when typed into; its scale depends on it.
    int insertInstrument = -1;
};

// The edits the caller echoes to the engine (note preview, etc.).
struct PhraseInputResult {
    bool modified = false;
    bool hasNote  = false;
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

    // What was edited comes back in the result, for the caller to store and preview.
    PhraseInputResult handle_input(songcore::Phrase& phrase, int cursor_row, int cursor_column,
                                   const InputAction& action) const;

private:
    void draw_row(Canvas& c, int x, int y, int index, const songcore::PhraseStep& step,
                  const PhraseEditorState& s, const songcore::RampCells& rampCells, int hopRow,
                  int stepX, int noteX, int volX, int instX, int fx1NameX, int fx1ValueX,
                  int fx2NameX, int fx2ValueX, int fx3NameX, int fx3ValueX) const;
};

}  // namespace pt::ui
