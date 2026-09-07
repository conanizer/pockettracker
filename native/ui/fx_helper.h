#pragma once

// ─── The FX helper overlay ───────────────────────────────────────────────────────────────────────
//
// The modal picker that opens when A+UP or A+DOWN is pressed while the cursor sits on an FX *type*
// column (PHRASE cols 4/6/8, TABLE cols 3/5/7): the effects in six-column blocks under a heading per
// group, with the highlighted one's documentation above them.
//
// It exists because a tracker's FX column is otherwise unusable — A+RIGHT steps blindly through dozens
// of three-letter codes with nothing on screen to say what "PVX" or "THO" does. Holding A and reading
// is how you find an effect; releasing A is how you pick it.
//
//   A + DPAD    move
//   release A   commit the highlighted effect and close  (dispatcher's `on_a_released`)
//
// ─── The accordion ───────────────────────────────────────────────────────────────────────────────
//
// The effects are grouped by WHAT THEY ACT ON, and one group is expanded at a time; the rest are a
// heading each. ⭐ **THE OPEN GROUP IS THE GROUP THE CURSOR IS IN** — it is not a second piece of
// state that could disagree with the cursor, so "which one is expanded" and "where the cursor is"
// cannot drift apart. DOWN off the bottom row of a group opens the one below it, UP off the top row
// the one above, and both wrap: the whole list is one ring.
//
// ⚠️ **A CELL HOLDS AN EFFECT CODE, NOT AN INDEX INTO songcore::EFFECT_TYPES.** The reading order here
// is the groups' order, which is not that array's, and some effects deliberately appear in TWO groups
// — `AUS`/`AUF`, which ramp a neighbouring cell that can belong to either, and `KIL`, which is both a
// thing done to a note and a thing done while writing a pattern. So there is no "the cell's index" to
// speak of; what a release of A commits is the code the cell carries, and opening the picker on a
// doubled effect lands in the first group that lists it.
//
// ⚠️ **WHICH EFFECTS A BUILD SHOWS IS STILL DECIDED BY THE TAIL OF `EFFECT_TYPES`**, exactly as the FX
// column's own A+RIGHT cycle decides it — `fx_layout_for(count)` keeps a code only while its index is
// below `count` (InputDispatcher::visible_effect_type_count()). One rule for both, or a build gets a
// cell the picker cannot name. The MIDI group empties itself that way and is then dropped whole.
//
// This header is PURE — no canvas, no theme. The drawing is ui/modules/fx_helper_overlay.h. That
// split is what lets `ptinput` check the navigation without linking a renderer.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "songcore/effects.h"

