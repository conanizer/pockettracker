#pragma once

// While A is held on a cell whose context names a `ValueUnit`, the status line shows what the value
// means: the value in Hz or ms on the first line, what it is measured against on the second. The
// cells say only which unit; every word and number here comes from the curve the engine plays.

#include <string>

#include "ui/cursor.h"

namespace pt::ui {

struct AppState;

struct ValueHint {
    std::string head;     // empty = nothing to show
    std::string detail;
};

ValueHint value_hint(ValueUnit unit, int value, const AppState& s);

// The unit an FX command's value is in; NONE for the rest.
ValueUnit fx_value_unit(int effect_code);

// "0.31Hz", "440Hz", "1.2kHz", "16kHz". From 20 Hz up it is the EQ's FREQ readout, which its
// stepping stops on — keep those three forms.
std::string format_hz(float hz);

// "4.2ms", "167ms", "1.25s", "12.5s".
std::string format_ms(double ms);

}  // namespace pt::ui
