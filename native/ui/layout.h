#pragma once

// ─── The layout ──────────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of `TrackerLayout.drawLayout` in ui/PixelPerfectRenderer.kt: the one function that
// paints a frame. It fills the background, draws the oscilloscope strip, picks the module for the
// current screen, and paints the furniture down the right — BPM, the note monitor, the navigation map.
//
// The geometry is the whole reason this exists as its own file, and it is exact:
//
//     y =  0..  5   6px top spacer
//     y =  6.. 75   oscilloscope        (620 × 70, at x = 10)
//     y = 76.. 81   6px spacer
//     y = 82..473   the editor module   (510 × 392, at x = 10)      ← 6 + 70 + 6 + 392 = 474
//     x = 515..     the right bar       (115 wide: BPM, note monitor, navigation map)
//
// Editors are clipped to the left of the right bar so a full-width row highlight cannot bleed into
// the BPM readout — Compose spells that `clipRect(right = …)`, and the number is derived below rather
// than written down, exactly as the Kotlin does.
//
// ⚠️ The clip is NOT cosmetic and the module widths do not make it redundant: an editor is 510px wide
// at x=10, so its row backgrounds run to x=520 — 11px INTO the right bar's column. The clip at 509 is
// what cuts them. (S1 had the nav map at 80px wide instead of 115, which put the clip at 544 and so
// clipped nothing at all; the row highlights ran under the BPM row. The width below is the Kotlin's.)

#include "ui/app_state.h"
#include "ui/canvas.h"
#include "ui/helpers.h"
#include "ui/modules/chain_editor.h"
#include "ui/modules/effects_editor.h"
#include "ui/modules/file_browser.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/instrument_editor.h"
#include "ui/modules/instrument_pool.h"
#include "ui/modules/mixer.h"
#include "ui/modules/modulation.h"
#include "ui/modules/navigation_map.h"
#include "ui/modules/oscilloscope.h"
#include "ui/modules/phrase_editor.h"
#include "ui/modules/qwerty_keyboard.h"
#include "ui/modules/sample_editor.h"
#include "ui/modules/song_editor.h"
#include "ui/modules/table_editor.h"

namespace pt::ui {

/** y of the editor module — 6 + 70 + 6 = 82. The one number every screen's draw() starts from. */
inline constexpr int EDITOR_Y = SCREEN_SPACER + OscilloscopeModule::HEIGHT + SCREEN_SPACER;

/** Left edge of the right bar: 640 − 115 − 10 = 515. */
inline constexpr int RIGHT_BAR_X = DESIGN_W - NavigationMapModule::WIDTH - SIDE_SPACER;

/** Right edge of the editor clip: 640 − 115 − 10 − 6 = 509. */
inline constexpr int EDITOR_CLIP_RIGHT = RIGHT_BAR_X - SCREEN_SPACER;

class TrackerLayout {
public:
    /**
     * Paint one frame of `state` onto `c`. The only entry point the shell (or ptshot) calls.
     *
     * NOT const, and that is the oscilloscope's doing — and, since S5, the MIXER's: both carry
     * peak-hold state across draws, because a falling peak marker is a function of the PREVIOUS frame
     * and there is nowhere else for that to live. They remain the only two stateful modules in the UI.
     */
    void draw(Canvas& c, const AppState& state);

private:
    /**
     * A screen with no module yet: its title, and "COMING SOON" in the middle. This is not port
     * scaffolding — it is `drawPlaceholderScreen` from the Kotlin renderer, which the Android app
     * used for exactly the same reason while its own screens were being written. Porting it first
     * means navigation works across the whole app from S1, and each screen simply stops being a
     * placeholder as it lands.
     */
    void draw_placeholder(Canvas& c, int x, int y, ScreenType screen, const Theme& t) const;

    /** BPM · the 8-track note monitor · the navigation map. Hidden on the full-screen screens. */
    void draw_right_bar(Canvas& c, const AppState& s) const;

    OscilloscopeModule    oscilloscope_;
    PhraseEditorModule    phraseEditor_;
    ChainEditorModule     chainEditor_;
    SongEditorModule      songEditor_;
    TableModule           tableModule_;
    GrooveModule          grooveModule_;
    InstrumentEditorModule instrumentEditor_;
    InstrumentPoolModule  instrumentPool_;
    ModulationModule      modulation_;
    MixerModule           mixer_;        // stateful (peak-hold) — see draw()
    EffectModule          effects_;
    NavigationMapModule   navigationMap_;
    FileBrowserModule     fileBrowser_;   // full-screen: draw() returns before the furniture
    SampleEditorModule    sampleEditor_;  // full-screen too — a waveform wants the width
    QwertyKeyboardOverlay qwerty_;        // modal: drawn LAST, over everything, including the browser
};

}  // namespace pt::ui
