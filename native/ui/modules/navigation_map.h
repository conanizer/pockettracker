#pragma once

// ─── NAVIGATION MAP ──────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/NavigationMapModule.kt: the 5×5 grid in the bottom-right corner that
// says where you are and what R+DPAD can reach from here.
//
// It is the PICTURE of the grid that `ui/navigation.h` MOVES through — the two are written and read
// together, and a disagreement between them (a cell you can see but not reach, a screen you land on
// that is drawn nowhere) is the only interesting bug either can have.
//
// Data-driven, as the Kotlin is: the column layouts are a table, not a stack of `if`s. Only the main
// row (row 2 — S C P I T) and the CURRENT column are ever drawn; the other columns' context screens
// are not reachable from here, so showing them would be a lie about where R+DPAD goes.
//
// 115×105 px — five 23px cells across, five 21px rows down.

#include "ui/canvas.h"
#include "ui/screen.h"
#include "ui/theme.h"

namespace pt::ui {

struct NavigationMapState {
    ScreenType currentScreen = ScreenType::PHRASE;
    /** Which column a shared screen (PROJECT / MIXER / EFFECTS) was entered from. */
    int  sourceColumn       = 2;
    bool instrumentFromPool = false;  // on INSTRUMENT, entered via the pool's R+RIGHT
    Theme theme = theme_classic();
};

class NavigationMapModule {
public:
    static constexpr int WIDTH  = 115;
    static constexpr int HEIGHT = 105;

    static constexpr int CELL_WIDTH  = 23;
    static constexpr int CELL_HEIGHT = 21;

    void draw(Canvas& c, int x, int y, const NavigationMapState& s) const;
};

}  // namespace pt::ui
