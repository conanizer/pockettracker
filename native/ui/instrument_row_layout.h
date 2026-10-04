#pragma once

// ─── The INSTRUMENT screen's row geometry ────────────────────────────────────────────────────────
//
// The ONE table the cursor walks: row stepping, spacer skipping, column snapping and LEFT/RIGHT all
// derive from it. ⚠️ It must mirror the DRAWN layout in ui/modules/instrument_editor.cpp — add, remove
// or move a row there and its entry here changes too.
//
//   NAME   — columns 1..3 by 1 (TYPE, LOAD, EDIT). No EDIT on a SoundFont (cap 2), neither button on
//            EXTERNAL (cap 1) — `instrument_name_row_max_column`.
//   TRIPLE — columns 1 / 3 / 5, LEFT/RIGHT step by 2.
//   DUAL   — columns 1 / 3, LEFT/RIGHT jump between them.
//   SOURCE — SAVE + LOAD of the .pti preset, columns 2 / 3; the cursor SNAPS to 2 on entry. Every type.
//   SINGLE — column 1 only.
//   SPACER — not selectable; vertical movement steps over it.
//
// The functions switch on the TYPE with the SAMPLER arm last and unconditional, so a new type must be
// named rather than silently defaulting (the input tests byte-compare the existing layouts).

#include <cstddef>

#include "songcore/model.h"

