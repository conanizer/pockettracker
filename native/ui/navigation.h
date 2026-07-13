#pragma once

// ─── Screen navigation ───────────────────────────────────────────────────────────────────────────
//
// A 1:1 port of the screen-grid half of core/logic/TrackerController.kt: `getScreenColumn`,
// `getMainScreenForColumn`, and the four `navigate*` functions R+DPAD drives.
//
// It is the SAME 5×5 grid that `modules/navigation_map.h` draws, and that is the whole reason these
// two land in the same session. The map is the picture; this is the movement. A drift between them —
// a cell you can see but cannot reach, or a screen you land on that the map draws nowhere — is the
// only interesting bug in either, and keeping them side by side is what makes it obvious.
//
//        col 0     col 1     col 2     col 3       col 4
//  row 0                     SCALE     INST.POOL              ← column-specific
//  row 1  PROJECT  PROJECT   GROOVE    MODS        PROJECT    ← column-specific
//  row 2  SONG     CHAIN     PHRASE    INSTRUMENT  TABLE      ← the main row, always visible
//  row 3  MIXER    MIXER     MIXER     MIXER       MIXER      ← shared
//  row 4  EFFECTS  EFFECTS   EFFECTS   EFFECTS     EFFECTS    ← shared
//
// PROJECT / MIXER / EFFECTS are shared: they sit in every column and therefore have no column of
// their own. `previousColumn` is what remembers which one you entered from, so that going back UP out
// of MIXER returns you to the main-row screen you came from rather than to a fixed default. That is
// why every function here takes it, and why they return the new column alongside the new screen — the
// pair travels together or the memory desyncs.
//
// The four `navigate_*` functions are PURE: a `NavState` in, a `NavResult` out, no canvas and no
// engine. `go_to_screen` at the bottom is the one that APPLIES the answer, and it carries the whole
// rest of the transaction — which is not optional bookkeeping, see its comment.

#include "app_state.h"
#include "cursor_move.h"
#include "screen.h"

namespace pt::ui {

/** Where R+DPAD lands, and the column memory that must be stored with it. */
struct NavResult {
    ScreenType screen = ScreenType::PHRASE;
    int        column = 2;
    /**
     * `TrackerController.instrumentFromPool` — set by R+RIGHT out of the pool, so a later R+LEFT
     * returns to the pool instead of falling through to PHRASE. The Kotlin `currentScreen` setter
     * clears it on any move OFF the instrument screen; `apply_navigation` below does the same.
     */
    bool instrumentFromPool = false;
};

/** The inputs the four navigate functions read. */
struct NavState {
    ScreenType currentScreen     = ScreenType::PHRASE;
    int        previousColumn    = 2;
    bool       instrumentFromPool = false;
};

/** The column a screen owns, or −1 for the shared screens (PROJECT / MIXER / EFFECTS) that own none. */
inline int screen_column(ScreenType s) {
    switch (s) {
        case ScreenType::SONG:  return 0;
        case ScreenType::CHAIN: return 1;
        case ScreenType::PHRASE:
        case ScreenType::GROOVE:
        case ScreenType::SCALE: return 2;
        case ScreenType::INSTRUMENT:
        case ScreenType::MODS:
        case ScreenType::INST_POOL: return 3;
        case ScreenType::TABLE: return 4;
        default: return -1;  // shared / popup — the caller substitutes previousColumn
    }
}

/** The main-row (row 2) screen of a column. */
inline ScreenType main_screen_for_column(int column) {
    switch (column) {
        case 0:  return ScreenType::SONG;
        case 1:  return ScreenType::CHAIN;
        case 2:  return ScreenType::PHRASE;
        case 3:  return ScreenType::INSTRUMENT;
        case 4:  return ScreenType::TABLE;
        default: return ScreenType::PHRASE;
    }
}

inline bool is_main_row(ScreenType s) {
    for (ScreenType m : MAIN_ROW_SCREENS)
        if (m == s) return true;
    return false;
}

namespace detail {
/** The column to reason from: the screen's own, or the remembered one if it has none. */
inline int context_column(const NavState& s) {
    const int c = screen_column(s.currentScreen);
    return (c == -1) ? s.previousColumn : c;
}
}  // namespace detail

inline NavResult navigate_up(const NavState& s) {
    const int col = detail::context_column(s);

    // Row-0 instrument (entered from the pool): nothing above it — stay.
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::INSTRUMENT, 3, true};

