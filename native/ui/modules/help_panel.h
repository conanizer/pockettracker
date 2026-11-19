#pragma once

// The panel the guide is drawn in (ui/guide.h): three lines in place of the visualizer, mascot at the
// left. `HELP_MAX_CHARS` derives from its width.
// Colours: VIZ BG under the lines' own inks, TXT TITLE mascot — readable wherever the scope is.

#include "ui/canvas.h"
#include "ui/guide.h"
#include "ui/help_text.h"
#include "ui/helpers.h"
#include "ui/mascot_sprite.h"
#include "ui/theme.h"

namespace pt::ui {

// One word of a controls line at (x, y): the buttons it opens with (`help_key_bytes`) in `keyInk`,
// the rest in `ink`. Returns its width in columns.
inline int draw_control_word(Canvas& c, const char* word, int bytes, int x, int y, Argb ink, Argb keyInk) {
    const auto columns = [&](int from, int to) {
        int n = 0;
        for (int i = from; i < to; ++n) i += detail::help_arrow_at(word + i) ? 3 : 1;
        return n;
    };
    const int keyBytes = help_key_bytes(word, bytes);
    const int keyCols  = columns(0, keyBytes);
    if (keyBytes > 0)
        c.text(std::string(word, static_cast<size_t>(keyBytes)), x, y, keyInk);
    if (keyBytes < bytes)
        c.text(std::string(word + keyBytes, static_cast<size_t>(bytes - keyBytes)), x + keyCols * CHAR_W, y, ink);
    return keyCols + columns(keyBytes, bytes);
}

class HelpPanelModule {
  public:
    static constexpr int WIDTH  = 620;
    static constexpr int HEIGHT = 70;

    // 64 + 3 + 3 = 70: snug top and bottom.
    static constexpr int MASCOT_MARGIN = 3;
    static constexpr int MASCOT_GUTTER = 7;

    // Three 21px rows in 70, the odd pixel on top.
    static constexpr int TEXT_TOP = 7;

    // From the panel's left edge.
    static constexpr int TEXT_X = MASCOT_MARGIN + MASCOT_W + MASCOT_GUTTER;

    // ⚠️ HELP_MAX_CHARS lives in help_text.h; this pins it to the geometry.
    static_assert(TEXT_X + HELP_MAX_CHARS * CHAR_W - CHAR_SPACING <= WIDTH,
                  "HELP_MAX_CHARS no longer fits beside the mascot");
    static_assert(TEXT_X + (HELP_MAX_CHARS + 1) * CHAR_W - CHAR_SPACING > WIDTH,
                  "HELP_MAX_CHARS is short by a character or more - widen it");

    void draw(Canvas& c, int x, int y, const GuideLines& lines, const Theme& t) const;
};

}  // namespace pt::ui
