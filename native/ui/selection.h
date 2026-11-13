#pragma once

// A selection: L+B taps grow it CELL → ROW → SCREEN, the D-pad drags its edge. Column 0 (the gutter)
// is never covered.

#include <algorithm>
#include <string>

namespace pt::ui {

enum class SelectionScope { NONE, CELL, ROW, SCREEN };

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

// `active` gates every read.
struct Selection {
    SelectionScope scope  = SelectionScope::NONE;
    bool           active = false;
    CursorPosition start{};
    CursorPosition end{};

    // L+B: select the cell; every L+B after that widens it, CELL → ROW → SCREEN and round to CELL.
    // It never leaves the selection — B, L+A, A+B and L+R do.
    void handle_select_b(int cursorRow, int cursorColumn, int maxColumn, int maxRow) {
        switch (scope) {
            case SelectionScope::CELL: scope = SelectionScope::ROW;    break;
            case SelectionScope::ROW:  scope = SelectionScope::SCREEN; break;
            default:                   scope = SelectionScope::CELL;   break;   // from none, and round
        }
        init_for_scope(cursorRow, cursorColumn, maxColumn, maxRow);
    }

    // Drag the active edge, clamped; the anchor never moves.
    void expand(NavDir direction, int maxRow, int maxColumn) {
        if (!active) return;
        switch (direction) {
            case NavDir::UP:    end.row    = std::max(0, end.row - 1);            break;
            case NavDir::DOWN:  end.row    = std::min(maxRow, end.row + 1);       break;
            case NavDir::LEFT:  end.column = std::max(1, end.column - 1);         break;
            case NavDir::RIGHT: end.column = std::min(maxColumn, end.column + 1); break;
        }
    }

    // Normalised: the edge may be above or left of the anchor.
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

    // The top-strip readout; "" with no selection.
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
