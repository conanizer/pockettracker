#pragma once

// ─── Filling rows of a phrase ────────────────────────────────────────────────────────────────────
//
// L + a direction on PHRASE. The range's top row is the source: repeat it every N rows, spread N copies
// of it evenly, ramp a column between the range's two ends, scatter it over random rows, or roll fresh
// values into the cells that hold one. Pure functions over a Phrase — the dispatcher keeps the phrase from before the first
// press, which is the undo.
//
// A fill adds and never erases: a blank source cell contributes nothing, and rows a fill does not land
// on are left as they were.
// Columns as on screen: 1 NOTE · 2 VOL · 3 INST · 4/5, 6/7, 8/9 the three FX command/value pairs.

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

#include "songcore/effects.h"
#include "songcore/model.h"
#include "songcore/rng.h"
#include "songcore/scales.h"

namespace pt::ui {

struct FillRange {
    int top = 0, bottom = 15;
    int firstColumn = 1, lastColumn = 1;
    // No selection: the cursor's cell travels as its whole unit (note + volume + instrument, or
    // command + value), and nothing ramps — the range ends where the phrase does, not where the user said.
    bool wholeUnit = false;

    bool has(int column) const { return column >= firstColumn && column <= lastColumn; }
    bool operator==(const FillRange& o) const {
        return top == o.top && bottom == o.bottom && firstColumn == o.firstColumn &&
               lastColumn == o.lastColumn && wholeUnit == o.wholeUnit;
    }
};

/** Both lines start here. L+RIGHT's goes on through the spacings, in rows, to the ramp; L+LEFT's
 *  through the spreads, in notes. */
inline constexpr int FILL_AS_IT_WAS = 0;
inline constexpr int FILL_RAMP      = -1;
inline constexpr int FILL_SPACINGS[] = {8, 6, 4, 3, 2, 1};
inline constexpr int FILL_SPREADS[]  = {3, 5, 6, 7, 9, 11, 13};

inline constexpr bool fill_is_fx_type(int column)  { return column >= 4 && column % 2 == 0; }
inline constexpr bool fill_is_fx_value(int column) { return column >= 5 && column % 2 == 1; }
inline constexpr int  fill_fx_slot(int column)     { return (column - 2) / 2; }

/**
 * Does this cell hold something a fill can use?
 * ⚠️ Volume and instrument are numbers with no empty state, so they count only beside a note — or an
 * empty top row would stamp its 7F / 00 over every real note in the range.
 */
inline bool fill_cell_holds(const songcore::PhraseStep& s, int column) {
    if (column == 1) return s.note != songcore::Note::EMPTY();
    if (column == 2 || column == 3) return songcore::step_has_note(s);
    return songcore::step_fx_type(s, fill_fx_slot(column)) != songcore::FX_NONE;
}

/** A cell as a number: a note's MIDI pitch, a volume, an instrument, an FX value. */
inline int fill_get(const songcore::PhraseStep& s, int column) {
    if (column == 1) return songcore::note_to_midi(s.note);
    if (column == 2) return s.volume;
    if (column == 3) return s.instrument;
    return songcore::step_fx_value(s, fill_fx_slot(column));
}
inline void fill_set(songcore::PhraseStep& s, int column, int value) {
    if (column == 1)      s.note = songcore::note_from_midi(value);
    else if (column == 2) s.volume = value;
    else if (column == 3) s.instrument = value;
    else                  songcore::step_set_fx_value(s, fill_fx_slot(column), value);
}

inline bool fill_same_steps(const songcore::Phrase& a, const songcore::Phrase& b) {
    if (a.steps.size() != b.steps.size()) return false;
    for (size_t i = 0; i < a.steps.size(); ++i) {
        const songcore::PhraseStep& x = a.steps[i];
        const songcore::PhraseStep& y = b.steps[i];
        if (x.note != y.note || x.instrument != y.instrument || x.volume != y.volume ||
            x.fx1Type != y.fx1Type || x.fx1Value != y.fx1Value || x.fx2Type != y.fx2Type ||
            x.fx2Value != y.fx2Value || x.fx3Type != y.fx3Type || x.fx3Value != y.fx3Value)
            return false;
    }
    return true;
}

inline bool fill_source_holds(const songcore::PhraseStep& src, const FillRange& r) {
    for (int column = std::max(1, r.firstColumn); column <= std::min(9, r.lastColumn); ++column)
        if (fill_cell_holds(src, column)) return true;
    return false;
}

/**
 * Lay the source row's cells onto `dst`, inside the range's columns.
 * A cell landing where its companions were blank brings them: a note onto a row with no note brings
 * volume and instrument, a command onto an empty slot brings its value, a value onto an empty slot its
 * command. ⚠️ A value never lands on a DIFFERENT command — it would mean something else there.
 */
inline void fill_place(songcore::PhraseStep& dst, const songcore::PhraseStep& src, const FillRange& r) {
    const bool srcNote = songcore::step_has_note(src);
    if (r.has(1) && src.note != songcore::Note::EMPTY()) {
        const bool companions = r.wholeUnit || !songcore::step_has_note(dst);
        dst.note = src.note;
        if (companions && srcNote) {
            dst.volume     = src.volume;
            dst.instrument = src.instrument;
        }
    }
    if (srcNote && r.has(2)) dst.volume = src.volume;
    if (srcNote && r.has(3)) dst.instrument = src.instrument;

    for (int slot = 1; slot <= 3; ++slot) {
        const int  type     = songcore::step_fx_type(src, slot);
        const bool hasType  = r.has(2 + slot * 2);
        const bool hasValue = r.has(3 + slot * 2);
        if (type == songcore::FX_NONE || (!hasType && !hasValue)) continue;

        const int was = songcore::step_fx_type(dst, slot);
        if (r.wholeUnit || (hasType && hasValue) || was == songcore::FX_NONE)
            songcore::step_set_fx(dst, slot, type, songcore::step_fx_value(src, slot));
        else if (hasType)
            songcore::step_set_fx(dst, slot, type, songcore::step_fx_value(dst, slot));
        else if (was == type)
            songcore::step_set_fx_value(dst, slot, songcore::step_fx_value(src, slot));
    }
}

/** The top row repeated on every `every`-th row below it. */
inline songcore::Phrase fill_every(const songcore::Phrase& base, const FillRange& r, int every) {
    songcore::Phrase out = base;
    const songcore::PhraseStep src = base.steps[static_cast<size_t>(r.top)];
    for (int row = r.top + every; row <= r.bottom; row += every)
        fill_place(out.steps[static_cast<size_t>(row)], src, r);
    return out;
}

/**
 * `count` hits over `rows` rows, as evenly as they go: 'x' on a hit, '.' off it. Row 0 is always a
 * hit and no two gaps differ by more than a row. A count that shares a factor with the rows is the
 * smaller pattern repeated — 6 over 16 is 3 over 8, twice.
 */
inline std::string fill_even_pattern(int count, int rows) {
    if (count >= rows) return std::string(static_cast<size_t>(std::max(rows, 0)), 'x');
    const int reps = std::gcd(count, rows);
    std::vector<std::string> head(static_cast<size_t>(count / reps), "x");
    std::vector<std::string> tail(static_cast<size_t>((rows - count) / reps), ".");
    // Pair a group from each pile, and let whichever pile has groups left over be the next tail.
    do {
        const size_t pairs = std::min(head.size(), tail.size());
        for (size_t i = 0; i < pairs; ++i) head[i] += tail[i];
        if (head.size() > pairs) {
            tail.assign(head.begin() + static_cast<std::ptrdiff_t>(pairs), head.end());
            head.resize(pairs);
        } else {
            tail.erase(tail.begin(), tail.begin() + static_cast<std::ptrdiff_t>(pairs));
        }
    } while (tail.size() > 1);

    std::string once, out;
    for (const std::string& group : head) once += group;
    for (const std::string& group : tail) once += group;
    for (int i = 0; i < reps; ++i) out += once;
    return out;
}

/** The top row on `count` rows of the range, spread as evenly as they go. */
inline songcore::Phrase fill_spread(const songcore::Phrase& base, const FillRange& r, int count) {
    songcore::Phrase out = base;
    if (count == FILL_AS_IT_WAS) return out;
    const songcore::PhraseStep src  = base.steps[static_cast<size_t>(r.top)];
    const std::string          hits = fill_even_pattern(count, r.bottom - r.top + 1);
    for (int row = r.top + 1; row <= r.bottom; ++row)
        if (hits[static_cast<size_t>(row - r.top)] == 'x')
            fill_place(out.steps[static_cast<size_t>(row)], src, r);
    return out;
}

/** Can this column run from the top row's value to the bottom row's? Both must hold one, and differ. */
inline bool fill_ramps(const songcore::Phrase& base, const FillRange& r, int column) {
    if (r.wholeUnit || r.bottom - r.top < 2 || !r.has(column) || fill_is_fx_type(column)) return false;
    const songcore::PhraseStep& a = base.steps[static_cast<size_t>(r.top)];
    const songcore::PhraseStep& b = base.steps[static_cast<size_t>(r.bottom)];
    if (column <= 3) {
        return songcore::step_has_note(a) && songcore::step_has_note(b) &&
               fill_get(a, column) != fill_get(b, column);
    }
    const int slot = fill_fx_slot(column);
    return songcore::step_fx_type(a, slot) != songcore::FX_NONE &&
           songcore::step_fx_type(a, slot) == songcore::step_fx_type(b, slot) &&
           fill_get(a, column) != fill_get(b, column);
}

/** Every row between the two ends, stepped evenly from one end's value to the other's. */
inline songcore::Phrase fill_ramp(const songcore::Phrase& base, const FillRange& r,
                                  const songcore::Project& p) {
    songcore::Phrase out = base;
    const songcore::PhraseStep& a = base.steps[static_cast<size_t>(r.top)];
    const songcore::PhraseStep& b = base.steps[static_cast<size_t>(r.bottom)];
    const int span = r.bottom - r.top;

    for (int column = 1; column <= 9; ++column) {
        if (!fill_ramps(base, r, column)) continue;
        const int from = fill_get(a, column), to = fill_get(b, column);
        for (int row = r.top + 1; row < r.bottom; ++row) {
            songcore::PhraseStep& dst = out.steps[static_cast<size_t>(row)];
            const int value = from + static_cast<int>(std::lround(
                                         static_cast<double>(to - from) * (row - r.top) / span));
            if (column == 1) {
                if (!songcore::step_has_note(dst)) {
                    dst.volume     = a.volume;
                    dst.instrument = a.instrument;
                }
                fill_set(dst, 1, songcore::phrase_note_scale(p, base, row, dst.instrument).snap(value));
            } else if (column <= 3) {
                if (songcore::step_has_note(dst)) fill_set(dst, column, value);
            } else {
                const int slot = fill_fx_slot(column);
                const int type = songcore::step_fx_type(a, slot);
                const int was  = songcore::step_fx_type(dst, slot);
                if (was == songcore::FX_NONE || was == type) songcore::step_set_fx(dst, slot, type, value);
            }
        }
    }
    return out;
}

/** The line L+RIGHT walks for this range. A spacing that reaches no second row is left off. */
inline std::vector<int> fill_stops(const songcore::Phrase& base, const FillRange& r) {
    std::vector<int> stops{FILL_AS_IT_WAS};
    if (fill_source_holds(base.steps[static_cast<size_t>(r.top)], r))
        for (const int every : FILL_SPACINGS)
            if (r.top + every <= r.bottom) stops.push_back(every);
    for (int column = 1; column <= 9; ++column) {
        if (!fill_ramps(base, r, column)) continue;
        stops.push_back(FILL_RAMP);
        break;
    }
    return stops;
}

inline songcore::Phrase fill_at_stop(const songcore::Phrase& base, const FillRange& r, int stop,
                                     const songcore::Project& p) {
    if (stop == FILL_AS_IT_WAS) return base;
    return stop == FILL_RAMP ? fill_ramp(base, r, p) : fill_every(base, r, stop);
}

/**
 * The line L+LEFT walks for this range: so many notes, spread evenly. A count is left off when it
 * does not leave a row free, or when it lands on the very rows a spacing of the other line does.
 */
inline std::vector<int> fill_spread_stops(const songcore::Phrase& base, const FillRange& r) {
    std::vector<int> stops{FILL_AS_IT_WAS};
    if (!fill_source_holds(base.steps[static_cast<size_t>(r.top)], r)) return stops;

    const int rows = r.bottom - r.top + 1;
    for (const int count : FILL_SPREADS) {
        if (count >= rows) break;
        const std::string hits = fill_even_pattern(count, rows);
        bool spacing = false;
        for (const int every : FILL_SPACINGS) {
            bool same = true;
            for (int row = 0; row < rows && same; ++row)
                same = (hits[static_cast<size_t>(row)] == 'x') == (row % every == 0);
            spacing = spacing || same;
        }
        if (!spacing) stops.push_back(count);
    }
    return stops;
}

/** The top row on a random third of the rows below it — at least one. */
inline songcore::Phrase fill_scatter(const songcore::Phrase& base, const FillRange& r,
                                     songcore::Rng& rng) {
    songcore::Phrase out = base;
    const songcore::PhraseStep src = base.steps[static_cast<size_t>(r.top)];
    if (r.bottom <= r.top || !fill_source_holds(src, r)) return out;

    bool any = false;
    for (int row = r.top + 1; row <= r.bottom; ++row) {
        if (rng.next_int(3) != 0) continue;
        fill_place(out.steps[static_cast<size_t>(row)], src, r);
        any = true;
    }
    if (!any) fill_place(out.steps[static_cast<size_t>(rng.next_int(r.top + 1, r.bottom + 1))], src, r);
    return out;
}

/**
 * A fresh random value in every cell of the range that already holds one — a command is never rolled,
 * only its value. Each column draws between the lowest and the highest value it holds in the range;
 * where they are all the same, a note gets the octave above it, a volume or an FX value its whole
 * range, and an instrument is left alone. A note lands in its row's scale.
 */
inline songcore::Phrase fill_randomize(const songcore::Phrase& placed, FillRange r,
                                       const songcore::Project& p, songcore::Rng& rng) {
    songcore::Phrase out = placed;
    // With no selection the cursor on a command means its value.
    if (r.wholeUnit && fill_is_fx_type(r.firstColumn)) r.firstColumn = r.lastColumn = r.firstColumn + 1;

    for (int column = std::max(1, r.firstColumn); column <= std::min(9, r.lastColumn); ++column) {
        if (fill_is_fx_type(column)) continue;
        // A note OFF holds no pitch to roll.
        const auto holds = [&](const songcore::PhraseStep& s) {
            return column == 1 ? songcore::step_has_note(s) : fill_cell_holds(s, column);
        };

        int lo = 0, hi = -1;
        for (int row = r.top; row <= r.bottom; ++row) {
            const songcore::PhraseStep& s = placed.steps[static_cast<size_t>(row)];
            if (!holds(s)) continue;
            const int v = fill_get(s, column);
            if (hi < lo) lo = hi = v;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        if (hi < lo) continue;                    // the column holds nothing here
        const bool flat = (lo == hi);
        if (flat && column == 3) continue;
        if (flat && column == 1) hi = std::min(127, lo + 12);
        if (flat && column == 2) { lo = 0; hi = 127; }

        for (int row = r.top; row <= r.bottom; ++row) {
            songcore::PhraseStep& s = out.steps[static_cast<size_t>(row)];
            if (!holds(s)) continue;
            if (column == 1) {
                const songcore::NoteScale scale = songcore::phrase_note_scale(p, placed, row, s.instrument);
                std::vector<int> notes;
                for (int midi = lo; midi <= hi; ++midi)
                    if (scale.contains(midi)) notes.push_back(midi);
                if (!notes.empty())
                    fill_set(s, 1, notes[static_cast<size_t>(rng.next_int(static_cast<int>(notes.size())))]);
            } else if (fill_is_fx_value(column)) {
                const int max = songcore::effect_value_max(songcore::step_fx_type(s, fill_fx_slot(column)));
                fill_set(s, column, flat ? rng.next_int(0, max + 1)
                                         : std::min(max, rng.next_int(lo, hi + 1)));
            } else {
                fill_set(s, column, rng.next_int(lo, hi + 1));
            }
        }
    }
    return out;
}

}  // namespace pt::ui
