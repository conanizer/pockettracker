#pragma once

// The list A raises on a phrase step's instrument cell: every slot by number and name. A writes the one
// under the cursor into the cell, B leaves it. Choosing is ui/dispatch/layers/instrument_picker.cpp;
// drawing is ui/modules/instrument_picker_overlay.h.

namespace pt::ui {

inline constexpr int INSTRUMENT_PICKER_ROWS = 14;   // shown at once; the list scrolls past that

struct InstrumentPickerState {
    bool isOpen = false;
    int  count  = 0;   // slots listed
    int  cursor = 0;   // the slot under the cursor
    int  top    = 0;   // the first row shown
};

// UP/DOWN wrap; a page (LEFT/RIGHT) stops at either end.
inline void instrument_picker_move(InstrumentPickerState& s, int delta, bool page) {
    if (s.count <= 0) return;
    if (page) {
        s.cursor += delta;
        if (s.cursor < 0) s.cursor = 0;
        if (s.cursor >= s.count) s.cursor = s.count - 1;
    } else {
        s.cursor = ((s.cursor + delta) % s.count + s.count) % s.count;
    }
    if (s.cursor < s.top) s.top = s.cursor;
    if (s.cursor >= s.top + INSTRUMENT_PICKER_ROWS) s.top = s.cursor - INSTRUMENT_PICKER_ROWS + 1;
}

}  // namespace pt::ui
