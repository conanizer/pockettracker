#pragma once

// The chain editor: 16 phrase slots, each with a transpose shown as the raw byte (00 = none, 80 = −128,
// FF = +127). The TSP cell is empty because the phrase slot is, not because the transpose is 0.

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/playhead.h"
#include "ui/theme.h"

#include <functional>

namespace pt::ui {

struct ChainEditorState {
    const songcore::Chain& chain;
    int  cursorRow     = 0;
    int  cursorColumn  = 1;

    // The song cell this chain is seen through (`S01 T3`); −1 = none. Under NAV = SONG it decides
    // where the next B+D-pad goes.
    int  songRow = -1;
    int  songTrack = -1;
    // Two tracks can be in this chain at different rows; a marker where `chainId` matches.
    TrackPlayhead playheads[8] = {};
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
