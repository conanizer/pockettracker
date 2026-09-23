#pragma once

// ─── MAPPING DESTINATION PICKER — the drawing ────────────────────────────────────────────────────
//
// The STATE and the navigation live in ui/map_picker.h and know nothing about a canvas; this is only
// the paint. Same arrangement as the FX helper, and for the same reason: `ptdispatch` can then drive
// the navigation without linking a renderer.
//
// Not a `Module` — it is not laid out at an (x, y) inside the editor area but covers the whole
// 640×480 frame, backdrop included, so it takes the canvas and the layout draws it LAST.

#include "ui/canvas.h"
#include "ui/map_picker.h"
#include "ui/theme.h"

namespace pt::ui {

/** Paint the overlay over the finished frame. No-op when it is not open. */
void draw_map_picker(Canvas& c, const MapPickerState& s, const Theme& t);

}  // namespace pt::ui
