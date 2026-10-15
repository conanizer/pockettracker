#pragma once

// The FX helper's drawing; state and navigation are in ui/fx_helper.h. A full-frame modal the layout
// draws last.

#include "ui/canvas.h"
#include "ui/fx_helper.h"
#include "ui/theme.h"

namespace pt::ui {

// No-op when it is not open.
void draw_fx_helper(Canvas& c, const FxHelperState& s, const Theme& t);

}  // namespace pt::ui
