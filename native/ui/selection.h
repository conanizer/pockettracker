#pragma once

// ─── The selection machinery ─────────────────────────────────────────────────────────────────────
//
// What a RANGE of cells needs (ui/cursor.h is the single cell): the L+B multi-tap that grows a
// selection CELL → ROW → SCREEN, the D-pad that drags its edge, and "is this cell selected?".
// ⚠️ It never reads the clock: `now_ms` is a parameter, so a test can drive the 500 ms window with a
// fake clock.
// Columns are 1-based from 1: column 0 is the read-only step-number gutter, which neither the cursor
// nor a selection may cover.

#include <algorithm>
#include <string>

namespace pt::ui {

/** How much the selection covers. Grows CELL → ROW → SCREEN on each tap inside the window. */
enum class SelectionScope { NONE, CELL, ROW, SCREEN };

/** A D-pad direction. Named so the hottest input path compares an int, not four strings. */
enum class NavDir { UP, DOWN, LEFT, RIGHT };

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

/** The selection state. `active` gates every read, so the two positions are plain values. */
struct Selection {
    SelectionScope scope  = SelectionScope::NONE;
    bool           active = false;
    CursorPosition start{};
    CursorPosition end{};

    /**
     * L+B. The first tap selects the CELL; each tap inside the 500 ms window widens it (CELL → ROW →
     * SCREEN → CELL); a tap after the window exits.
     * ⚠️ `lastTapMs` is updated on EVERY path, exit included, so a slow double-tap re-opens on CELL
     * rather than jumping to ROW.
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
    void expand(NavDir direction, int maxRow, int maxColumn) {
        if (!active) return;
        switch (direction) {
            case NavDir::UP:    end.row    = std::max(0, end.row - 1);            break;
            case NavDir::DOWN:  end.row    = std::min(maxRow, end.row + 1);       break;
            case NavDir::LEFT:  end.column = std::max(1, end.column - 1);         break;
            case NavDir::RIGHT: end.column = std::min(maxColumn, end.column + 1); break;
        }
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
                // The WHOLE screen — on SONG all 256 rows, hence `maxRow` as a parameter.
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