namespace pt::ui {

inline constexpr int FX_GRID_COLS = 6;

// ─── The groups ──────────────────────────────────────────────────────────────────────────────────
//
// Grouped by what a command REACHES, which is the one division a reader can predict without being
// told: the sequence (the step ladder itself, before a note exists), the instrument (the note being
// played), and the whole song. MIDI is a group rather than a tail because a build without the MIDI
// surfaces drops it entire.
//
// ⚠️ **ORDER INSIDE A GROUP IS READING ORDER AND NOTHING ELSE** — it is free to change, because a
// `.ptp` stores the effect CODE and a cell here IS a code. This is the one FX list in the tree that
// may be re-ordered at will; `EFFECT_TYPES` is not (songcore/effects.h).
//
// ⚠️ **AN EFFECT IN NO GROUP IS AN EFFECT NO ONE CAN PICK**, which is why the coverage below is a
// static_assert and not a test: appending to `EFFECT_TYPES` without adding a line here fails the build.
//
// ⚠️⚠️ **THE EMPTY SLOT `---` IS IN NONE OF THESE LISTS, AND MUST NOT BE.** Every group is given one as
// its first cell by `fx_layout_for`, so clearing a command never means walking back to the top of the
// picker. Listing it as a member instead would look identical and break the build that hides MIDI:
// `---` is visible in every build, so the MIDI group would never come out empty and a release user
// would get a MIDI heading holding nothing but a blank. A group is dropped on having no REAL effects
// left, and the placeholder is added after that decision, never before it.

inline constexpr int FX_SEQUENCE_CODES[] = {
    // ⚠️ `KIL` is here AND in INSTRUMENT, the same way the ramp pair is in two groups: silencing a
    // step is how a pattern is written, so it belongs in the group a reader is already in while
    // writing one — and it is also a thing done to the note, which is the other group. It sits first
    // because that is where it is reached for, not because of what it touches.
    songcore::FX_KILL,
    songcore::FX_HOP, songcore::FX_THO, songcore::FX_TBL, songcore::FX_TIC,
    songcore::FX_GRV, songcore::FX_LAT, songcore::FX_CHA, songcore::FX_RND, songcore::FX_RNL,
    songcore::FX_ARC, songcore::FX_ARPEGGIO,
    // The TRACK scale. Its global twin is in GLOBAL, where it moves all eight tracks at once.
    songcore::FX_SCA,
};

inline constexpr int FX_INSTRUMENT_CODES[] = {
    songcore::FX_KILL, songcore::FX_OFFSET, songcore::FX_SLI, songcore::FX_BCK,
    songcore::FX_REPEAT, songcore::FX_VOLUME, songcore::FX_PAN,
    songcore::FX_PIT, songcore::FX_FIN, songcore::FX_PSL, songcore::FX_PBN,
    songcore::FX_PVB, songcore::FX_PVX, songcore::FX_TSX,
    songcore::FX_LPF, songcore::FX_HPF, songcore::FX_BPF, songcore::FX_CUT, songcore::FX_RES,
    songcore::FX_DRV, songcore::FX_CRU,
    songcore::FX_RSEND, songcore::FX_DSEND, songcore::FX_EQN,
    songcore::FX_LPO,
    // The ramp pair sits in this group AND in GLOBAL — see the header note. It is last here so that
    // hiding LPO (which is directly above it) shortens the group without reflowing the rows above.
    songcore::FX_AUS, songcore::FX_AUF,
};

inline constexpr int FX_GLOBAL_CODES[] = {
    songcore::FX_VTR, songcore::FX_VMV, songcore::FX_EQM,
    songcore::FX_SCG,   // the scale command that moves all eight tracks — SCA, the per-track one, is in SEQUENCE
    songcore::FX_AUS, songcore::FX_AUF,
};

inline constexpr int FX_MIDI_CODES[] = {
    songcore::FX_MPG, songcore::FX_MPB,
    songcore::FX_CCA, songcore::FX_CCB, songcore::FX_CCC, songcore::FX_CCD,
};

struct FxGroupDef {
    const char* title;
    const int*  codes;
    int         count;
};

// ⚠️ MIDI STAYS LAST. It is the group that can empty completely, and a heading that sits in the
// middle of the list on one build and is absent on another makes "the group below this one" mean two
// different things on two devices.
inline constexpr FxGroupDef FX_GROUP_DEFS[] = {
    {"SEQUENCE",   FX_SEQUENCE_CODES,   static_cast<int>(sizeof(FX_SEQUENCE_CODES) / sizeof(int))},
    {"INSTRUMENT", FX_INSTRUMENT_CODES, static_cast<int>(sizeof(FX_INSTRUMENT_CODES) / sizeof(int))},
    {"GLOBAL",     FX_GLOBAL_CODES,     static_cast<int>(sizeof(FX_GLOBAL_CODES) / sizeof(int))},
    {"MIDI",       FX_MIDI_CODES,       static_cast<int>(sizeof(FX_MIDI_CODES) / sizeof(int))},
};
inline constexpr int FX_GROUP_COUNT = static_cast<int>(sizeof(FX_GROUP_DEFS) / sizeof(FxGroupDef));

constexpr bool fx_code_is_grouped(int code) {
    for (const FxGroupDef& g : FX_GROUP_DEFS)
        for (int i = 0; i < g.count; ++i)
            if (g.codes[i] == code) return true;
    return false;
}

/**
 * Every effect sits in at least one group — one that sits in none is unreachable from the picker.
 * `---` is the exception and is asserted separately below: it is every group's first cell.
 */
constexpr bool fx_groups_cover_every_effect() {
    for (int i = 0; i < songcore::EFFECT_TYPE_COUNT; ++i)
        if (songcore::EFFECT_TYPES[i] != songcore::FX_NONE &&
            !fx_code_is_grouped(songcore::EFFECT_TYPES[i]))
            return false;
    return true;
}
static_assert(fx_groups_cover_every_effect(),
              "every effect must appear in an FX picker group — one that appears in none is an "
              "effect the picker cannot reach");

/**
 * …no group names a code that is not an effect, or names the same one twice, and none of them names
 * `---`. That last clause is the one with teeth: a group listing it would be a group that never
 * empties, and the build that hides the MIDI commands relies on emptying one.
 */
constexpr bool fx_groups_hold_real_effects_once() {
    for (const FxGroupDef& g : FX_GROUP_DEFS) {
        for (int i = 0; i < g.count; ++i) {
            if (g.codes[i] == songcore::FX_NONE) return false;
            bool known = false;
            for (int k = 0; k < songcore::EFFECT_TYPE_COUNT; ++k)
                if (songcore::EFFECT_TYPES[k] == g.codes[i]) known = true;
            if (!known) return false;
            for (int j = 0; j < i; ++j)
                if (g.codes[j] == g.codes[i]) return false;
        }
    }
    return true;
}
static_assert(fx_groups_hold_real_effects_once(),
              "an FX picker group holds an unknown effect code, the same one twice, or the empty "
              "slot — which every group is given as its first cell instead");

/** Rows of cells the biggest group needs, its leading `---` included. */
constexpr int fx_max_group_rows() {
    int most = 0;
    for (const FxGroupDef& g : FX_GROUP_DEFS) {
        const int rows = (g.count + 1 + FX_GRID_COLS - 1) / FX_GRID_COLS;
        if (rows > most) most = rows;
    }
    return most;
}

/** The tallest the overlay can ever be: a heading per group, plus the biggest group expanded. */
inline constexpr int FX_MAX_LAYOUT_ROWS = FX_GROUP_COUNT + fx_max_group_rows();

// The box the overlay draws is sized from a row count, so a taller layout would draw its last row
// outside the screen. The ceiling is asserted where the height and the screen are both in scope —
// `box_h` in modules/fx_helper_overlay.cpp — rather than as a number copied to here, which is how it
// came to read `<= 6` while the real geometry fits more than twice that.

// ─── One build's picker ──────────────────────────────────────────────────────────────────────────

struct FxGroup {
    const char*      title = "";
    std::vector<int> codes;   // effect CODES, in reading order

