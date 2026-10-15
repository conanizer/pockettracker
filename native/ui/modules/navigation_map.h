#pragma once

// The 5×5 navigation map, bottom right: where you are and what R+DPAD reaches (`SCREEN_GRID`). Only
// the main row and the current column are drawn.

#include "ui/canvas.h"
#include "ui/screen.h"
#include "ui/theme.h"

namespace pt::ui {

struct NavigationMapState {
    ScreenType currentScreen = ScreenType::PHRASE;
    // The column a shared screen was entered from.
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
