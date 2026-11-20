#pragma once

// The instrument list's drawing, the SoundFont preset list's and MACRO's model list; state is
// ui/instrument_picker.h and ui/sf_preset_picker.h.

#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/instrument_picker.h"
#include "ui/sf_preset_picker.h"
#include "ui/theme.h"

namespace pt::ui {

// No-op when it is not open.
void draw_instrument_picker(Canvas& c, const InstrumentPickerState& s,
                            const std::vector<songcore::Instrument>& instruments, const Theme& t);
void draw_sf_preset_picker(Canvas& c, const SfPresetPickerState& s, const Theme& t);
void draw_macro_model_picker(Canvas& c, const InstrumentPickerState& s, const Theme& t);

}  // namespace pt::ui