    int size() const { return static_cast<int>(codes.size()); }
    int rows() const { return (size() + FX_GRID_COLS - 1) / FX_GRID_COLS; }

    /**
     * Cells on `row`. A group's last row is usually short, and the cells it does not have are not
     * there to land on — every move that could reach one is clamped back (fx_clamp_cursor), because a
     * cursor on a missing cell would commit whatever code the arithmetic happened to name.
     */
    int row_width(int row) const {
        const int left = size() - row * FX_GRID_COLS;
        if (left <= 0) return 0;
        return left < FX_GRID_COLS ? left : FX_GRID_COLS;
    }

    int code_at(int row, int col) const {
        const int i = row * FX_GRID_COLS + col;
        return (i >= 0 && i < size()) ? codes[static_cast<size_t>(i)] : songcore::FX_NONE;
    }
};

struct FxLayout {
    std::vector<FxGroup> groups;

    int count() const { return static_cast<int>(groups.size()); }

    int max_group_rows() const {
        int most = 0;
        for (const FxGroup& g : groups) most = std::max(most, g.rows());
        return most;
    }

    /**
     * The rows the BOX is sized for: one heading per group, plus the TALLEST group's cells.
     *
     * ⚠️ The tallest, not the open one. A box measured from whichever group happens to be expanded
     * would change height as the cursor crossed a heading — and the modal is centred, so the
     * description text and the grid would jump under the reader on every crossing.
     */
    int total_rows() const { return count() + max_group_rows(); }
};

/**
 * The picker for a build that shows the first `visible_effect_count` entries of EFFECT_TYPES.
 *
 * ⚠️ The two steps are in this order for a reason. A group left with no REAL effects is dropped
 * rather than drawn as an empty heading — that is how MIDI disappears from a build without the MIDI
 * surfaces — and only then does a surviving group get its leading `---`. Prepend first and nothing
 * ever empties.
 */
inline FxLayout fx_layout_for(int visible_effect_count) {
    FxLayout out;
    for (const FxGroupDef& def : FX_GROUP_DEFS) {
        std::vector<int> visible;
        for (int i = 0; i < def.count; ++i)
            if (songcore::effect_type_index(def.codes[i]) < visible_effect_count)
                visible.push_back(def.codes[i]);
        if (visible.empty()) continue;

        FxGroup g;
        g.title = def.title;
        g.codes.push_back(songcore::FX_NONE);   // clearing a command, without leaving the group
        g.codes.insert(g.codes.end(), visible.begin(), visible.end());
        out.groups.push_back(std::move(g));
    }
    return out;
}

/** Every effect, MIDI included — the default, and what a build with the MIDI surfaces shows. */
inline const FxLayout& fx_layout_full() {
    static const FxLayout full = fx_layout_for(songcore::EFFECT_TYPE_COUNT);
    return full;
}

struct FxHelperState {
    bool     isOpen    = false;
    int      group     = 0;   // the expanded group — and the one the cursor is in
    int      cursorRow = 0;   // row within that group
    int      cursorCol = 0;
    FxLayout layout    = fx_layout_full();

