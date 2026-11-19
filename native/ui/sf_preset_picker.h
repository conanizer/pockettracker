#pragma once

// The list A raises on a SoundFont's PATCH row when its file holds more than one preset: every preset
// by number and name. A plays the one under the cursor from then on, B leaves. Choosing is
// ui/dispatch/layers/instrument_picker.cpp; drawing is ui/modules/instrument_picker_overlay.h.

#include <string>
#include <vector>

#include "ui/instrument_picker.h"

namespace pt::ui {

// Moves as the instrument list does (`instrument_picker_move`).
struct SfPresetPickerState : InstrumentPickerState {
    std::vector<std::string> names;   // the file's presets, in the PATCH row's order
};

}  // namespace pt::ui
