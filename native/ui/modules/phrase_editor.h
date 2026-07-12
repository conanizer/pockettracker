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

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

struct PhraseEditorState {
    const songcore::Phrase& phrase;
    int                     cursorRow    = 0;
    int                     cursorColumn = 1;
    int                     playbackRow  = 0;
    bool                    isPlaying    = false;
    bool                    selectionMode = false;
    std::function<bool(int, int)> isCellSelected = [](int, int) { return false; };
    Theme                   theme = theme_classic();
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
                  const PhraseEditorState& s, int stepX, int noteX, int volX, int instX,
                  int fx1NameX, int fx1ValueX, int fx2NameX, int fx2ValueX, int fx3NameX,
                  int fx3ValueX) const;
};

}  // namespace pt::ui
