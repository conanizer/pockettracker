#include "ui/modules/navigation_map.h"

#include "ui/helpers.h"
#include "ui/navigation.h"

namespace pt::ui {

namespace {

/**
 * The 5×5 template: what each COLUMN holds, top to bottom. `nullopt` (here: the NONE sentinel) is an
 * empty cell. Rows 3 and 4 are MIXER/EFFECTS in every column because they are shared; row 2 is the
 * main row and is filled in separately, since it is drawn whatever column you are in.
 *
 *   COLUMN_LAYOUTS in the Kotlin, verbatim — only the phrase and instrument columns have five screens.
 */
constexpr int EMPTY_CELL = -1;

int column_layout(int col, int row) {
    // -1 = empty; otherwise a ScreenType cast to int.
    static const int L[5][5] = {
        // row 0            row 1                             row 2                             row 3                          row 4
        {EMPTY_CELL,        (int)ScreenType::PROJECT,   (int)ScreenType::SONG,       (int)ScreenType::MIXER, (int)ScreenType::EFFECTS},
        {EMPTY_CELL,        (int)ScreenType::PROJECT,   (int)ScreenType::CHAIN,      (int)ScreenType::MIXER, (int)ScreenType::EFFECTS},
        {(int)ScreenType::SCALE,     (int)ScreenType::GROOVE, (int)ScreenType::PHRASE,     (int)ScreenType::MIXER, (int)ScreenType::EFFECTS},
        {(int)ScreenType::INST_POOL, (int)ScreenType::MODS,   (int)ScreenType::INSTRUMENT, (int)ScreenType::MIXER, (int)ScreenType::EFFECTS},
        {EMPTY_CELL,        (int)ScreenType::PROJECT,   (int)ScreenType::TABLE,      (int)ScreenType::MIXER, (int)ScreenType::EFFECTS},
    };
    if (col < 0 || col > 4) col = 2;  // the phrase column is the fallback, as in Kotlin
    return L[col][row];
}

}  // namespace

void NavigationMapModule::draw(Canvas& c, int x, int y, const NavigationMapState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    // Which column are we in? A shared screen has none of its own, so it uses the one we came from.
    const int screenCol  = screen_column(s.currentScreen);
    const int currentCol = (screenCol == -1) ? s.sourceColumn : screenCol;

    // The grid: the always-visible main row, plus this column's own screens.
    int grid[5][5];
    for (int row = 0; row < 5; ++row)
        for (int col = 0; col < 5; ++col) grid[row][col] = EMPTY_CELL;

    grid[2][0] = static_cast<int>(ScreenType::SONG);
    grid[2][1] = static_cast<int>(ScreenType::CHAIN);
    grid[2][2] = static_cast<int>(ScreenType::PHRASE);
    grid[2][3] = static_cast<int>(ScreenType::INSTRUMENT);
    grid[2][4] = static_cast<int>(ScreenType::TABLE);

    const int col = (currentCol < 0 || currentCol > 4) ? 2 : currentCol;
    for (int row = 0; row < 5; ++row) grid[row][col] = column_layout(col, row);

    // The pool's fast-jump INSTRUMENT cell, at row 0 / col 4 — to the RIGHT of the pool (row 0 / col
    // 3), which is where R+RIGHT goes from there. It shows both while ON the pool and while on an
    // INSTRUMENT reached from it; in the latter case THAT cell is the current position, not the normal
    // row-2 instrument (they share a ScreenType, so position — not identity — is what disambiguates).
    const bool onPoolInstrument = (s.currentScreen == ScreenType::INSTRUMENT && s.instrumentFromPool);
    if (s.currentScreen == ScreenType::INST_POOL || onPoolInstrument)
        grid[0][4] = static_cast<int>(ScreenType::INSTRUMENT);

    for (int row = 0; row < 5; ++row) {
        for (int gcol = 0; gcol < 5; ++gcol) {
            const int cell = grid[row][gcol];
            if (cell == EMPTY_CELL) continue;  // empty cells are just background

            const ScreenType screen = static_cast<ScreenType>(cell);
            const int        cellX  = x + (gcol * CELL_WIDTH);
            const int        cellY  = y + (row * CELL_HEIGHT);

            const bool isCurrent = onPoolInstrument ? (row == 0 && gcol == 4)
                                                    : (screen == s.currentScreen);

            const std::string label  = screen_short_label(screen);
            const int         labelW = Canvas::text_width(label, CHAR_SPACING, FONT_SCALE);

            c.draw_text(label, cellX + (CELL_WIDTH - labelW) / 2, cellY + 3,
                        isCurrent ? t.textCursor : t.textValue, CHAR_SPACING, FONT_SCALE);
        }
    }
}

}  // namespace pt::ui
