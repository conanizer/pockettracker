#pragma once

// ─── The layout ──────────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of `TrackerLayout.drawLayout` in ui/PixelPerfectRenderer.kt: the one function that
// paints a frame. It fills the background, picks the module for the current screen, and (from S2)
// draws the furniture around it — the oscilloscope strip on top, the BPM/note-monitor/navigation bar
// down the right.
//
// The geometry is the whole reason this exists as its own file, and it is exact:
//
//     y =  0..  5   6px top spacer
//     y =  6.. 75   oscilloscope        (620 × 70, at x = 10)
//     y = 76.. 81   6px spacer
//     y = 82..473   the editor module   (620 × 392, at x = 10)      ← 6 + 70 + 6 + 392 = 474
//     x = 515..     the right bar       (80 wide: BPM, note monitor, navigation map)
//
// Editors are clipped to the left of the right bar so a full-width row highlight cannot bleed into
// the BPM readout — Compose spells that `clipRect(right = …)`, and the number is derived below rather
// than written down, exactly as the Kotlin does.

#include "ui/app_state.h"
#include "ui/canvas.h"
#include "ui/helpers.h"
#include "ui/modules/phrase_editor.h"

namespace pt::ui {

// Module sizes that the layout needs before the module itself exists. Each moves into its own module
// header as that module lands (S2: oscilloscope, navigation map) — they are here, not invented: the
// Kotlin layout reads them off `OscilloscopeModule(width = 620, height = 70)` and `navigationMap.width`.
inline constexpr int OSCILLOSCOPE_W = 620;
inline constexpr int OSCILLOSCOPE_H = 70;
inline constexpr int NAV_MAP_W      = 80;

/** y of the editor module — 6 + 70 + 6 = 82. The one number every screen's draw() starts from. */
inline constexpr int EDITOR_Y = SCREEN_SPACER + OSCILLOSCOPE_H + SCREEN_SPACER;

/** Right edge of the editor clip: 640 − 80 − 10 − 6 = 544. */
inline constexpr int EDITOR_CLIP_RIGHT = DESIGN_W - NAV_MAP_W - SIDE_SPACER - SCREEN_SPACER;

class TrackerLayout {
public:
    /** Paint one frame of `state` onto `c`. The only entry point the shell (or ptshot) calls. */
    void draw(Canvas& c, const AppState& state) const;

private:
    /**
     * A screen with no module yet: its title, and "COMING SOON" in the middle. This is not port
     * scaffolding — it is `drawPlaceholderScreen` from the Kotlin renderer, which the Android app
     * used for exactly the same reason while its own screens were being written. Porting it first
     * means navigation works across the whole app from S1, and each screen simply stops being a
     * placeholder as it lands.
     */
    void draw_placeholder(Canvas& c, int x, int y, ScreenType screen, const Theme& t) const;

    PhraseEditorModule phraseEditor_;
};

}  // namespace pt::ui
