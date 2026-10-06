#pragma once

// ─── The layout ──────────────────────────────────────────────────────────────────────────────────
//
// The one function that paints a frame: background, oscilloscope strip, the current screen's module,
// and the furniture down the right (BPM, note monitor, navigation map).
//
//     y =  0..  5   6px top spacer
//     y =  6.. 75   oscilloscope        (620 × 70, at x = 10)
//     y = 76.. 81   6px spacer
//     y = 82..473   the editor module   (510 × 392, at x = 10)      ← 6 + 70 + 6 + 392 = 474
//     x = 515..     the right bar       (115 wide: BPM, note monitor, navigation map)
//
// ⚠️ Editors are CLIPPED at x = 509: an editor is 510 px wide at x = 10, so its row backgrounds run
// 11 px into the right bar's column. The clip is derived below, never typed.

#include "ui/app_state.h"
#include "ui/canvas.h"
#include "ui/helpers.h"
#include "ui/modules/chain_editor.h"
#include "ui/modules/confirm_dialog.h"
#include "ui/modules/effects_editor.h"
#include "ui/modules/eq_editor.h"
#include "ui/modules/file_browser.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/help_overlay.h"
#include "ui/modules/help_panel.h"
#include "ui/modules/instrument_editor.h"
#include "ui/modules/instrument_pool.h"
#include "ui/modules/loading_strip.h"
#include "ui/modules/midi_map_editor.h"
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

// PROJECT's NAME row shows only as many characters as fit this clip — a fact neither the module nor
// its x knows, so it is pinned here from both sides (too wide draws into the right bar, too narrow
// wastes a column).
inline constexpr int PROJECT_NAME_CELLS_X = SIDE_SPACER + ProjectModule::VALUE_X;
static_assert(PROJECT_NAME_CELLS_X + ProjectModule::NAME_VISIBLE_CHARS * CHAR_W <= EDITOR_CLIP_RIGHT,
              "PROJECT's NAME window spills past the editor clip");
static_assert(PROJECT_NAME_CELLS_X + (ProjectModule::NAME_VISIBLE_CHARS + 1) * CHAR_W
                  > EDITOR_CLIP_RIGHT,
              "PROJECT's NAME window is narrower than the row affords — another cell fits");

// INST.POOL's USED RAM readout is in the title row under the same clip; pinned here for the same reason
// — a RAM figure is exactly the string that grows when things go wrong.
inline constexpr int POOL_RAM_VALUE_X = SIDE_SPACER + InstrumentPoolModule::RAM_VALUE_X;
static_assert(POOL_RAM_VALUE_X + InstrumentPoolModule::RAM_MAX_CHARS * CHAR_W - CHAR_SPACING
                  <= EDITOR_CLIP_RIGHT,
              "INST.POOL's USED RAM total can print past the editor clip");
static_assert(SIDE_SPACER + InstrumentPoolModule::RAM_LABEL_X + 3 * CHAR_W <= POOL_RAM_VALUE_X,
              "INST.POOL's RAM label overlaps its value");

// ⚠️ The sample editor's waveform hosts the help panel only because they share width and left edge;
// neither knows the other, so the agreement is pinned here.
static_assert(SampleEditorModule::WAVEFORM_W == HelpPanelModule::WIDTH,
              "the waveform panel is no longer the width the help panel draws");
static_assert(SampleEditorModule::WAVEFORM_H >= HelpPanelModule::HEIGHT,
              "the waveform panel is too short to hold the help panel");

class TrackerLayout {
public:
    /**
     * Paint one frame of `state` onto `c` — the shell's (and the screenshot tests') only entry point. Not const: the
     * oscilloscope and MIXER carry peak-hold state across draws, and the EQ editor caches its response
     * curve. It is `draw_frame` plus the loading strip — see `draw_frame`.
     */
    void draw(Canvas& c, const AppState& state);

    /**
     * Is something on screen still falling that no input will bring back? The shell's idle gate knows
     * the audio sources; this covers the MIXER's peak markers and the SPECTRUM strip's bars, which fall
     * inside `draw` and freeze part-way without frames. The modules that own the state are asked.
     * ⚠️ Each half is gated on its module being DRAWN: off the MIXER the peaks are not polled and never
     * age, and a full-screen module has no strip — answering "falling" there would pin the loop at
     * 60 Hz with nothing moving.
     */
    bool has_falling_meters(const AppState& state) const;

private:
    /**
     * Everything below the loading strip. ⚠️ It RETURNS EARLY with no document and on the full-screen
     * modules, so anything that must appear on EVERY screen goes in `draw`.
     */
    void draw_frame(Canvas& c, const AppState& state);

    /** The FILE BROWSER or SAMPLE EDITOR, edge to edge, with no furniture. */
    void draw_full_screen(Canvas& c, const AppState& s);

    /** The oscilloscope strip, or the help panel in its place. */
    void draw_scope_strip(Canvas& c, const AppState& s);

    /** The current screen's module — or the EQ / THEME editor standing in for it. */
    void draw_editor(Canvas& c, const AppState& s);

    /** The modals, over everything. */
    void draw_overlays(Canvas& c, const AppState& s);

    /** A screen with no module: its title and "COMING SOON". */
    void draw_placeholder(Canvas& c, int x, int y, ScreenType screen, const Theme& t) const;

    /** BPM · the 8-track note monitor · the navigation map. Hidden on the full-screen screens. */
    void draw_right_bar(Canvas& c, const AppState& s) const;

    /** The global status line — "SAVED", "SEQ CLEANED", "NO FREE PHRASES" — over the scope strip. */
    void draw_status_line(Canvas& c, const AppState& s) const;

    /** The selection scope ("SEL:CELL") and clipboard contents ("PHR:2x3") — top-RIGHT of the scope strip. */
    void draw_selection_clipboard(Canvas& c, const AppState& s) const;

    OscilloscopeModule    oscilloscope_;
    HelpPanelModule       helpPanel_;    // drawn INSTEAD of the oscilloscope while help is up
    HelpOverlayModule     helpOverlay_;  // the full help, over everything
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
    MidiMapModule         midiMap_;
    NavigationMapModule   navigationMap_;
    FileBrowserModule     fileBrowser_;   // full-screen: draw() returns before the furniture
    SampleEditorModule    sampleEditor_;  // full-screen too — a waveform wants the width
    QwertyKeyboardOverlay qwerty_;        // modal: drawn LAST, over everything, including the browser
    EqModule              eq_;            // stateful (curve cache); drawn INSTEAD of the screen module
    ThemeEditorModule     themeEditor_;   // drawn INSTEAD of the screen module, like the EQ
};

}  // namespace pt::ui
