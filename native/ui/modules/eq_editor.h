#pragma once

// ─── THE EQ EDITOR ───────────────────────────────────────────────────────────────────────────────
//
// Three parametric bands, a live spectrum, and the response curve they add up to.
//
// ── It is an OVERLAY, not a screen, and that is not a technicality ───────────────────────────────
//
// No cell in the navigation grid; `currentScreen` does not change. It is raised from an EQ slot
// CELL — INSTRUMENT, INST.POOL, MIXER's master strip, EFFECTS' two input rows, the SAMPLE EDITOR's FX
// row — and `EqCallerContext` remembers which, because B+LEFT/RIGHT cycles the slot from inside the
// editor and must write back into that caller's own field.
//
// ⚠️ The caller is captured AT OPEN TIME and never re-read, so a cursor moving underneath cannot
// re-point the bands at a different instrument.
//
// ── Its geometry is its own ──────────────────────────────────────────────────────────────────────
//
// 495 × 392 at the normal module position, so the scope strip and the note monitor stay drawn — an
// EQ is dialled WHILE a note rings.
//
//     y =   0.. 20   header — "EQ 07" and who opened it
//     y =  21.. 41   one blank row (the module's own top spacer)
//     y =  42..261   the visualization: spectrum fill + spectrum curve + the yellow response curve
//     y = 263..      the editor: a label column and three band columns, four param rows each
//
// ── The cursor is ONE int over a 3×4 grid ────────────────────────────────────────────────────────
//
// `cursorRow` is 0..11; band = row / 4 (the COLUMN on screen) and param = row % 4 (the ROW). So UP and
// DOWN walk the four params of one band, and LEFT and RIGHT change band while keeping the param — which
// is what lets you sweep the same parameter across all three bands without moving your thumb. Both
// CLAMP; neither wraps.

#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

/** The six band types, in the order the engine's `type` field indexes them. 0 = OFF = bypassed. */
const std::vector<std::string>& eq_band_type_names();

/** WHICH EQ slot reference opened the editor: a tag plus the one payload any arm carries. */
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

/** The overlay's live state — what `AppState` holds while it is up. */
struct EqEditorState {
    bool            isOpen    = false;
    int             slotIndex = 0;  // 0..127, an index into Project::eqPresets
    int             cursorRow = 0;  // 0..11 = band * 4 + param
    EqCallerContext caller{};

    int cursor_band() const { return cursorRow / 4; }
    int cursor_param() const { return cursorRow % 4; }
};

/** What the module is handed to draw one frame. */
struct EqState {
    const songcore::Project& project;
    int                      slotIndex = 0;
    int                      cursorRow = 0;
    EqCallerContext          caller{};

    /**
     * The spectrum of the signal this EQ sits on (engine_feed.h picks it from the caller). Null
     * draws the grid with nothing behind it.
     */
    const float* spectrum      = nullptr;
    int          spectrumCount = 0;

    /**
     * The rate the ENGINE's EQ bands were built at, so the plotted curve is the curve the audio has.
     * `EqBandModule::reset(sr)` takes the device rate; a plot at a literal 44100 disagrees with it on
     * every device that is not 44.1 kHz, worst near Nyquist — and this panel's axis runs to 20 kHz.
     */
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

    /**
     * NOT const: a pure CACHE of the response curve (unlike the scope's and mixer's picture state).
     * The curve is 495 columns × 3 bands of sin/cos/pow/log10 and the shell redraws at 60 Hz on a
     * 1 GHz ARM; it only changes when a band does.
     */
    void draw(Canvas& c, int x, int y, const EqState& s);

    CursorContext cursor_context(const EqState& s) const;

    /**
     * Write the resolved action into the band under the cursor. `slot_index` and `cursor_row` are the
     * overlay's, not a screen's — the editor has no screen cursor at all.
     *
     * ⚠️ FREQ does NOT simply take the action's value: see `step_freq_display_aware` below.
     */
    EqInputResult handle_input(songcore::Project& project, int slot_index, int cursor_row,
                               const InputAction& action) const;

    // ── Pure helpers, public so they can be driven directly ──────────────────────────────────────

    /**
     * ⚠️ THE DISPLAY-AWARE FREQ NUDGE — one press can move more than one step. `freq` is 0..255,
     * log over 20 Hz..20 kHz, so one step (~2.7%) is finer than the readout near 1 kHz. A SINGLE step
     * keeps going until `format_freq_hz` prints something different (bounded by 0..255); multi-step
     * moves (±16, A+B's 0x80) are exact. So `format_freq_hz` is load-bearing for the cell.
     */
    static int step_freq_display_aware(int old_value, int target);

    /** `freq` hex → Hz, log-mapped 20..20000. */
    static float freq_hz_from_hex(int hex);

    /** "440Hz" / "1.2kHz" / "16kHz" — the readout, and the tie-breaker the nudge above stops on. */
    static std::string format_freq_hz(float hz);

    /** "+3.5" / "-12.0" — a band's gain, from its stored 0..240. */
    static std::string format_gain_db(float db);

    /**
     * Is the spectrum panel's picture already empty? — the idle gate's question, answered about the
     * LAST FRAME DRAWN. ⚠️ Nothing ages here (the feed re-polls every 50 ms regardless); what hangs
     * on screen is the last frame, so once the empty one is drawn there is nothing left to change.
     */
    bool spectrum_at_rest() const { return spectrumAtRest_; }

private:
    void draw_header(Canvas& c, int x, int y, const EqState& s) const;
    void draw_visualization(Canvas& c, int x, int y, const EqState& s);
    void draw_editor(Canvas& c, int x, int y, const EqState& s) const;

    /** dB → a y offset inside the VIS_H panel. 0 dB is the centre line. */
    static int db_to_pixel(float db);
    /** Hz → an x offset inside the panel's width. */
    static int freq_to_pixel(float freq);

    /** The three bands' combined gain at one frequency, clamped to the panel's ±VIS_DB. */
    static float combined_gain_db(const std::vector<songcore::EqBand>& bands, float freq,
                                  float sampleRate);

    // ── The response-curve cache (see draw) ──────────────────────────────────────────────────────
    //
    // One dB per pixel column, rebuilt when the slot or any band value changes. Keyed by a content
    // HASH: `handle_input` mutates an EqBand in place, so only its contents can say it is stale.
    int   curveCacheSlot_       = -1;
    long long curveCacheHash_   = 0;
    float curveCacheDb_[WIDTH]  = {};

    /**
     * Whether the last drawn frame's spectrum had any height — written from the same `specY[]` the
     * picture is drawn from. Starts TRUE: nothing drawn, nothing to fall.
     */
    bool spectrumAtRest_ = true;
};

}  // namespace pt::ui
