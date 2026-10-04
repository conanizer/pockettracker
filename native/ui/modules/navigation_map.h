#pragma once

// ─── NAVIGATION MAP ──────────────────────────────────────────────────────────────────────────────
//
// The 5×5 grid in the bottom-right corner: where you are and what R+DPAD can reach. It is the PICTURE
// of the grid `ui/navigation.h` moves through — the two must agree. Only the main row (row 2 — S C P I
// T) and the CURRENT column are drawn; other columns' screens are not reachable from here.
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
