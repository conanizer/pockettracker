#pragma once

// ─── The selection machinery ─────────────────────────────────────────────────────────────────────
//
// The half of core/logic/InputController.kt that S1 left behind. `ui/cursor.h` ported the part a
// SINGLE cell needs — what is under the cursor, and what the five buttons do to it. This is the part
// a RANGE of cells needs: the L+B multi-tap that grows a selection from one cell to a row to the
// whole screen, the D-pad that drags its edge, and the question every grid editor already asks while
// drawing ("is this cell selected?").
//
// Two things are worth stating, because both are load-bearing.
//
// ⚠️ **It never reads the clock.** Kotlin's `handleSelectB` calls `System.currentTimeMillis()` inside
// itself, which is fine when a human is pressing the button and fatal when a test is. `now_ms` is a
// PARAMETER here, exactly as `SdlInput::handle_event` takes one (S1 changed it for the same reason):
// a function whose behaviour is a function of time cannot be tested if it reaches for the time
// itself. `ptinput` drives the multi-tap window with a fake clock and asserts the exact 500 ms edge.
//
// **Columns are 1-based and start at 1, not 0.** Every bound below (`ROW` spanning `1..maxColumn`,
// `expand`'s LEFT clamp) says 1, because column 0 is the read-only step-number gutter — the cursor
// cannot reach it (ui/cursor_move.h) and a selection must not cover it.

#include <algorithm>
#include <string>

namespace pt::ui {

/** How much the selection covers. Grows CELL → ROW → SCREEN on each tap inside the window. */
enum class SelectionScope { NONE, CELL, ROW, SCREEN };

struct CursorPosition {
    int row    = 0;
    int column = 0;
    bool operator==(const CursorPosition& o) const { return row == o.row && column == o.column; }
    bool operator!=(const CursorPosition& o) const { return !(*this == o); }
};

struct SelectionBounds {
    int topLeftRow     = 0;
    int topLeftColumn  = 0;
    int bottomRightRow = 0;
    int bottomRightColumn = 0;

    int width()  const { return bottomRightColumn - topLeftColumn + 1; }
    int height() const { return bottomRightRow - topLeftRow + 1; }

    bool contains(int row, int column) const {
        return row >= topLeftRow && row <= bottomRightRow && column >= topLeftColumn &&
               column <= bottomRightColumn;
    }
};

/** The tap window, in ms. Two L+B presses closer than this cycle the scope; further apart, exit. */
inline constexpr long long MULTI_TAP_WINDOW = 500;

/**
 * InputController's selection state. Kotlin keeps `selectionStart`/`selectionEnd` as nullable
 * CursorPositions and gates every read on `selectionMode`; `active` is that same gate, so the two
 * positions can stay plain values rather than optionals.
 */
struct Selection {
    SelectionScope scope  = SelectionScope::NONE;
    bool           active = false;   // Kotlin's `selectionMode`
    CursorPosition start{};
    CursorPosition end{};

    /**
     * L+B. First tap selects the CELL; each further tap inside the 500 ms window widens the scope
     * (CELL → ROW → SCREEN → CELL); a tap after the window has closed exits selection entirely.
     *
     * ⚠️ `lastTapMs` is updated on EVERY path, the exit path included — that is Kotlin's behaviour and
     * it matters: it means the tap that closed a selection also starts the clock for the next one, so
     * a slow double-tap re-opens on CELL rather than skipping straight to ROW.
     */
    void handle_select_b(long long now_ms, int cursorRow, int cursorColumn, int maxColumn,
                         int maxRow = 15) {
        if (scope == SelectionScope::NONE) {
            scope = SelectionScope::CELL;
            init_for_scope(cursorRow, cursorColumn, maxColumn, maxRow);
        } else if (now_ms - lastTapMs < MULTI_TAP_WINDOW) {
            switch (scope) {
                case SelectionScope::CELL:   scope = SelectionScope::ROW;    break;
                case SelectionScope::ROW:    scope = SelectionScope::SCREEN; break;
                case SelectionScope::SCREEN: scope = SelectionScope::CELL;   break;
                default:                     scope = SelectionScope::CELL;   break;
            }
            init_for_scope(cursorRow, cursorColumn, maxColumn, maxRow);
        } else {
            exit();
        }
        lastTapMs = now_ms;
    }

    /** D-pad while a selection is up: drag its active edge, clamped. The anchor never moves. */
    void expand(const char* direction, int maxRow, int maxColumn) {
        if (!active) return;
        const std::string d(direction);
        if (d == "UP")         end.row    = std::max(0, end.row - 1);
        else if (d == "DOWN")  end.row    = std::min(maxRow, end.row + 1);
        else if (d == "LEFT")  end.column = std::max(1, end.column - 1);
        else if (d == "RIGHT") end.column = std::min(maxColumn, end.column + 1);
    }

    /** The rectangle, normalised — the edge may be above/left of the anchor. */
    SelectionBounds bounds() const {
        return SelectionBounds{std::min(start.row, end.row), std::min(start.column, end.column),
                               std::max(start.row, end.row), std::max(start.column, end.column)};
    }

    bool is_cell_selected(int row, int column) const {
        return active && bounds().contains(row, column);
    }

    void exit() {
        active = false;
        scope  = SelectionScope::NONE;
        start  = CursorPosition{};
        end    = CursorPosition{};
    }

    /** The top-strip readout. "" when there is no selection. */
    std::string info() const {
        if (!active) return "";
        switch (scope) {
            case SelectionScope::CELL:   return "SEL:CELL";
            case SelectionScope::ROW:    return "SEL:ROW";
            case SelectionScope::SCREEN: return "SEL:ALL";
            default:                     return "";
        }
    }

  private:
    long long lastTapMs = 0;

    void init_for_scope(int cursorRow, int cursorColumn, int maxColumn, int maxRow) {
        switch (scope) {
            case SelectionScope::CELL:
                start = CursorPosition{cursorRow, cursorColumn};
                end   = CursorPosition{cursorRow, cursorColumn};
                break;
            case SelectionScope::ROW:
                start = CursorPosition{cursorRow, 1};
                end   = CursorPosition{cursorRow, maxColumn};
                break;
            case SelectionScope::SCREEN:
                // The WHOLE screen, which on SONG means all 256 rows and not merely the 16 on
                // display — hence `maxRow` being a parameter rather than a constant 15.
                start = CursorPosition{0, 1};
                end   = CursorPosition{maxRow, maxColumn};
                break;
            default:
                break;
        }
        active = true;
    }
};

}  // namespace pt::ui
