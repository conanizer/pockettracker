#pragma once

// ─── The INSTRUMENT screen's row geometry ────────────────────────────────────────────────────────
//
// A 1:1 port of core/data/InstrumentRowLayout.kt: the ONE table the cursor walks. Row stepping,
// spacer skipping, column snapping and LEFT/RIGHT column stepping all derive from it.
//
// It exists because the same geometry used to be re-encoded at four movement sites as spacer-skip
// literals and row sets, and one miss stranded the cursor on a spacer or off the screen. The DRAWN
// layout lives in ui/modules/instrument_editor.cpp, and this table must mirror it: add, remove or move
// a row there and the matching entry here changes too — one edit, in the file whose name says so.
//
// The column shape of each kind, which is what the movement code actually reads:
//
//   NAME   — columns 1..3, LEFT/RIGHT step by 1 (the value, then two buttons).
//   TRIPLE — columns 1 / 3 / 5, LEFT/RIGHT step by 2.
//   DUAL   — columns 1 / 3, LEFT/RIGHT jump straight between them.
//   SOURCE — LOAD (2) / EDIT (3); the cursor SNAPS to 2 on entry. A SoundFont caps at 2 — it has no
//            editable waveform, so there is no EDIT button, and a cursor allowed onto column 3 there
//            would sit on a cell that is not drawn.
//   SINGLE — column 1 only.
//   SPACER — not selectable; vertical movement steps straight over it.

#include <cstddef>

namespace pt::ui {

enum class InstrumentRowKind { NAME, TRIPLE, DUAL, SOURCE, SINGLE, SPACER };

/** Sampler: 16 rows. */
inline constexpr InstrumentRowKind INSTRUMENT_ROWS_SAMPLER[] = {
    InstrumentRowKind::NAME,    //  0  TYPE + LOAD + SAVE
    InstrumentRowKind::SINGLE,  //  1  NAME
    InstrumentRowKind::TRIPLE,  //  2  ROOT + DETUNE + TIC
    InstrumentRowKind::TRIPLE,  //  3  VOL + SLICE + PAN
    InstrumentRowKind::SPACER,  //  4
    InstrumentRowKind::SOURCE,  //  5  sample LOAD / EDIT
    InstrumentRowKind::SPACER,  //  6
    InstrumentRowKind::DUAL,    //  7  DRIVE + FILTER
    InstrumentRowKind::DUAL,    //  8  CRUSH + FREQ
    InstrumentRowKind::DUAL,    //  9  DWNSMPL + RES
    InstrumentRowKind::SPACER,  // 10
    InstrumentRowKind::DUAL,    // 11  REV + DEL
    InstrumentRowKind::SINGLE,  // 12  EQ
    InstrumentRowKind::DUAL,    // 13  LOOP + START
    InstrumentRowKind::DUAL,    // 14  LOOP ST + END
    InstrumentRowKind::DUAL,    // 15  LOOP END + REVERSE
};

/** SoundFont: 15 rows. It gains PRESET and loses the four sample-window rows. */
inline constexpr InstrumentRowKind INSTRUMENT_ROWS_SOUNDFONT[] = {
    InstrumentRowKind::NAME,    //  0  TYPE + LOAD + SAVE
    InstrumentRowKind::SINGLE,  //  1  NAME
    InstrumentRowKind::TRIPLE,  //  2  ROOT + DETUNE + TIC
    InstrumentRowKind::DUAL,    //  3  VOL + PAN
    InstrumentRowKind::SPACER,  //  4
    InstrumentRowKind::SOURCE,  //  5  SF2 LOAD (no EDIT)
    InstrumentRowKind::SINGLE,  //  6  PRESET
    InstrumentRowKind::SPACER,  //  7
    InstrumentRowKind::DUAL,    //  8  DRIVE + FILTER
    InstrumentRowKind::DUAL,    //  9  CRUSH + FREQ
    InstrumentRowKind::DUAL,    // 10  DWNSMPL + RES
    InstrumentRowKind::SPACER,  // 11
    InstrumentRowKind::SINGLE,  // 12  REV
    InstrumentRowKind::SINGLE,  // 13  DEL
    InstrumentRowKind::SINGLE,  // 14  EQ
};

inline constexpr int INSTRUMENT_ROWS_SAMPLER_COUNT =
    static_cast<int>(sizeof(INSTRUMENT_ROWS_SAMPLER) / sizeof(INSTRUMENT_ROWS_SAMPLER[0]));
inline constexpr int INSTRUMENT_ROWS_SOUNDFONT_COUNT =
    static_cast<int>(sizeof(INSTRUMENT_ROWS_SOUNDFONT) / sizeof(INSTRUMENT_ROWS_SOUNDFONT[0]));

/** How many rows the screen has for this instrument type. */
inline int instrument_row_count(bool is_soundfont) {
    return is_soundfont ? INSTRUMENT_ROWS_SOUNDFONT_COUNT : INSTRUMENT_ROWS_SAMPLER_COUNT;
}

/** The kind of row `row`. Out-of-range reads as SINGLE, as Kotlin's `getOrElse` does. */
inline InstrumentRowKind instrument_row_kind(bool is_soundfont, int row) {
    const int count = instrument_row_count(is_soundfont);
    if (row < 0 || row >= count) return InstrumentRowKind::SINGLE;
    return is_soundfont ? INSTRUMENT_ROWS_SOUNDFONT[static_cast<size_t>(row)]
                        : INSTRUMENT_ROWS_SAMPLER[static_cast<size_t>(row)];
}

/**
 * The SoundFont layout inserts PRESET at row 6, so every row below it shifts down by one. Kotlin
 * spells this `sfOffset` and adds it to a literal row number at each site; the same name is kept here
 * so the two read alike.
 */
inline int instrument_sf_offset(bool is_soundfont) { return is_soundfont ? 1 : 0; }

/**
 * The EQ row — 12 on a sampler, 14 on a SoundFont (the two extra rows above it are PRESET and the four
 * sample-window rows the SF layout drops, netting +2). Its column 1 is the cell that raises the EQ
 * EDITOR (S8).
 *
 * Named here rather than open-coded at the three sites that need it, because it is a NUMBER READ OFF
 * THE TABLE ABOVE and the table is what moves. Kotlin writes the literals `12` and `14` into
 * `openSubScreenAtCursor` and `handleSelect` and its own module, which is three places to forget when a
 * row is inserted — and S5 already found the shape of that bug once (`go_to_screen` silently skipping
 * the three screens S4 added).
 */
inline int instrument_eq_row(bool is_soundfont) { return is_soundfont ? 14 : 12; }

}  // namespace pt::ui
