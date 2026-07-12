#include "ui/layout.h"

#include <string>

#include "ui/helpers.h"

namespace pt::ui {

void TrackerLayout::draw(Canvas& c, const AppState& s) const {
    const Theme& t = s.theme;

    c.fill_rect(0, 0, DESIGN_W, DESIGN_H, t.background);

    if (!s.project) return;  // no document: the background is the honest thing to draw

    const int moduleX = SIDE_SPACER;
    const int moduleY = EDITOR_Y;

    // ── The editor ───────────────────────────────────────────────────────────────────────────────
    // Clipped to the left of the right bar. FILE_BROWSER and SAMPLE_EDITOR are full-screen and draw
    // OUTSIDE this clip when they land (S6/S7); everything else lives inside it.
    {
        Canvas::ClipScope clip(c, 0, 0, EDITOR_CLIP_RIGHT, DESIGN_H);

        switch (s.currentScreen) {
            case ScreenType::PHRASE: {
                PhraseEditorState ps{s.project->phrases[static_cast<size_t>(s.currentPhrase)]};
                ps.cursorRow      = s.cursorRow;
                ps.cursorColumn   = s.cursorColumn;
                ps.playbackRow    = s.playbackRow;
                ps.isPlaying      = s.isPlaying;
                ps.selectionMode  = s.selectionMode;
                ps.isCellSelected = [&s](int row, int col) { return s.is_cell_selected(row, col); };
                ps.theme          = t;
                phraseEditor_.draw(c, moduleX, moduleY, ps);
                break;
            }

            default:
                draw_placeholder(c, moduleX, moduleY, s.currentScreen, t);
                break;
        }
    }

    // ── The right bar (BPM · note monitor · navigation map) and the oscilloscope strip land in S2.
    // The regions they occupy are deliberately left as background rather than filled with something
    // temporary: the Phase 2 shell already learned that a stand-in readout is the kind of progress
    // that gets built on. The geometry above already reserves their space exactly.
}

void TrackerLayout::draw_placeholder(Canvas& c, int x, int y, ScreenType screen,
                                     const Theme& t) const {
    // Same size as the phrase editor's content column (620 × 392).
    c.fill_rect(x, y, OSCILLOSCOPE_W, 392, t.background);

    c.draw_text(screen_label(screen), x + 20, y + TEXT_PADDING, t.textTitle, CHAR_SPACING,
                FONT_SCALE);

    const std::string message = "COMING SOON";
    const int         msgW    = Canvas::text_width(message, CHAR_SPACING, FONT_SCALE);
    c.draw_text(message, x + (OSCILLOSCOPE_W - msgW) / 2, y + 180, t.textEmpty, CHAR_SPACING,
                FONT_SCALE);
}

}  // namespace pt::ui