    const FxGroup* open_group() const {
        if (group < 0 || group >= layout.count()) return nullptr;
        return &layout.groups[static_cast<size_t>(group)];
    }

    /** The effect CODE under the cursor — what a release of A commits. */
    int selected_effect_code() const {
        const FxGroup* g = open_group();
        return g == nullptr ? songcore::FX_NONE : g->code_at(cursorRow, cursorCol);
    }
};

/** Pull the cursor back onto a cell that exists — the row it arrives on may be a short one. */
inline void fx_clamp_cursor(FxHelperState& s) {
    const FxGroup* g = s.open_group();
    if (g == nullptr || g->rows() == 0) { s.cursorRow = 0; s.cursorCol = 0; return; }
    if (s.cursorRow < 0) s.cursorRow = 0;
    if (s.cursorRow >= g->rows()) s.cursorRow = g->rows() - 1;
    const int width = g->row_width(s.cursorRow);
    if (s.cursorCol < 0) s.cursorCol = 0;
    if (s.cursorCol >= width) s.cursorCol = width - 1;
}

/** Open with the cursor on `effect_code`, in the first group that carries it. */
inline FxHelperState fx_helper_opened_at(int effect_code, FxLayout layout) {
    FxHelperState s;
    s.layout = std::move(layout);
    s.isOpen = true;
    for (int gi = 0; gi < s.layout.count(); ++gi) {
        const FxGroup& g = s.layout.groups[static_cast<size_t>(gi)];
        for (int i = 0; i < g.size(); ++i) {
            if (g.codes[static_cast<size_t>(i)] != effect_code) continue;
            s.group     = gi;
            s.cursorRow = i / FX_GRID_COLS;
            s.cursorCol = i % FX_GRID_COLS;
            return s;
        }
    }
    // A code this build does not show — the cell was authored somewhere that does, and read off disk.
    // The FX column keeps drawing it; the picker simply has nowhere to point, so it opens at the top.
    return s;
}

inline FxHelperState fx_helper_opened_at(int effect_code) {
    return fx_helper_opened_at(effect_code, fx_layout_full());
}

// ─── Navigation ──────────────────────────────────────────────────────────────────────────────────
//
// LEFT/RIGHT cycle the cursor's own row, whose width is six except on a group's last row. UP/DOWN
// walk the rows, and stepping off either end of a group moves into the next one — which is what
// opens it, since the open group IS the cursor's.

inline void fx_move_up(FxHelperState& s) {
    if (s.open_group() == nullptr) return;
    if (s.cursorRow > 0) {
        --s.cursorRow;
    } else {
        s.group     = (s.group + s.layout.count() - 1) % s.layout.count();
        s.cursorRow = s.open_group()->rows() - 1;
    }
    fx_clamp_cursor(s);
}

inline void fx_move_down(FxHelperState& s) {
    const FxGroup* g = s.open_group();
    if (g == nullptr) return;
    if (s.cursorRow + 1 < g->rows()) {
        ++s.cursorRow;
    } else {
        s.group     = (s.group + 1) % s.layout.count();
        s.cursorRow = 0;
    }
    fx_clamp_cursor(s);
}

inline void fx_move_left(FxHelperState& s) {
    const FxGroup* g = s.open_group();
    if (g == nullptr) return;
    const int width = g->row_width(s.cursorRow);
    if (width > 0) s.cursorCol = (s.cursorCol + width - 1) % width;
}

inline void fx_move_right(FxHelperState& s) {
    const FxGroup* g = s.open_group();
    if (g == nullptr) return;
    const int width = g->row_width(s.cursorRow);
    if (width > 0) s.cursorCol = (s.cursorCol + 1) % width;
}

// ─── The documentation ───────────────────────────────────────────────────────────────────────────
//
// EFFECT_DESCRIPTIONS, indexed to match songcore::EFFECT_TYPES. 2–4 lines each:
//   [0] "SHORT: what it is"   [1] what the value (or its first nibble) does
//   [2] what the second nibble does (optional)   [3] table-specific behaviour (optional)
// Max ~31 chars a line — the overlay is 580 px at font scale 3, and a longer line runs off the box.

inline const std::vector<std::vector<std::string>>& effect_descriptions() {
    static const std::vector<std::vector<std::string>> d = {
        /* 00 --- */ {"---: No effect", "Empty FX slot"},
        /* 1 ARC */ {"ARC: Arpeggio config", "x=mode(0=UP 1=DN 2=PP 3=RND)", "y=speed in ticks"},
        /* 2 CHA */ {"CHA: Probability gate", "x=prob(0=never F=always 8=50%)", "y=target(0=note 1-3=FX slot)"},
        /* 3 LAT */ {"LAT: Latency (delay trigger)", "xx=ticks before note fires"},
        /* 4 GRV */ {"GRV: Groove assign", "xx=groove ID (00=disable)"},
        /* 5 HOP */ {"HOP: Phrase/table jump", "y=target row (FF=stop track)", "table: x=repeat count"},
        /* 6 TIC */ {"TIC: Table tick rate", "01-FB=ticks per row", "FC-FF=special modes"},
        /* 7 ARP */ {"ARP: Arpeggio", "x=+semitones 1st note", "y=+semitones 2nd note", "configure speed with ARC"},
        /* 8 KIL */ {"KIL: Kill voice", "xx=ticks of latency before stop", "00=immediate, 0C=next step"},
        /* 9 OFF */ {"OFF: Sample offset", "xx=start point (00-FF)"},
        /* 10 RND */ {"RND: Randomize FX", "randomizes previous FX column", "x=min nibble  y=max nibble"},
        /* 11 RNL */ {"RNL: Randomize left FX", "same as RND but targets", "FX column to the left"},
        /* 12 RPT */ {"RPT: Retrigger", "RX0: retrig every x ticks", "RXY(Y!=0): retrig y+vol ramp x"},
        /* 13 TBL */ {"TBL: Table override", "xx=table ID for this note"},
        /* 14 THO */ {"THO: Table hop", "xx=target row in current table"},
        /* 15 VOL */ {"VOL: Volume", "xx=volume (00=silent FF=max)"},
        /* 16 PSL */ {"PSL: Pitch slide", "xx=duration(01=fast FF=slow)", "slides pitch from previous note"},
        /* 17 PBN */ {"PBN: Pitch bend", "01-7F=bend up  80-FF=bend down", "00=stop bending"},
        /* 18 PVB */ {"PVB: Vibrato", "x=speed(0-F Hz)", "y=depth(0-F in 1/8 semitone)"},
        /* 19 PVX */ {"PVX: Extreme vibrato", "4x depth and 2x speed", "same format as PVB"},
        /* 20 PIT */ {"PIT: Pitch semitone offset", "00-7F=+0..+127 semitones", "80-FF=-128..-1 semitones", "does not affect slice index"},
        /* 21 SLI */ {"SLI: Slice index override", "xx=slice index (00-FF)", "works even when SLICE=OFF"},
        /* 22 PAN */ {"PAN: Per-note pan", "00=left 80=center FF=right", "this note only; next reverts"},
        /* 23 BCK */ {"BCK: Playback direction", "00=reverse 01=forward", "sampler; toggle live to scratch"},
        /* 24 REV */ {"REV: Per-note reverb send", "xx=send amount (00-FF)", "this note only"},
        /* 25 DEL */ {"DEL: Per-note delay send", "xx=send amount (00-FF)", "this note only"},
        /* 26 EQN */ {"EQN: Per-note EQ slot", "xx=EQ preset slot (00-7F)", "this note only"},
        /* 27 EQM */ {"EQM: Master/mixer EQ slot", "xx=EQ preset slot (00-7F)", "holds till next EQM", "resets to mixer EQ on stop"},
        /* 28 VTR */ {"VTR: Track mixer fader", "xx=level (00=silent FF=max)", "replaces the MIXER fader", "resets to the MIXER on stop"},
        /* 29 VMV */ {"VMV: Master mixer fader", "xx=level (00=silent FF=max)", "replaces the MASTER fader", "resets to the MIXER on stop"},
        /* 30 AUS */ {"AUS: Automation start", "xx=curve 00=IN 80=LIN FF=OUT", "ramps the FX slot to its left", "to the value of the next AUF"},
        /* 31 AUF */ {"AUF: Automation finish", "xx=destination value", "ends the ramp an AUS opened", "one chain, or one table"},
        /* 32 CUT */ {"CUT: Filter cutoff", "xx=cutoff (00=low FF=high)", "needs a filter: INST or LPF", "this note only"},
        /* 33 RES */ {"RES: Filter resonance", "xx=resonance (00-FF)", "needs a filter: INST or LPF", "this note only"},
        /* 34 SCA */ {"SCA: Track scale", "x=key (0=C 1=C# .. B=B)", "y=scale slot (0-F)", "holds till next SCA or stop"},
        /* 35 SCG */ {"SCG: Global scale", "x=key (0=C 1=C# .. B=B)", "y=scale slot (0-F)", "moves all 8 tracks at once"},
        /* 36 LPF */ {"LPF: Low-pass filter ON", "xx=cutoff (00=dark FF=open)", "switches the filter ON", "unlike CUT which only moves it"},
        /* 37 HPF */ {"HPF: High-pass filter ON", "xx=cutoff (00=open FF=thin)", "switches the filter ON", "unlike CUT which only moves it"},
        /* 38 BPF */ {"BPF: Band-pass filter ON", "xx=centre (00=low FF=high)", "switches the filter ON", "unlike CUT which only moves it"},
        /* 39 DRV */ {"DRV: Overdrive", "xx=amount (00=clean FF=heavy)", "this note only"},
        /* 40 CRU */ {"CRU: Bit crush + downsample", "x=bits crushed (0=off F=most)", "y=rate drop (0=off F=most)", "this note only"},
        /* 41 FIN */ {"FIN: Fine tune", "00=flat 80=in tune FF=sharp", "one semitone either way", "bends a note already playing"},
        /* 42 TSX */ {"TSX: Transpose multiplier", "xx=how far TSP moves a note", "01=normal 02=twice 00=never", "FF=the other way FE=2x that"},
        /* 43 LPO */ {"LPO: Loop window slide", "moves the whole loop, both ends", "10=one loop 01=a 16th", "F0=back a loop  adds up"},
        /* 44 MPG */ {"MPG: MIDI program change", "xx=program (00-7F)", "external instruments only"},
        /* 45 MPB */ {"MPB: MIDI pitch bend", "00=down 80=centre FF=up", "absolute - external only"},
        // ⚠️ **NO APOSTROPHE AND NO SEMICOLON IN A DESCRIPTION** — the font has neither glyph and draws
        // a BLANK, so "instrument's" renders as "INSTRUMENT S". It is silent: the string is right, the
        // width is right, only the pixels are wrong, and these lines are the only long prose in the UI.
        // ⚠️ Pre-existing, not new: BCK's "sampler; toggle live to scratch" has always drawn as
        // "SAMPLER  TOGGLE…". Caught by ptshot — the one tool here that looks at pixels. Stick to
        // letters, digits, and `: = - ( ) . /`, all of which are proven by the entries above.
        /* 46 CCA */ {"CCA: MIDI CC slot A", "xx=value (00-FF)", "moves the CC number set in", "the instrument CC A row"},
        /* 47 CCB */ {"CCB: MIDI CC slot B", "xx=value (00-FF)", "moves the CC number set in", "the instrument CC B row"},
        /* 48 CCC */ {"CCC: MIDI CC slot C", "xx=value (00-FF)", "moves the CC number set in", "the instrument CC C row"},
        /* 49 CCD */ {"CCD: MIDI CC slot D", "xx=value (00-FF)", "moves the CC number set in", "the instrument CC D row"},
    };
    return d;
}

/**
 * The lines for the highlighted effect, or a two-line placeholder when the cursor is off the table.
 *
 * The list above is indexed by EFFECT_TYPES, and a picker cell holds a CODE, so the lookup goes
 * through `effect_type_index` — which is also what keeps the two lists honest: a description added
 * out of order describes the wrong effect on screen, and nothing else would say so.
 *
 * By reference into `effect_descriptions()`'s static, because the picker redraws this every frame it
 * is up and the 2-4 description lines are past SSO. The out-of-range arm needs a static of its own to
 * have something to bind to.
 */
inline const std::vector<std::string>& fx_description_lines(const FxHelperState& s) {
    static const std::vector<std::string> kNone{"---", "No effect"};
    const auto& all = effect_descriptions();
    const int   i   = songcore::effect_type_index(s.selected_effect_code());
    if (i < 0 || i >= static_cast<int>(all.size())) return kNone;
    return all[static_cast<size_t>(i)];
}

}  // namespace pt::ui
