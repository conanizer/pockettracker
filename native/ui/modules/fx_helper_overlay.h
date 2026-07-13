#pragma once

// ─── FX HELPER OVERLAY — the drawing ─────────────────────────────────────────────────────────────
//
// The C++ twin of PixelPerfectRenderer.drawFxHelper. The STATE and the navigation live in
// ui/fx_helper.h and know nothing about a canvas; this is only the paint.
//
// It is not a `Module` like the grid editors, because it is not laid out at an (x, y) inside the
// editor area — it is a modal that covers the whole 640×480 frame, backdrop included. It therefore
// takes the canvas rather than a position, and the layout draws it LAST, over everything.

#include "ui/canvas.h"
#include "ui/fx_helper.h"
#include "ui/theme.h"

namespace pt::ui {

/** Paint the overlay over the finished frame. No-op when it is not open. */
void draw_fx_helper(Canvas& c, const FxHelperState& s, const Theme& t);

}  // namespace pt::ui
