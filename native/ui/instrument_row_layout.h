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
//   NAME   — columns 1..3, LEFT/RIGHT step by 1 (a value, then two buttons: TYPE + LOAD + EDIT). On a
//            SoundFont the EDIT button is not drawn — there is no single waveform to edit — so the
//            cursor caps at column 2 there, and column 3 would sit on a cell that is not drawn. On
//            EXTERNAL neither button is drawn and the cap is 1. `instrument_name_row_max_column`.
//   TRIPLE — columns 1 / 3 / 5, LEFT/RIGHT step by 2.
//   DUAL   — columns 1 / 3, LEFT/RIGHT jump straight between them.
//   SOURCE — two buttons, columns 2 / 3 (SAVE + LOAD the .pti preset); the cursor SNAPS to 2 on entry.
//            Present on BOTH instrument types — a preset saves and loads either kind, so unlike the NAME
//            row above there is no per-type cap here.
//   SINGLE — column 1 only.
//   SPACER — not selectable; vertical movement steps straight over it.
//
// ⚠️ These functions took a `bool is_soundfont` until the MIDI plan's B4 — correct only while there
// were exactly TWO layouts. EXTERNAL is the third, so the parameter is the TYPE itself. Written as a
// switch with the SAMPLER arm last and unconditional, so the two original layouts keep byte-for-byte
// the answers they always gave (`tools/ptinput` byte-compares 21687 cases against the Kotlin original
// and is the check that says so) and a fourth type has to be named rather than silently defaulting.

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
    InstrumentRowKind::SINGLE,  //  6  PATCH (the SF2's internal patch selector)
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
 * EXTERNAL: 13 rows. It owns NO source file — no sample, no SF2 — so its row 0 is the TYPE cell alone
 * (no LOAD, no EDIT) and every row is a byte the device is actually sent rather than a DSP parameter.
 * None of the sampler's voice controls apply: there is no gain stage, no filter and no loop on our
 * side of the cable. VOL and PAN are here because `ExternalConsumer` folds them into note-on velocity
 * and CC 10 — they are the two that survive the trip.
 *
 * ⚠️ **Every row is a DUAL, and BANK is why.** The obvious layout packs CHAN/BANK/PROG onto one TRIPLE
 * — but BANK is 14-bit (CC0 MSB + CC32 LSB, `midi_out.h`) and prints as FOUR hex digits, and a TRIPLE's
 * third column starts 63px after its second. `ptshot` drew it: a bank of 1024 rendered as `00`, because
 * a 2-digit cell had silently masked it. DUAL rows give the value 140px, and the row list is what had
 * to change to make the number fit.
 *
 * ⚠️ **Row 5 is the INST PRESET row on all three layouts, and that is load-bearing** —
 * `instrument_open_at_cursor` tests row 5 col 2/3 for the .pti SAVE/LOAD by literal number. Moving it
 * here would take those two buttons away from EXTERNAL without a word.
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

/** The kind of row `row`. Out-of-range reads as SINGLE, as Kotlin's `getOrElse` does. */
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

/**
 * The rightmost cursor column on row 0 — the cap the NAME row's LEFT/RIGHT stepping honours.
 *
 * 3 on a sampler (TYPE, LOAD, EDIT), 2 on a SoundFont (no single waveform to edit), 1 on EXTERNAL
 * (no source at all). Read off the drawn row: a cursor past the cap sits on a cell that is not drawn.
 */
inline int instrument_name_row_max_column(songcore::InstrumentType type) {
    switch (type) {
        case songcore::InstrumentType::SOUNDFONT: return 2;
        case songcore::InstrumentType::EXTERNAL:  return 1;
        case songcore::InstrumentType::SAMPLER:   break;
    }
    return 3;
}

/**
 * The SoundFont layout inserts PATCH at row 6, so every row below it shifts down by one. Kotlin
 * spells this `sfOffset` and adds it to a literal row number at each site; the same name is kept here
 * so the two read alike. EXTERNAL does not share that spine at all — its tail is its own table — so
 * the offset is meaningless there and reads 0.
 */
inline int instrument_sf_offset(songcore::InstrumentType type) {
    return type == songcore::InstrumentType::SOUNDFONT ? 1 : 0;
}

/**
 * The EQ row — 12 on a sampler, 14 on a SoundFont (the two extra rows above it are PATCH and the four
 * sample-window rows the SF layout drops, netting +2). Its column 1 is the cell that raises the EQ
 * EDITOR (S8). **−1 on EXTERNAL, which has no EQ row** — there is no signal of ours to equalise — and
 * −1 is a row number no cursor can hold, so the callers' `cursorRow == instrument_eq_row(...)` test
 * goes false without a second condition to forget at each site.
 *
 * Named here rather than open-coded at the three sites that need it, because it is a NUMBER READ OFF
 * THE TABLE ABOVE and the table is what moves. Kotlin writes the literals `12` and `14` into
 * `openSubScreenAtCursor` and `handleSelect` and its own module, which is three places to forget when a
 * row is inserted — and S5 already found the shape of that bug once (`go_to_screen` silently skipping
 * the three screens S4 added).
 */
/**
 * ⚠️ On a SAMPLER the EQ row is a DUAL — its second column is `SLICE`, which moved down here when
 * `TSP` took its place beside VOL. On a SoundFont it stays a SINGLE: there are no slices to put
 * there. The row NUMBER is unchanged on both, which is what mattered — it is identity (this function,
 * and the dispatcher site that tests it) where a column is free.
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
