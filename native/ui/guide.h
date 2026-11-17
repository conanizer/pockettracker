#pragma once

// The guide: what the top panel says when TOP PANEL is GUIDE. The first line names the cell under the
// cursor, the two below are its controls — or, while L, R or B is held, what that button does here.
// A selection and LIVE mode have lines of their own; a status message takes the first line while it
// lasts. The words are in `help_text.h`; this picks them.

#include <string>

#include "ui/theme.h"

namespace pt::ui {

struct AppState;

struct GuideLines {
    struct Line {
        std::string text;   // at most HELP_MAX_CHARS glyphs
        Argb        ink = 0;
        Argb        keyInk = 0;   // non-zero: a controls line, its buttons drawn in this
    };
    Line line[3];
};

// Does the guide have the top panel? ⚠️ Then the status line and the selection readout are not drawn
// over it: the guide carries both.
bool guide_up(const AppState& s);

GuideLines guide_lines(const AppState& s);

}  // namespace pt::ui
