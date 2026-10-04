#pragma once

// ─── FX HELPER OVERLAY — the drawing ─────────────────────────────────────────────────────────────
//
// The state and navigation live in ui/fx_helper.h, canvas-free. A full-frame modal rather than a
// `Module`: it takes the canvas, not a position, and the layout draws it LAST.

#include "ui/canvas.h"
#include "ui/fx_helper.h"
#include "ui/theme.h"

namespace pt::ui {

/** Paint the overlay over the finished frame. No-op when it is not open. */
void draw_fx_helper(Canvas& c, const FxHelperState& s, const Theme& t);

}  // namespace pt::ui
