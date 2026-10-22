#pragma once

// Paints a frame: the oscilloscope strip, the current screen's module, and the right bar.
//     y =  6.. 75   oscilloscope        (620 × 70, at x = 10)
//     y = 82..473   the editor module   (510 × 392, at x = 10)
//     x = 515..     the right bar       (115 wide: BPM, note monitor, navigation map)
// Editors are clipped at x = 509, or their row backgrounds would run into the right bar.

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

inline constexpr int EDITOR_Y = SCREEN_SPACER + OscilloscopeModule::HEIGHT + SCREEN_SPACER;   // 82
inline constexpr int RIGHT_BAR_X = DESIGN_W - NavigationMapModule::WIDTH - SIDE_SPACER;      // 515
inline constexpr int EDITOR_CLIP_RIGHT = RIGHT_BAR_X - SCREEN_SPACER;                         // 509

// Widths only this clip can check, pinned from both sides.
inline constexpr int PROJECT_NAME_CELLS_X = SIDE_SPACER + ProjectModule::VALUE_X;
static_assert(PROJECT_NAME_CELLS_X + ProjectModule::NAME_VISIBLE_CHARS * CHAR_W <= EDITOR_CLIP_RIGHT,
              "PROJECT's NAME window spills past the editor clip");
static_assert(PROJECT_NAME_CELLS_X + (ProjectModule::NAME_VISIBLE_CHARS + 1) * CHAR_W
                  > EDITOR_CLIP_RIGHT,
              "PROJECT's NAME window is narrower than the row affords — another cell fits");

inline constexpr int POOL_RAM_VALUE_X = SIDE_SPACER + InstrumentPoolModule::RAM_VALUE_X;
static_assert(POOL_RAM_VALUE_X + InstrumentPoolModule::RAM_MAX_CHARS * CHAR_W - CHAR_SPACING
                  <= EDITOR_CLIP_RIGHT,
              "INST.POOL's USED RAM total can print past the editor clip");
static_assert(SIDE_SPACER + InstrumentPoolModule::RAM_LABEL_X + 3 * CHAR_W <= POOL_RAM_VALUE_X,
              "INST.POOL's RAM label overlaps its value");

// The sample editor's waveform hosts the help panel; neither knows the other.
static_assert(SampleEditorModule::WAVEFORM_W == HelpPanelModule::WIDTH,
              "the waveform panel is no longer the width the help panel draws");
static_assert(SampleEditorModule::WAVEFORM_H >= HelpPanelModule::HEIGHT,
              "the waveform panel is too short to hold the help panel");

class TrackerLayout {
public:
    // Not const: the scope and MIXER keep peak-hold state, the EQ editor a curve cache.
    void draw(Canvas& c, const AppState& state);

    // Is something still falling (MIXER peaks, SPECTRUM bars) that needs frames to settle?
    // ⚠️ Only while its module is drawn, or the loop pins at 60 Hz with nothing moving.
    bool has_falling_meters(const AppState& state) const;

private:
    // ⚠️ Returns early with no document and on the full-screen modules; what every screen needs goes
    // in `draw`.
    void draw_frame(Canvas& c, const AppState& state);

    void draw_full_screen(Canvas& c, const AppState& s);   // the browser or the sample editor
    void draw_scope_strip(Canvas& c, const AppState& s);
    void draw_editor(Canvas& c, const AppState& s);        // or the EQ / theme editor in its place
    void draw_overlays(Canvas& c, const AppState& s);
    void draw_placeholder(Canvas& c, int x, int y, ScreenType screen, const Theme& t) const;
    void draw_right_bar(Canvas& c, const AppState& s) const;
    void draw_status_line(Canvas& c, const AppState& s) const;
    bool draw_chord_hint(Canvas& c, const AppState& s) const;
    void draw_selection_clipboard(Canvas& c, const AppState& s) const;

    OscilloscopeModule    oscilloscope_;
    HelpPanelModule       helpPanel_;    // instead of the oscilloscope while help is up
    HelpOverlayModule     helpOverlay_;
    PhraseEditorModule    phraseEditor_;
    ChainEditorModule     chainEditor_;
    SongEditorModule      songEditor_;
    TableModule           tableModule_;
    GrooveModule          grooveModule_;
    ScaleModule           scaleModule_;
    InstrumentEditorModule instrumentEditor_;
    InstrumentPoolModule  instrumentPool_;
    ModulationModule      modulation_;
    MixerModule           mixer_;
    EffectModule          effects_;
    ProjectModule         project_;
    SettingsModule        settings_;
    MidiModule            midi_;
    MidiMapModule         midiMap_;
    NavigationMapModule   navigationMap_;
    FileBrowserModule     fileBrowser_;
    SampleEditorModule    sampleEditor_;
    QwertyKeyboardOverlay qwerty_;
    EqModule              eq_;
    ThemeEditorModule     themeEditor_;
};

}  // namespace pt::ui
