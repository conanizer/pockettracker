#pragma once

// The mapping-destination picker's drawing; state and navigation are in ui/map_picker.h. A
// full-frame modal the layout draws last.

#include "ui/canvas.h"
#include "ui/map_picker.h"
#include "ui/theme.h"

namespace pt::ui {

// No-op when it is not open.
void draw_map_picker(Canvas& c, const MapPickerState& s, const Theme& t);

}  // namespace pt::ui
