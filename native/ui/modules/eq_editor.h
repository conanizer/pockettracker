#pragma once

// The EQ editor: three parametric bands, a live spectrum and their summed response curve. An
// overlay, not a screen: raised from an EQ slot cell, and `EqCallerContext` remembers which so
// B+LEFT/RIGHT can write the slot back there. ⚠️ The caller is captured at open and never re-read.
// Drawn at the module position so the scope and note monitor stay up — an EQ is dialled while a note
// rings.
//
// The cursor is one int over a 3×4 grid: band = row / 4 (a column on screen), param = row % 4. UP/DOWN
// walk one band's params, LEFT/RIGHT change band keeping the param. Both clamp.

#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

// In the order the engine's `type` indexes them. 0 = OFF.
const std::vector<std::string>& eq_band_type_names();

// Which EQ slot reference opened the editor.
struct EqCallerContext {
    enum class Kind { MASTER, REVERB_IN, DELAY_IN, INSTRUMENT, SAMPLE_EDITOR_FX };

    Kind kind    = Kind::MASTER;
    int  instrId = 0;  // INSTRUMENT only

    static EqCallerContext master() { return {Kind::MASTER, 0}; }
    static EqCallerContext reverb_in() { return {Kind::REVERB_IN, 0}; }
    static EqCallerContext delay_in() { return {Kind::DELAY_IN, 0}; }
    static EqCallerContext instrument(int id) { return {Kind::INSTRUMENT, id}; }
    static EqCallerContext sample_editor_fx() { return {Kind::SAMPLE_EDITOR_FX, 0}; }
};

struct EqEditorState {
    bool            isOpen    = false;
    int             slotIndex = 0;  // 0..127, an index into Project::eqPresets
    int             cursorRow = 0;  // 0..11 = band * 4 + param
    EqCallerContext caller{};

    int cursor_band() const { return cursorRow / 4; }
    int cursor_param() const { return cursorRow % 4; }
};

// What the module is handed to draw one frame.
struct EqState {
    const songcore::Project& project;
    int                      slotIndex = 0;
    int                      cursorRow = 0;
    EqCallerContext          caller{};

    // The spectrum of the signal this EQ sits on; null draws the empty grid.
    const float* spectrum      = nullptr;
    int          spectrumCount = 0;

    // The rate the engine's bands were built at, so the plotted curve is the one the audio has.
    float sampleRate = 44100.0f;

    Theme theme = theme_classic();
};

struct EqInputResult {
    bool modified      = false;
    bool eqBandChanged = false;  // → the caller must push this band to the engine
};

class EqModule {
public:
    static constexpr int WIDTH  = 495;
    static constexpr int HEIGHT = 392;

    static constexpr int VIS_H  = 220;    // the spectrum + curve panel
    static constexpr float VIS_DB = 15.0f;  // the ±dB the panel spans

    static constexpr int HEADER_H = 21;
    static constexpr int ROW_H    = 21;
    static constexpr int EDITOR_Y = HEADER_H + ROW_H + VIS_H + 1;

    static constexpr int LABEL_COL_W = 90;
    static constexpr int BAND_COL_W  = 135;  // (495 − 90) / 3

    static constexpr int MAX_CURSOR_ROW = 11;

    // Not const: caches the response curve, which only changes when a band does.
    void draw(Canvas& c, int x, int y, const EqState& s);

    CursorContext cursor_context(const EqState& s) const;

    // Writes the action into the band under the cursor. ⚠️ FREQ goes through `step_freq_display_aware`.
    EqInputResult handle_input(songcore::Project& project, int slot_index, int cursor_row,
                               const InputAction& action) const;

    // ── Pure helpers, public so they can be driven directly ──────────────────────────────────────

    // ⚠️ A single FREQ step keeps going until `format_freq_hz` prints something different (one step is
    // finer than the readout); multi-step moves are exact.
    static int step_freq_display_aware(int old_value, int target);

    // 0..255 → Hz, log over 20..20000.
    static float freq_hz_from_hex(int hex);

    // "440Hz" / "1.2kHz" / "16kHz".
    static std::string format_freq_hz(float hz);

    // "+3.5" / "-12.0".
    static std::string format_gain_db(float db);

    // The last drawn spectrum was empty, so the idle gate has nothing to redraw.
    bool spectrum_at_rest() const { return spectrumAtRest_; }

private:
    void draw_header(Canvas& c, int x, int y, const EqState& s) const;
    void draw_visualization(Canvas& c, int x, int y, const EqState& s);
    void draw_editor(Canvas& c, int x, int y, const EqState& s) const;

    // 0 dB is the centre line.
    static int db_to_pixel(float db);
    static int freq_to_pixel(float freq);

    // The three bands' gain at one frequency, clamped to ±VIS_DB.
    static float combined_gain_db(const std::vector<songcore::EqBand>& bands, float freq,
                                  float sampleRate);

    // The response curve, one dB per column. Keyed by a content hash: `handle_input` edits a band in
    // place, so only its contents can say it is stale.
    int   curveCacheSlot_       = -1;
    long long curveCacheHash_   = 0;
    float curveCacheDb_[WIDTH]  = {};

    // Written from the same heights the picture is drawn from. True before anything is drawn.
    bool spectrumAtRest_ = true;
};

}  // namespace pt::ui
