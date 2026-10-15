#pragma once

// The INSTRUMENT screen: one table per instrument type of what each row is and which cell sits in
// each column. The cursor walks it, the module draws it, and everything else switches on the cell —
// with no `default`, so a new cell warns at every switch.
//   NAME   — columns 1 / 2 / 3: TYPE, then the source LOAD and EDIT where the type has them.
//   TRIPLE — columns 1 / 3 / 5.   DUAL — columns 1 / 3.   SINGLE — column 1.
//   SOURCE — the .pti preset's SAVE + LOAD, columns 2 / 3.   SPACER — skipped.
// ⚠️ The cursor stores a row's number: a row inserted mid-table moves every cursor below it.

#include <cstddef>
#include <iterator>

#include "songcore/model.h"

namespace pt::ui {

enum class InstrumentRowKind { NAME, TRIPLE, DUAL, SOURCE, SINGLE, SPACER };

enum class InstrumentCell {
    NONE,
    // every type
    TYPE, SOURCE_LOAD, SOURCE_EDIT, NAME, PRESET_SAVE, PRESET_LOAD, VOL, PAN, TSP, TIC,
    // sampler and SoundFont
    ROOT, DETUNE, DRIVE, FILTER, CRUSH, FREQ, DOWNSAMPLE, RES, REV, DEL, EQ,
    // SoundFont
    PATCH,
    // sampler
    SLICE, LOOP, START, LOOP_START, END, LOOP_END, REVERSE,
    // EXTERNAL
    MIDI_CHANNEL, MIDI_BANK, MIDI_PROGRAM, MIDI_LENGTH, CC_NUMBER, CC_VALUE,
};

struct InstrumentRow {
    InstrumentRowKind kind;
    InstrumentCell    cells[3];   // in the order of the kind's columns (`instrument_row_columns`)
    int               ccSlot;     // CC rows: which of the instrument's CC slots; −1 elsewhere

