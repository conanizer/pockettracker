#include "ui/modules/loading_strip.h"

#include "ui/helpers.h"

namespace pt::ui {

namespace {

// Two of the app's rows in the app's own font: the words on the top one, the bar on the bottom.
constexpr int TEXT_Y   = TEXT_PADDING;                    // row 1
constexpr int ROW2_Y   = ROW_HEIGHT;                      // row 2
constexpr int GLYPH_W  = (5 + CHAR_SPACING) * FONT_SCALE; // one column's advance

constexpr int INSET = 10;   // the left/right margin every bar in the app uses
constexpr int GAP   = GLYPH_W;

// The bar sits inside row 2's text band, a readout beside the percentage rather than a block.
constexpr int BAR_H = 9;
constexpr int BAR_Y = ROW2_Y + TEXT_PADDING + 3;

// One sweep of the indeterminate block: slow enough not to flicker, fast enough that a stall shows.
constexpr int SWEEP_MS = 1400;

constexpr const char* PREFIX = "LOADING ";
constexpr const char* CANCEL = "B=STOP";

}  // namespace

void draw_loading_strip(Canvas& c, const AppState::LoadingState& s, const Theme& t) {
    if (!s.shown) return;

    c.fill_rect(0, 0, DESIGN_W, LOADING_STRIP_H, t.meterBackground);

    // ── Row 1: what is being opened, and the way out ─────────────────────────────────────────────
    // "B=STOP" is measured first: it is the one part that must never be clipped.
    const int cancelW = Canvas::text_width(CANCEL, CHAR_SPACING, FONT_SCALE);
    c.draw_text(CANCEL, DESIGN_W - INSET - cancelW, TEXT_Y, t.textParam, CHAR_SPACING, FONT_SCALE);

    // The name's budget is DERIVED from what the rest of the row leaves, so a change to the wording
    // on either side cannot quietly push the file name out through the "B=STOP".
    const int nameCols = (DESIGN_W - INSET * 2 - cancelW - GAP -
                          Canvas::text_width(PREFIX, CHAR_SPACING, FONT_SCALE)) / GLYPH_W;
    const std::string line = PREFIX + Canvas::clip_text(s.detail, nameCols > 0 ? nameCols : 0);
    c.draw_text(line, INSET, TEXT_Y, t.textValue, CHAR_SPACING, FONT_SCALE);

    // ── Row 2: the bar, and the percentage where there is one ────────────────────────────────────
    // ⚠️ A percentage only where the file states a total (below zero: it does not, e.g. an mp3 with
    // no Xing header). Without one, the bar takes the number's width.
    int barW = DESIGN_W - INSET * 2;
    if (s.progress >= 0.0f) {
        const float       p   = s.progress > 1.0f ? 1.0f : s.progress;
        const std::string pct = std::to_string(static_cast<int>(p * 100.0f + 0.5f)) + "%";
        const int         w   = Canvas::text_width(pct, CHAR_SPACING, FONT_SCALE);
        c.draw_text(pct, DESIGN_W - INSET - w, ROW2_Y + TEXT_PADDING, t.textValue, CHAR_SPACING,
                    FONT_SCALE);
        barW -= w + GAP;
    }

    // A filled shape, so a ground colour one step up from the strip rather than an ink role.
    c.fill_rect(INSET, BAR_Y, barW, BAR_H, t.rowEvery4th);
    c.stroke_rect(INSET, BAR_Y, barW, BAR_H, t.textParam);

    if (s.progress >= 0.0f) {
        const float p    = s.progress > 1.0f ? 1.0f : s.progress;
        const int   fill = static_cast<int>(p * static_cast<float>(barW - 2) + 0.5f);
        if (fill > 0) c.fill_rect(INSET + 1, BAR_Y + 1, fill, BAR_H - 2, t.textValue);
    } else {
        // A sweeping block, not a filling bar: nothing knows a total, only that the load is running.
        const int   blockW = barW / 5;
        const int   travel = barW - 2 - blockW;
        const int   phase  = s.elapsedMs % (SWEEP_MS * 2);
        const float f      = (phase < SWEEP_MS)
                                 ? static_cast<float>(phase) / static_cast<float>(SWEEP_MS)
                                 : 1.0f - static_cast<float>(phase - SWEEP_MS) /
                                              static_cast<float>(SWEEP_MS);
        if (travel > 0)
            c.fill_rect(INSET + 1 + static_cast<int>(f * static_cast<float>(travel)), BAR_Y + 1,
                        blockW, BAR_H - 2, t.textValue);
    }
}

}  // namespace pt::ui
