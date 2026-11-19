#pragma once

// The rows that step a folder of presets, and what a screen needs to draw one (`preset_folder.h`).

namespace pt::ui {

// ⚠️ A number is what `AppState::presetRows` is indexed by; NONE is no pending step.
enum class PresetRow { NONE = -1, SCALE, GROOVE, REVERB, DELAY, THEME, COUNT };
inline constexpr int PRESET_ROW_COUNT = static_cast<int>(PresetRow::COUNT);

// A row as the screen needs it: its place in the list (−1: in no file), the list's length, the entry
// A+B returns to, and the `*` — the values have moved off the file the row is named after.
struct PresetRowView {
    int  index        = -1;
    int  count        = 0;
    int  defaultIndex = 0;
    bool marked       = false;
};

}  // namespace pt::ui