    constexpr InstrumentRow(InstrumentRowKind k, InstrumentCell a = InstrumentCell::NONE,
                            InstrumentCell b = InstrumentCell::NONE,
                            InstrumentCell c = InstrumentCell::NONE, int slot = -1)
        : kind(k), cells{a, b, c}, ccSlot(slot) {}
};

namespace instrument_rows {
using K = InstrumentRowKind;
using C = InstrumentCell;

inline constexpr InstrumentRow SAMPLER[] = {
    {K::NAME, C::TYPE, C::SOURCE_LOAD, C::SOURCE_EDIT},  //  0
    {K::SINGLE, C::NAME},                                //  1
    {K::TRIPLE, C::ROOT, C::DETUNE, C::TIC},             //  2
    {K::TRIPLE, C::VOL, C::TSP, C::PAN},                 //  3
    {K::SPACER},                                         //  4
    {K::SOURCE, C::PRESET_SAVE, C::PRESET_LOAD},         //  5
    {K::SPACER},                                         //  6
    {K::DUAL, C::DRIVE, C::FILTER},                      //  7
    {K::DUAL, C::CRUSH, C::FREQ},                        //  8
    {K::DUAL, C::DOWNSAMPLE, C::RES},                    //  9
    {K::SPACER},                                         // 10
    {K::DUAL, C::REV, C::DEL},                           // 11
    {K::DUAL, C::EQ, C::SLICE},                          // 12
    {K::DUAL, C::LOOP, C::START},                        // 13
    {K::DUAL, C::LOOP_START, C::END},                    // 14
    {K::DUAL, C::LOOP_END, C::REVERSE},                  // 15
};

// No EDIT and no sample window; PATCH picks a patch inside the SF2.
inline constexpr InstrumentRow SOUNDFONT[] = {
    {K::NAME, C::TYPE, C::SOURCE_LOAD},                  //  0
    {K::SINGLE, C::NAME},                                //  1
    {K::TRIPLE, C::ROOT, C::DETUNE, C::TIC},             //  2
    {K::TRIPLE, C::VOL, C::TSP, C::PAN},                 //  3
    {K::SPACER},                                         //  4
    {K::SOURCE, C::PRESET_SAVE, C::PRESET_LOAD},         //  5
    {K::SINGLE, C::PATCH},                               //  6
    {K::SPACER},                                         //  7
    {K::DUAL, C::DRIVE, C::FILTER},                      //  8
    {K::DUAL, C::CRUSH, C::FREQ},                        //  9
    {K::DUAL, C::DOWNSAMPLE, C::RES},                    // 10
    {K::SPACER},                                         // 11
    {K::SINGLE, C::REV},                                 // 12
    {K::SINGLE, C::DEL},                                 // 13
    {K::SINGLE, C::EQ},                                  // 14
};

// Every cell is a byte the device is sent. DUALs, because BANK prints four hex digits.
inline constexpr InstrumentRow EXTERNAL[] = {
    {K::NAME, C::TYPE},                                  //  0
    {K::SINGLE, C::NAME},                                //  1
    {K::DUAL, C::MIDI_CHANNEL, C::MIDI_BANK},            //  2
    {K::DUAL, C::MIDI_PROGRAM, C::MIDI_LENGTH},          //  3
    {K::SPACER},                                         //  4
    {K::SOURCE, C::PRESET_SAVE, C::PRESET_LOAD},         //  5
    {K::SPACER},                                         //  6
    {K::DUAL, C::VOL, C::PAN},                           //  7
    {K::DUAL, C::TSP, C::TIC},                           //  8
    {K::DUAL, C::CC_NUMBER, C::CC_VALUE, C::NONE, 0},    //  9
    {K::DUAL, C::CC_NUMBER, C::CC_VALUE, C::NONE, 1},    // 10
    {K::DUAL, C::CC_NUMBER, C::CC_VALUE, C::NONE, 2},    // 11
    {K::DUAL, C::CC_NUMBER, C::CC_VALUE, C::NONE, 3},    // 12
};
}  // namespace instrument_rows

inline int instrument_row_count(songcore::InstrumentType type) {
    switch (type) {
        case songcore::InstrumentType::SOUNDFONT: return static_cast<int>(std::size(instrument_rows::SOUNDFONT));
        case songcore::InstrumentType::EXTERNAL:  return static_cast<int>(std::size(instrument_rows::EXTERNAL));
        case songcore::InstrumentType::SAMPLER:   break;
    }
    return static_cast<int>(std::size(instrument_rows::SAMPLER));
}

// Out of range reads as an empty SINGLE.
inline InstrumentRow instrument_row(songcore::InstrumentType type, int row) {
    if (row < 0 || row >= instrument_row_count(type)) return InstrumentRow{InstrumentRowKind::SINGLE};
    const size_t r = static_cast<size_t>(row);
    switch (type) {
        case songcore::InstrumentType::SOUNDFONT: return instrument_rows::SOUNDFONT[r];
        case songcore::InstrumentType::EXTERNAL:  return instrument_rows::EXTERNAL[r];
        case songcore::InstrumentType::SAMPLER:   break;
    }
    return instrument_rows::SAMPLER[r];
}

inline InstrumentRowKind instrument_row_kind(songcore::InstrumentType type, int row) {
    return instrument_row(type, row).kind;
}

// The cursor column of each of a row's cells; 0 past the last.
inline int instrument_row_column(InstrumentRowKind kind, int index) {
    static constexpr int NAME[] = {1, 2, 3}, TRIPLE[] = {1, 3, 5}, DUAL[] = {1, 3, 0},
                         SOURCE[] = {2, 3, 0}, SINGLE[] = {1, 0, 0};
    if (index < 0 || index > 2) return 0;
    const size_t i = static_cast<size_t>(index);
    switch (kind) {
        case InstrumentRowKind::NAME:   return NAME[i];
        case InstrumentRowKind::TRIPLE: return TRIPLE[i];
        case InstrumentRowKind::DUAL:   return DUAL[i];
        case InstrumentRowKind::SOURCE: return SOURCE[i];
        case InstrumentRowKind::SINGLE: return SINGLE[i];
        case InstrumentRowKind::SPACER: return 0;
    }
    return 0;
}

// NONE where nothing is drawn.
inline InstrumentCell instrument_cell_at(songcore::InstrumentType type, int row, int column) {
    const InstrumentRow r = instrument_row(type, row);
    for (int i = 0; i < 3; ++i)
        if (column != 0 && instrument_row_column(r.kind, i) == column) return r.cells[i];
    return InstrumentCell::NONE;
}

// The rightmost cursor column on row 0.
inline int instrument_name_row_max_column(songcore::InstrumentType type) {
    const InstrumentRow r = instrument_row(type, 0);
    int last = 1;
    for (int i = 0; i < 3; ++i)
        if (r.cells[i] != InstrumentCell::NONE) last = instrument_row_column(r.kind, i);
    return last;
}

struct InstrumentCursor {
    int row;
    int column;
};

// Where a cursor from another type's layout lands: unchanged on a cell, else the row below a spacer
// and the nearest column at or left of its own.
inline InstrumentCursor instrument_nearest_cell(songcore::InstrumentType type, int row, int column) {
    const int count = instrument_row_count(type);
    int       r     = row < 0 ? 0 : (row >= count ? count - 1 : row);
    while (instrument_row_kind(type, r) == InstrumentRowKind::SPACER && r + 1 < count) ++r;

    const InstrumentRow ir = instrument_row(type, r);
    int first = 1, best = 0;
    for (int i = 2; i >= 0; --i) {
        if (ir.cells[i] == InstrumentCell::NONE) continue;
        const int c = instrument_row_column(ir.kind, i);
        first = c;
        if (best == 0 && c <= column) best = c;
    }
    return {r, best != 0 ? best : first};
}

// EXTERNAL has no source.
inline bool instrument_has_source_row(songcore::InstrumentType type) {
    return instrument_cell_at(type, 0, 2) == InstrumentCell::SOURCE_LOAD;
}

}  // namespace pt::ui