    switch (s.currentScreen) {
        case ScreenType::EFFECTS: return {ScreenType::MIXER, col};                    // row 4 → 3
        case ScreenType::MIXER:   return {main_screen_for_column(col), col};          // row 3 → 2

        case ScreenType::SONG:                                                        // row 2 → 1
        case ScreenType::CHAIN:
        case ScreenType::TABLE:      return {ScreenType::PROJECT, col};
        case ScreenType::PHRASE:     return {ScreenType::GROOVE, 2};
        case ScreenType::INSTRUMENT: return {ScreenType::MODS, 3};

        case ScreenType::PROJECT: return {ScreenType::PROJECT, col};                  // row 1 → 0
        case ScreenType::GROOVE:  return {ScreenType::SCALE, 2};
        case ScreenType::MODS:    return {ScreenType::INST_POOL, 3};

        default: return {s.currentScreen, col};  // row 0 (SCALE / INST_POOL) and the popups: stay
    }
}

inline NavResult navigate_down(const NavState& s) {
    const int col = detail::context_column(s);

    // Row-0 instrument (from the pool) drops to MODS, like the pool to its left.
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::MODS, 3};

    switch (s.currentScreen) {
        case ScreenType::SCALE:     return {ScreenType::GROOVE, 2};                   // row 0 → 1
        case ScreenType::INST_POOL: return {ScreenType::MODS, 3};

        case ScreenType::GROOVE:  return {ScreenType::PHRASE, 2};                     // row 1 → 2
        case ScreenType::MODS:    return {ScreenType::INSTRUMENT, 3};
        case ScreenType::PROJECT: return {main_screen_for_column(col), col};

        case ScreenType::SONG:                                                        // row 2 → 3
        case ScreenType::CHAIN:
        case ScreenType::PHRASE:
        case ScreenType::INSTRUMENT:
        case ScreenType::TABLE: return {ScreenType::MIXER, col};

        case ScreenType::MIXER:   return {ScreenType::EFFECTS, col};                  // row 3 → 4
        case ScreenType::EFFECTS: return {ScreenType::EFFECTS, col};                  // row 4: stay

        default: return {s.currentScreen, col};
    }
}

inline NavResult navigate_left(const NavState& s) {
    // EFFECTS has no side doors.
    if (s.currentScreen == ScreenType::EFFECTS) return {s.currentScreen, s.previousColumn};

    // The instrument-pool fast-jump pair, R+LEFT half: out of the pool exits left to PHRASE, and out
    // of an INSTRUMENT that was ENTERED from the pool returns to it. (A normally-entered INSTRUMENT
    // still goes to PHRASE — which is exactly what instrumentFromPool is for.)
    if (s.currentScreen == ScreenType::INST_POOL) return {ScreenType::PHRASE, 2};
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::INST_POOL, 3, true};

    // Row 1 (PROJECT / GROOVE / MODS) and row 3 (MIXER) exit sideways onto the MAIN row, one column
    // over — you do not walk along row 1, you drop back to the tracker and then move.
    int contextCol = -1;
    switch (s.currentScreen) {
        case ScreenType::PROJECT: contextCol = s.previousColumn; break;
        case ScreenType::GROOVE:  contextCol = 2; break;
        case ScreenType::MODS:    contextCol = 3; break;
        case ScreenType::MIXER:   contextCol = 2; break;
        default: break;
    }
    if (contextCol >= 0) {
        const int target = contextCol - 1 < 0 ? 0 : contextCol - 1;
        return {main_screen_for_column(target), target};
    }

    // Any other non-main-row screen (SCALE): drop to the main row of its own column.
    if (!is_main_row(s.currentScreen)) {
        const int c = screen_column(s.currentScreen);
        return {main_screen_for_column(c), c};
    }

    switch (s.currentScreen) {  // along the main row: S C P I T
        case ScreenType::TABLE:      return {ScreenType::INSTRUMENT, 3};
        case ScreenType::INSTRUMENT: return {ScreenType::PHRASE, 2};
        case ScreenType::PHRASE:     return {ScreenType::CHAIN, 1};
        case ScreenType::CHAIN:      return {ScreenType::SONG, 0};
        case ScreenType::SONG:       return {ScreenType::SONG, 0};  // leftmost: stay
        default: return {s.currentScreen, s.previousColumn};
    }
}

