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
#include "ui/modules/confirm_dialog.h"
#include "ui/modules/effects_editor.h"
#include "ui/modules/eq_editor.h"
#include "ui/modules/file_browser.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/help_panel.h"
#include "ui/modules/instrument_editor.h"
#include "ui/modules/instrument_pool.h"
#include "ui/modules/midi_settings.h"
#include "ui/modules/mixer.h"
#include "ui/modules/modulation.h"
#include "ui/modules/navigation_map.h"
#include "ui/modules/oscilloscope.h"
#include "ui/modules/phrase_editor.h"
#include "ui/modules/project_editor.h"
#include "ui/modules/qwerty_keyboard.h"
#include "ui/modules/scale_editor.h"
#include "ui/modules/sample_editor.h"
#include "ui/modules/settings_editor.h"
#include "ui/modules/song_editor.h"
#include "ui/modules/table_editor.h"
#include "ui/modules/theme_editor.h"

namespace pt::ui {

/** y of the editor module — 6 + 70 + 6 = 82. The one number every screen's draw() starts from. */
inline constexpr int EDITOR_Y = SCREEN_SPACER + OscilloscopeModule::HEIGHT + SCREEN_SPACER;

/** Left edge of the right bar: 640 − 115 − 10 = 515. */
inline constexpr int RIGHT_BAR_X = DESIGN_W - NavigationMapModule::WIDTH - SIDE_SPACER;

/** Right edge of the editor clip: 640 − 115 − 10 − 6 = 509. */
inline constexpr int EDITOR_CLIP_RIGHT = RIGHT_BAR_X - SCREEN_SPACER;

// PROJECT's NAME row holds more characters than it can show, and the number it CAN show is a fact
// about this clip — the module knows neither the clip nor the x it is drawn at. Pinned here, from
// both sides, because a window one column too WIDE draws into the right bar (which is the bug that
// put it here: 20 cells were drawn and 17 arrived) and one column too NARROW throws a column away.
inline constexpr int PROJECT_NAME_CELLS_X = SIDE_SPACER + ProjectModule::VALUE_X;
static_assert(PROJECT_NAME_CELLS_X + ProjectModule::NAME_VISIBLE_CHARS * CHAR_W <= EDITOR_CLIP_RIGHT,
              "PROJECT's NAME window spills past the editor clip");
static_assert(PROJECT_NAME_CELLS_X + (ProjectModule::NAME_VISIBLE_CHARS + 1) * CHAR_W
                  > EDITOR_CLIP_RIGHT,
              "PROJECT's NAME window is narrower than the row affords — another cell fits");

// INST.POOL's USED RAM readout sits in the title row, and the same clip applies to it. Pinned here
// for the same reason: the module knows neither the clip nor the x it is drawn at, so nothing inside
// it could catch a total that runs off the edge — and a RAM figure is exactly the string that grows
// when things are going wrong.
inline constexpr int POOL_RAM_VALUE_X = SIDE_SPACER + InstrumentPoolModule::RAM_VALUE_X;
static_assert(POOL_RAM_VALUE_X + InstrumentPoolModule::RAM_MAX_CHARS * CHAR_W - CHAR_SPACING
                  <= EDITOR_CLIP_RIGHT,
              "INST.POOL's USED RAM total can print past the editor clip");
static_assert(SIDE_SPACER + InstrumentPoolModule::RAM_LABEL_X + 3 * CHAR_W <= POOL_RAM_VALUE_X,
              "INST.POOL's RAM label overlaps its value");

class TrackerLayout {
public:
    /**
     * Paint one frame of `state` onto `c`. The only entry point the shell (or ptshot) calls.
     *
     * NOT const, and that is the oscilloscope's doing — and, since S5, the MIXER's: both carry
     * peak-hold state across draws, because a falling peak marker is a function of the PREVIOUS frame
     * and there is nowhere else for that to live. Since S8 the EQ EDITOR joins them, for a different
     * reason: it caches its response curve, which is a function of the project alone but far too
     * expensive to recompute 60 times a second (eq_editor.h).
     */
    void draw(Canvas& c, const AppState& state);

    /**
     * Is something on screen still falling that no input will bring back?
     *
     * The shell's idle gate (C7) knows about two animation sources — the transport and the master
     * waveform — and both are AUDIO. These are the ones that are not: the MIXER's peak markers step down
     * once per peak poll (mixer.h) and the SPECTRUM strip's bars and dots step down once per frame
     * (oscilloscope.h), and both do it inside `draw`, so both need frames for a few seconds after the
     * audio has gone silent or they freeze part-way. The gate is allowed to be conservative, never
     * clever, so it asks the modules that own the state rather than modelling the fall itself.
     *
     * ⚠️ **Each half is gated on its module being DRAWN, and that is load-bearing rather than tidy.**
     * Off the MIXER the feed stops polling peaks (ui/engine_feed.h), so `peaksVersion` never moves and
     * the markers never age; off the strip — a full-screen module has the frame — the bars never age
     * either, and neither does a visualizer mode that draws no bars at all. Answering "still falling"
     * in any of those would pin the redraw loop at 60 Hz for as long as the user stayed there, with
     * nothing moving on screen to show for it: the exact battery cost the idle gate exists to avoid.
     */
    bool has_falling_meters(const AppState& state) const;

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

    /** The global status line — "SAVED", "SEQ CLEANED", "NO FREE PHRASES" — over the scope strip. */
    void draw_status_line(Canvas& c, const AppState& s) const;

    /** The selection scope ("SEL:CELL") and clipboard contents ("PHR:2x3") — top-RIGHT of the scope strip. */
    void draw_selection_clipboard(Canvas& c, const AppState& s) const;

    OscilloscopeModule    oscilloscope_;
    HelpPanelModule       helpPanel_;    // drawn INSTEAD of the oscilloscope while help is up
    PhraseEditorModule    phraseEditor_;
    ChainEditorModule     chainEditor_;
    SongEditorModule      songEditor_;
    TableModule           tableModule_;
    GrooveModule          grooveModule_;
    ScaleModule           scaleModule_;
    InstrumentEditorModule instrumentEditor_;
    InstrumentPoolModule  instrumentPool_;
    ModulationModule      modulation_;
    MixerModule           mixer_;        // stateful (peak-hold) — see draw()
    EffectModule          effects_;
    ProjectModule         project_;
    SettingsModule        settings_;
    MidiModule            midi_;
    NavigationMapModule   navigationMap_;
    FileBrowserModule     fileBrowser_;   // full-screen: draw() returns before the furniture
    SampleEditorModule    sampleEditor_;  // full-screen too — a waveform wants the width
    QwertyKeyboardOverlay qwerty_;        // modal: drawn LAST, over everything, including the browser
    EqModule              eq_;            // stateful (curve cache); drawn INSTEAD of the screen module
    ThemeEditorModule     themeEditor_;   // drawn INSTEAD of the screen module, on the EQ's terms (S9)
};

}  // namespace pt::ui