namespace pt::ui {

enum class InstrumentRowKind { NAME, TRIPLE, DUAL, SOURCE, SINGLE, SPACER };

/** Sampler: 16 rows. */
inline constexpr InstrumentRowKind INSTRUMENT_ROWS_SAMPLER[] = {
    InstrumentRowKind::NAME,    //  0  TYPE + source LOAD + EDIT
    InstrumentRowKind::SINGLE,  //  1  NAME
    InstrumentRowKind::TRIPLE,  //  2  ROOT + DETUNE + TIC
    InstrumentRowKind::TRIPLE,  //  3  VOL + TSP + PAN
    InstrumentRowKind::SPACER,  //  4
    InstrumentRowKind::SOURCE,  //  5  INST PRESET: SAVE / LOAD (.pti)
    InstrumentRowKind::SPACER,  //  6
    InstrumentRowKind::DUAL,    //  7  DRIVE + FILTER
    InstrumentRowKind::DUAL,    //  8  CRUSH + FREQ
    InstrumentRowKind::DUAL,    //  9  DWNSMPL + RES
    InstrumentRowKind::SPACER,  // 10
    InstrumentRowKind::DUAL,    // 11  REV + DEL
    InstrumentRowKind::DUAL,    // 12  EQ + SLICE
    InstrumentRowKind::DUAL,    // 13  LOOP + START
    InstrumentRowKind::DUAL,    // 14  LOOP ST + END
    InstrumentRowKind::DUAL,    // 15  LOOP END + REVERSE
};

/** SoundFont: 15 rows. It gains PATCH and loses the four sample-window rows. */
inline constexpr InstrumentRowKind INSTRUMENT_ROWS_SOUNDFONT[] = {
    InstrumentRowKind::NAME,    //  0  TYPE + source LOAD (no EDIT on SF)
    InstrumentRowKind::SINGLE,  //  1  NAME
    InstrumentRowKind::TRIPLE,  //  2  ROOT + DETUNE + TIC
    InstrumentRowKind::TRIPLE,  //  3  VOL + TSP + PAN
    InstrumentRowKind::SPACER,  //  4
    InstrumentRowKind::SOURCE,  //  5  INST PRESET: SAVE / LOAD (.pti)
    InstrumentRowKind::SINGLE,  //  6  PATCH (the SF2's patch selector)
    InstrumentRowKind::SPACER,  //  7
    InstrumentRowKind::DUAL,    //  8  DRIVE + FILTER
    InstrumentRowKind::DUAL,    //  9  CRUSH + FREQ
    InstrumentRowKind::DUAL,    // 10  DWNSMPL + RES
    InstrumentRowKind::SPACER,  // 11
    InstrumentRowKind::SINGLE,  // 12  REV
    InstrumentRowKind::SINGLE,  // 13  DEL
    InstrumentRowKind::SINGLE,  // 14  EQ
};

/**
 * EXTERNAL: 13 rows. It owns no source file, so row 0 is TYPE alone, and every row is a byte the
 * device is sent. VOL and PAN survive as note-on velocity and CC 10 (`ExternalConsumer`).
 * ⚠️ Every row is a DUAL because BANK is 14-bit and prints four hex digits — too wide for a TRIPLE's
 * third column.
 * ⚠️ Row 5 is the INST PRESET row on all three layouts — `instrument_open_at_cursor` tests row 5 col
 * 2/3 by literal number.
 */
inline constexpr InstrumentRowKind INSTRUMENT_ROWS_EXTERNAL[] = {
    InstrumentRowKind::NAME,    //  0  TYPE (column 1 only — nothing to LOAD or EDIT)
    InstrumentRowKind::SINGLE,  //  1  NAME
    InstrumentRowKind::DUAL,    //  2  CHAN + BANK
    InstrumentRowKind::DUAL,    //  3  PROG + LEN
    InstrumentRowKind::SPACER,  //  4
    InstrumentRowKind::SOURCE,  //  5  INST PRESET: SAVE / LOAD (.pti)
    InstrumentRowKind::SPACER,  //  6
    InstrumentRowKind::DUAL,    //  7  VOL + PAN
    InstrumentRowKind::DUAL,    //  8  TSP + TIC
    InstrumentRowKind::DUAL,    //  9  CC A: number + default
    InstrumentRowKind::DUAL,    // 10  CC B
    InstrumentRowKind::DUAL,    // 11  CC C
    InstrumentRowKind::DUAL,    // 12  CC D
};

inline constexpr int INSTRUMENT_ROWS_SAMPLER_COUNT =
    static_cast<int>(sizeof(INSTRUMENT_ROWS_SAMPLER) / sizeof(INSTRUMENT_ROWS_SAMPLER[0]));
inline constexpr int INSTRUMENT_ROWS_SOUNDFONT_COUNT =
    static_cast<int>(sizeof(INSTRUMENT_ROWS_SOUNDFONT) / sizeof(INSTRUMENT_ROWS_SOUNDFONT[0]));
inline constexpr int INSTRUMENT_ROWS_EXTERNAL_COUNT =
    static_cast<int>(sizeof(INSTRUMENT_ROWS_EXTERNAL) / sizeof(INSTRUMENT_ROWS_EXTERNAL[0]));

/** The first row of the EXTERNAL layout's CC block — CC A. The three below it follow. */
inline constexpr int INSTRUMENT_EXTERNAL_CC_ROW = 9;

/** How many rows the screen has for this instrument type. */
inline int instrument_row_count(songcore::InstrumentType type) {
    switch (type) {
        case songcore::InstrumentType::SOUNDFONT: return INSTRUMENT_ROWS_SOUNDFONT_COUNT;
        case songcore::InstrumentType::EXTERNAL:  return INSTRUMENT_ROWS_EXTERNAL_COUNT;
        case songcore::InstrumentType::SAMPLER:   break;
    }
    return INSTRUMENT_ROWS_SAMPLER_COUNT;
}

/** The kind of row `row`; out-of-range reads as SINGLE. */
inline InstrumentRowKind instrument_row_kind(songcore::InstrumentType type, int row) {
    const int count = instrument_row_count(type);
    if (row < 0 || row >= count) return InstrumentRowKind::SINGLE;
    const size_t r = static_cast<size_t>(row);
    switch (type) {
        case songcore::InstrumentType::SOUNDFONT: return INSTRUMENT_ROWS_SOUNDFONT[r];
        case songcore::InstrumentType::EXTERNAL:  return INSTRUMENT_ROWS_EXTERNAL[r];
        case songcore::InstrumentType::SAMPLER:   break;
    }
    return INSTRUMENT_ROWS_SAMPLER[r];
}

/** Does row 0 carry a source LOAD button (and, on a sampler, an EDIT)? EXTERNAL has no source. */
inline bool instrument_has_source_row(songcore::InstrumentType type) {
    return type != songcore::InstrumentType::EXTERNAL;
}

/** The rightmost cursor column on row 0: 3 on a sampler (TYPE, LOAD, EDIT), 2 on a SoundFont, 1 on
 *  EXTERNAL — read off the drawn row. */
inline int instrument_name_row_max_column(songcore::InstrumentType type) {
    switch (type) {
        case songcore::InstrumentType::SOUNDFONT: return 2;
        case songcore::InstrumentType::EXTERNAL:  return 1;
        case songcore::InstrumentType::SAMPLER:   break;
    }
    return 3;
}

/** The SoundFont layout inserts PATCH at row 6, shifting every row below it down one. 0 on EXTERNAL,
 *  whose tail is its own table. */
inline int instrument_sf_offset(songcore::InstrumentType type) {
    return type == songcore::InstrumentType::SOUNDFONT ? 1 : 0;
}

/**
 * The EQ row (which raises the EQ EDITOR): 12 on a sampler, 14 on a SoundFont, read off the tables
 * above so an inserted row cannot strand a literal. −1 on EXTERNAL (nothing of ours to equalise) — a
 * row no cursor can hold, so `cursorRow == instrument_eq_row(...)` needs no second condition.
 * On a sampler it is a DUAL (EQ + SLICE); on a SoundFont a SINGLE. The row NUMBER is the identity.
 */
inline int instrument_eq_row(songcore::InstrumentType type) {
    switch (type) {
        case songcore::InstrumentType::SOUNDFONT: return 14;
        case songcore::InstrumentType::EXTERNAL:  return -1;
        case songcore::InstrumentType::SAMPLER:   break;
    }
    return 12;
}

}  // namespace pt::ui