inline NavResult navigate_right(const NavState& s) {
    if (s.currentScreen == ScreenType::EFFECTS) return {s.currentScreen, s.previousColumn};

    // R+RIGHT out of the pool jumps to INSTRUMENT and MARKS it, so R+LEFT comes back to the pool.
    if (s.currentScreen == ScreenType::INST_POOL) return {ScreenType::INSTRUMENT, 3, true};
    // …and that row-0 instrument has nothing to its right — stay, rather than fall through to TABLE.
    if (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool)
        return {ScreenType::INSTRUMENT, 3, true};

    int contextCol = -1;
    switch (s.currentScreen) {
        case ScreenType::PROJECT: contextCol = s.previousColumn; break;
        case ScreenType::GROOVE:  contextCol = 2; break;
        case ScreenType::MODS:    contextCol = 3; break;
        case ScreenType::MIXER:   contextCol = 2; break;
        default: break;
    }
    if (contextCol >= 0) {
        const int target = contextCol + 1 > 4 ? 4 : contextCol + 1;
        return {main_screen_for_column(target), target};
    }

    if (!is_main_row(s.currentScreen)) {
        const int c = screen_column(s.currentScreen);
        return {main_screen_for_column(c), c};
    }

    switch (s.currentScreen) {
        case ScreenType::SONG:       return {ScreenType::CHAIN, 1};
        case ScreenType::CHAIN:      return {ScreenType::PHRASE, 2};
        case ScreenType::PHRASE:     return {ScreenType::INSTRUMENT, 3};
        case ScreenType::INSTRUMENT: return {ScreenType::TABLE, 4};
        case ScreenType::TABLE:      return {ScreenType::TABLE, 4};  // rightmost: stay
        default: return {s.currentScreen, s.previousColumn};
    }
}

// ─── Applying it ─────────────────────────────────────────────────────────────────────────────────

/** The NavState the four functions above want, read off the live AppState. */
inline NavState nav_state_of(const AppState& s) {
    return NavState{s.currentScreen, s.previousColumn, s.instrumentFromPool};
}

/**
 * Land on a screen. Everything Kotlin's `TrackerController.currentScreen` SETTER does, plus the
 * cursor save/restore its callers do around it — and none of it is bookkeeping you can skip:
 *
 *   • THE CURSOR MUST BE SAVED AND RESTORED. SONG, CHAIN and PHRASE share one `cursorColumn` but have
 *     8, 2 and 9 columns. Leave PHRASE on column 9, arrive on CHAIN, and the cursor is outside every
 *     cell — it does not clamp, it DISAPPEARS (no cell matches, so nothing draws highlighted). The
 *     per-screen slots are what make the shared cursor safe.
 *   • TABLE FOLLOWS THE INSTRUMENT. Arriving on TABLE syncs `currentTable` to `currentInstrument`,
 *     because a table is an instrument's automation and showing table 3 while instrument 7 is selected
 *     would be showing you someone else's.
 *   • THE POOL FLAG IS STICKY ONLY ON INSTRUMENT. Any move to a screen that is not INSTRUMENT clears
 *     it, so a stale flag cannot silently reroute a later R+LEFT back to the pool.
 */
