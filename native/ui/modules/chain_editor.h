#pragma once

// ─── CHAIN EDITOR ────────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/ChainEditorModule.kt. 16 phrase slots, each with a transpose.
//
// Transpose encoding: 00 = no change, 80 = −128 semitones, FF = +127 — a byte read as two's
// complement (`byte_to_signed_semitones` in songcore/timing.h). The cell shows the raw byte; the
// sequencer is what interprets it, and the editor deliberately does not second-guess that.
//
// The TSP column is empty *because the phrase slot is empty*, not because the transpose is 0 — an
// empty slot has nothing to transpose. That is why both cells below read `isEmpty` off `phraseRefs`.

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

#include <functional>

namespace pt::ui {

struct ChainEditorState {
    const songcore::Chain& chain;
    int  cursorRow     = 0;
    int  cursorColumn  = 1;
    int  playbackRow   = 0;
    bool isPlaying     = false;
    bool selectionMode = false;
    std::function<bool(int, int)> isCellSelected = [](int, int) { return false; };
    Theme theme = theme_classic();
};

struct ChainInputResult {
    bool modified = false;
    bool hasPhrase        = false;
    int  lastEditedPhrase = 0;
    bool hasTranspose        = false;
    int  lastEditedTranspose = 0;
};

class ChainEditorModule {
public:
    static constexpr int WIDTH  = 510;
    static constexpr int HEIGHT = 392;

    void draw(Canvas& c, int x, int y, const ChainEditorState& s) const;

    CursorContext cursor_context(const ChainEditorState& s) const;

    ChainInputResult handle_input(songcore::Chain& chain, int cursor_row, int cursor_column,
                                  const InputAction& action) const;

private:
    void draw_row(Canvas& c, int x, int y, int index, const ChainEditorState& s, int stepX, int phX,
                  int tspX) const;
};

}  // namespace pt::ui
