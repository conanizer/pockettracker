#include "ui/modules/navigation_map.h"

#include "ui/helpers.h"
#include "ui/navigation.h"

namespace pt::ui {

void NavigationMapModule::draw(Canvas& c, int x, int y, const NavigationMapState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // Which column are we in? A shared screen has none of its own, so it uses the one we came from.
    const int screenCol  = screen_column(s.currentScreen);
    const int currentCol = (screenCol == -1) ? s.sourceColumn : screenCol;

    // The always-visible main row, plus this column's own screens.
    const int col = (currentCol < 0 || currentCol >= NAV_COLUMNS) ? 2 : currentCol;
    std::optional<ScreenType> grid[NAV_ROWS][NAV_COLUMNS] = {};
    for (int gcol = 0; gcol < NAV_COLUMNS; ++gcol)
        grid[NAV_MAIN_ROW][gcol] = SCREEN_GRID[NAV_MAIN_ROW][gcol];
    for (int row = 0; row < NAV_ROWS; ++row) grid[row][col] = SCREEN_GRID[row][col];

    // The pool's fast-jump INSTRUMENT cell at row 0 / col 4, right of the pool. On an INSTRUMENT reached
    // from the pool, THAT cell is the current position (same ScreenType, so position disambiguates).
    const bool onPoolInstrument = (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool);
    if (s.currentScreen == ScreenType::INST_POOL || onPoolInstrument)
        grid[0][4] = ScreenType::INSTRUMENT;

    for (int row = 0; row < NAV_ROWS; ++row) {
        for (int gcol = 0; gcol < NAV_COLUMNS; ++gcol) {
            if (!grid[row][gcol]) continue;  // empty cells are just background

            const ScreenType screen = *grid[row][gcol];
            const int        cellX  = x + (gcol * CELL_WIDTH);
            const int        cellY  = y + (row * CELL_HEIGHT);

            const bool isCurrent = onPoolInstrument ? (row == 0 && gcol == 4)
                                                    : (screen == s.currentScreen);

            const std::string label  = screen_short_label(screen);
            const int         labelW = Canvas::text_width(label, CHAR_SPACING, FONT_SCALE);

            c.draw_text(label, cellX + (CELL_WIDTH - labelW) / 2, cellY + 3,
                        isCurrent ? cursor_mark_ink(t) : t.textValue, CHAR_SPACING, FONT_SCALE);
        }
    }
}

}  // namespace pt::ui
