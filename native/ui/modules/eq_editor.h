#pragma once

// ─── THE EQ EDITOR ───────────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/EqModule.kt + ui/overlays/EqEditorOverlay.kt: three parametric bands, a
// live spectrum, and the response curve they add up to.
//
// ── It is an OVERLAY, not a screen, and that is not a technicality ───────────────────────────────
//
// It has no cell in the 5×5 navigation grid and `currentScreen` does not change when it opens. It is
// raised from a CELL — the EQ slot on INSTRUMENT, on INST.POOL, on MIXER's master strip, on either of
// EFFECTS' two input rows, and on the SAMPLE EDITOR's FX row — and every one of those cells has spent
// seven sessions dialling a slot NUMBER with nothing behind it. `EqCallerContext` is what remembers
// which cell asked, because B+LEFT/RIGHT cycles the slot from inside the editor and that has to write
// back into the caller's own field: `masterEqSlot`, `reverbInputEq`, `delayInputEq`, an instrument's
// `eqSlot`, or the sample editor's `fxValue`. Five different fields, one gesture.
//
// ⚠️ The caller is captured AT OPEN TIME and never re-read. That is deliberate and it is what makes
// the editor safe to leave open: the pool cursor moving underneath it must not silently re-point the
// bands at a different instrument. (Android lets that cursor move — see the B+UP note in
// input_dispatcher.cpp — which is the bug this session found.)
//
// ── Its geometry is its own ──────────────────────────────────────────────────────────────────────
//
// 495 × 392 at the normal module position, so the oscilloscope strip and the right-hand bar stay
// drawn around it. Not full-screen like the browser and the sample editor: an EQ is something you dial
// WHILE a note rings, and the note monitor is how you see that it is still ringing.
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

/**
 * WHICH EQ slot reference opened the editor — Kotlin's `sealed class EqCallerContext`, as a tag plus
 * the one payload any arm carries. A sealed hierarchy in C++ would be five heap-allocated types to
 * express "an enum, and sometimes an int".
 */
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
     * The spectrum of the signal this EQ actually sits on — the master bus, a send's input, or one
     * instrument's own voices (ui/engine_feed.h picks the source from the caller). Null is not an
     * error: it means no engine, and the visualization simply draws its grid with nothing behind it,
     * which is exactly what `ptshot` renders.
     */
    const float* spectrum      = nullptr;
    int          spectrumCount = 0;

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
     * NOT const, and it is the port's THIRD stateful module — but a different kind of state from the
     * other two. The oscilloscope's peak-hold and the mixer's falling meters are part of the PICTURE:
     * the frame is a function of the previous frame. This is a pure CACHE — the picture is a function
     * of the project alone, and the cache only makes it affordable.
     *
     * ⚠️ And it is needed MORE here than on Android, not less. Kotlin caches the response curve because
     * the animating spectrum recomposes this screen ~20×/s and the 3-band transfer function would
     * otherwise run per pixel per redraw. The shell redraws at 60 Hz — three times as often — over 495
     * columns × 3 bands of `sin`/`cos`/`pow`/`log10`. On the 1 GHz ARM the port is aimed at, that is a
     * frame budget spent on a curve that only changes when a band does.
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

    // ── The pieces the golden measures, exposed so `ptinput` can drive them directly ─────────────

    /**
     * ⚠️ THE DISPLAY-AWARE FREQ NUDGE, and the one place in this module where a one-step press can move
     * the value by more than one step.
     *
     * `freq` is 0..255 mapped logarithmically over 20 Hz..20 kHz, so a single hex step is ~2.7% — FINER
     * than the readout can show near 1 kHz, where "1.2kHz" covers several adjacent values. A+UP there
     * would leave the number on screen unchanged and the cell would feel stuck. So a SINGLE step keeps
     * advancing in the direction pressed until `format_freq_hz` produces a different string (bounded by
     * 0..255). Multi-step moves — A+LEFT/RIGHT's ±16, and A+B's reset to 0x80 — are applied exactly.
     *
     * The label comparison is what decides where it stops, which makes `format_freq_hz` load-bearing for
     * the CELL rather than merely for the picture — and therefore something the golden must measure over
     * the whole domain rather than argue about. It does: the `EQFREQ` sweep walks all 256 values.
     */
    static int step_freq_display_aware(int old_value, int target);

    /** `freq` hex → Hz, log-mapped 20..20000. */
    static float freq_hz_from_hex(int hex);

    /** "440Hz" / "1.2kHz" / "16kHz" — the readout, and the tie-breaker the nudge above stops on. */
    static std::string format_freq_hz(float hz);

    /** "+3.5" / "-12.0" — a band's gain, from its stored 0..240. */
    static std::string format_gain_db(float db);

private:
    void draw_header(Canvas& c, int x, int y, const EqState& s) const;
    void draw_visualization(Canvas& c, int x, int y, const EqState& s);
    void draw_editor(Canvas& c, int x, int y, const EqState& s) const;

    /** dB → a y offset inside the VIS_H panel. 0 dB is the centre line. */
    static int db_to_pixel(float db);
    /** Hz → an x offset inside the panel's width. */
    static int freq_to_pixel(float freq);

    /** The three bands' combined gain at one frequency, clamped to the panel's ±VIS_DB. */
    static float combined_gain_db(const std::vector<songcore::EqBand>& bands, float freq);

    // ── The response-curve cache (see draw) ──────────────────────────────────────────────────────
    //
    // One dB value per pixel column, rebuilt only when the viewed SLOT or any of the twelve band values
    // changes. The key is a content HASH rather than the bands themselves, because an EqBand is mutated
    // in place by `handle_input` — the object identity never changes, so only its contents can say that
    // the curve is stale.
    int   curveCacheSlot_       = -1;
    long long curveCacheHash_   = 0;
    float curveCacheDb_[WIDTH]  = {};
};

}  // namespace pt::ui
