#pragma once

// PHRASE's and TABLE's command columns: each slot (1..3) is a TYPE column, then its VALUE.

#include "songcore/model.h"

namespace pt::ui {

inline constexpr int PHRASE_FX_COLUMN = 4;   // FX1's TYPE on PHRASE
inline constexpr int TABLE_FX_COLUMN  = 3;   // FX1's TYPE on TABLE

// The slot a column belongs to, or 0. `first` is the screen's FX1 TYPE column.
inline constexpr int fx_slot_of_column(int column, int first) {
    return column >= first && column < first + 2 * songcore::FX_SLOTS ? (column - first) / 2 + 1 : 0;
}
inline constexpr bool is_fx_type_column(int column, int first) {
    return fx_slot_of_column(column, first) != 0 && (column - first) % 2 == 0;
}
inline constexpr int fx_type_column(int slot, int first) { return first + 2 * (slot - 1); }

}  // namespace pt::ui
