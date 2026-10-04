#pragma once

// ─── MAPPING DESTINATION PICKER — the drawing ────────────────────────────────────────────────────
//
// The state and navigation live in ui/map_picker.h, canvas-free, so they can be driven without a
// renderer. A full-frame modal rather than a `Module`: the layout draws it LAST.

#include "ui/canvas.h"
#include "ui/map_picker.h"
#include "ui/theme.h"

namespace pt::ui {

/** Paint the overlay over the finished frame. No-op when it is not open. */
void draw_map_picker(Canvas& c, const MapPickerState& s, const Theme& t);

}  // namespace pt::ui