inline void go_to_screen(AppState& s, const NavResult& r) {
    // Save where we were leaving from (REMEMBER mode reads these back).
    switch (s.currentScreen) {
        case ScreenType::SONG:
            s.songCursorRow = s.cursorRow;   s.songCursorColumn = s.cursorColumn;   break;
        case ScreenType::CHAIN:
            s.chainCursorRow = s.cursorRow;  s.chainCursorColumn = s.cursorColumn;  break;
        case ScreenType::PHRASE:
            s.phraseCursorRow = s.cursorRow; s.phraseCursorColumn = s.cursorColumn; break;
        default: break;
    }

    s.currentScreen  = r.screen;
    s.previousColumn = r.column;

    s.instrumentFromPool = (r.screen == ScreenType::INSTRUMENT) ? r.instrumentFromPool : false;

    if (r.screen == ScreenType::TABLE) s.currentTable = s.currentInstrument;

    // Restore — or refresh, which is the Android default.
    if (s.cursorRemember) {
        switch (r.screen) {
            case ScreenType::SONG:
                s.cursorRow = s.songCursorRow;   s.cursorColumn = s.songCursorColumn;   break;
            case ScreenType::CHAIN:
                s.cursorRow = s.chainCursorRow;  s.cursorColumn = s.chainCursorColumn;  break;
            case ScreenType::PHRASE:
                s.cursorRow = s.phraseCursorRow; s.cursorColumn = s.phraseCursorColumn; break;
            // TABLE / GROOVE / the rest own their cursors outright — they persist by construction.
            default: break;
        }
    } else {
        // REFRESH — every screen that owns a cursor resets it to its top-left editable cell on entry.
        //
        // ⚠️ INSTRUMENT, MODS and INST.POOL were MISSING here until S5 (their cursors persisted across
        // an entry, where Android's refresh them). Harmless-looking, but INSTRUMENT is the one screen
        // whose ROW MAP changes shape under it — a SoundFont has a different row list from a sampler —
        // so a stale row survives onto a map that may not have one, and the cursor silently draws
        // nowhere. `instrument_row_kind` is bounds-safe (out of range reads as SINGLE, as Kotlin's
        // getOrElse does), so it was never a crash; it was a cursor you could lose.
        //
        // EFFECTS is deliberately absent: Kotlin does not reset it either, so its row persists in BOTH
        // modes.
        switch (r.screen) {
            case ScreenType::SONG:
            case ScreenType::CHAIN:
            case ScreenType::PHRASE:
                s.cursorRow    = 0;
                s.cursorColumn = min_cursor_column(r.screen);  // 1 — never the read-only gutter
                break;
            case ScreenType::TABLE:
                s.tableCursorRow = 0; s.tableCursorColumn = 1;
                break;
            case ScreenType::GROOVE:
                s.grooveCursorRow = 0;
                break;
            case ScreenType::INSTRUMENT:
                s.instrumentCursorRow = 0; s.instrumentCursorColumn = 1;
                break;
            case ScreenType::MODS:
                s.modCursorRow = 0; s.modCursorPair = 0; s.modCursorSide = 0;
                break;
            case ScreenType::INST_POOL:
                s.poolCursorColumn = 0;   // …but NOT currentInstrument: that IS the pool's row
                break;
            case ScreenType::MIXER:
                s.mixerCursorColumn = 0; s.mixerMasterRow = 0;
                break;
            default: break;
        }
    }

    // SONG's viewport must contain its cursor, whichever branch above set it.
    if (r.screen == ScreenType::SONG) scroll_song_to_row(s, s.cursorRow);
}

}  // namespace pt::ui
