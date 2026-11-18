#pragma once

// The instrument list's drawing; state is ui/instrument_picker.h.

#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/instrument_picker.h"
#include "ui/theme.h"

namespace pt::ui {

// No-op when it is not open.
void draw_instrument_picker(Canvas& c, const InstrumentPickerState& s,
                            const std::vector<songcore::Instrument>& instruments, const Theme& t);

}  // namespace pt::ui
