#pragma once

// ─── THE SAMPLE EDITOR ───────────────────────────────────────────────────────────────────────────
//
// The C++ twin of ui/modules/SampleEditorModule.kt — the biggest single module in the app, and the
// last screen that is neither the arrangement nor the filesystem. It is where a sample stops being a
// file and becomes an instrument: trim it, slice it, normalise it, pitch it to the grid, chop it into
// a folder of one-shots.
//
// ── IT PORTS NO DSP, AND THAT IS THE POINT ──────────────────────────────────────────────────────
//
// Every one of the twelve operations already exists, in C++, in the engine (`native/sample-editor.cpp`
// + `native/transient-detector.cpp`), because Android's JNI layer is a thin forward and always was.
// crop, copy, cut, dupl, paste, del / norm, fade+, fade−, silence, reverse, undo — plus the FX row,
// the pitch shift, the time stretch and the transient detector. **Not one line of signal processing is
// written this session.** What was missing was never the DSP: it was the module that draws the
// waveform, the cursor that walks it, and the dispatcher arm that turns a button into one of those
// calls. That is what lands here.
//
// ── FULL-SCREEN, like the file browser ──────────────────────────────────────────────────────────
//
// 640×480 with no oscilloscope strip and no right bar (`SampleEditorModule.height = 480`). A waveform
// needs the width, and the note monitor has nothing to say about a sample that is not playing.
//
// ── THE ROW MAP IS SPARSE, and the gaps are the layout ──────────────────────────────────────────
//
// The cursor does not walk 0..19. It walks 1, 2, 8, 10, 11, 13, 14, 16, 18, 19 — rows 3–7 are the
// WAVEFORM (which the cursor enters as a unit, not as five rows), and 9/12/15/17 are spacers that
// exist only to give the sections air. Which is why `row_above`/`row_below` are a lookup table rather
// than `row ± 1`, and why they take the SLICE METHOD: with slicing OFF, row 11 (the slice detail) is
// not drawn and must not be reachable, so DOWN from 10 skips straight to 13.
//
// ⚠️ **Rows 3..8 are the SELECTION, and their D-pad does not move the cursor — it drags an edge.**
// A+LEFT/RIGHT nudges the selection edge coarsely (±totalFrames/16), A+UP/DOWN finely (±/256), both
// scaled by the zoom and snapped to a zero crossing when SNAP is on. That is why `cursor_context()`
// answers `none()` for the whole range: there is no cell there to increment. The dispatcher handles it.

#include <cstdint>
#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

/**
 * The editor's own session state — and unlike every other module's state struct, this one is
 * PERSISTENT (it lives in `AppState`, not on the draw stack).
 *
 * That is not a port artifact: it is what a sample editor IS. Nothing here can be derived from the
 * project, because none of it is *in* the project — the zoom level, the selection, the detected
 * transients and the pending pitch shift are all facts about an editing session, and they are gone the
 * moment it closes. The audio itself is in the ENGINE (which is why there is no PCM here, only the 620
 * min/max pairs the waveform draws from), and the only things that reach the document are the ones the
 * user explicitly saves: the name, the file path, and the slice markers.
 */
struct SampleEditorState {
    int sampleId     = 0;
    int instrumentId = 0;

    int cursorRow = 1;
    int cursorCol = 0;

    // ── The sample, as the engine reports it ─────────────────────────────────────────────────────
    std::string sampleName;
    std::string sampleFilePath;      // empty = none (the `sampleFilePath == null` convention)
    int         sampleRate  = 44100; // the FILE's rate, not the device's
    int         totalFrames = 0;

    /** 620 (min, max) pairs — one per waveform pixel column. Refilled by the feed on zoom/scroll. */
    std::vector<float> waveformData;

    // ── Row 1: the view ──────────────────────────────────────────────────────────────────────────
    int  zoomLevel     = 0;      // 0 = 1×, 1 = 2×, … 4 = 16×
    int  sourceMode    = 0;      // 0 = LEFT, 1 = RIGHT, 2 = STEREO, 3 = MONO
    int  rateMode      = 0;      // 0 = HIGH, 1 = NORM, 2 = LOFI  (a DESTRUCTIVE decimation)
    bool hasStereoData = false;  // the loaded sample has a right channel

    // ── Row 2: the edit controls ─────────────────────────────────────────────────────────────────
    /** A PENDING shift, in semitones — nothing is resampled until SAVE bakes it (`bake_pending_pitch`). */
    int  pitchSemitones = 0;
    int  durationIndex  = 2;     // "1 BAR" — the target SYNC stretches/pitches to
    bool snapEnabled    = true;  // snap a dragged selection edge to the nearest zero crossing

    // ── The selection, in FRAMES (not the 0..255 the instrument stores) ──────────────────────────
    int64_t selectionStart = 0;
    int64_t selectionEnd   = 0;

    // ── Slicing ──────────────────────────────────────────────────────────────────────────────────
    int sliceMethod      = 2;   // 0 = TRANSIENT, 1 = DIVIDE, 2 = OFF
    int sliceSensitivity = 64;  // TRANSIENT: the detector's threshold
    int sliceDivisions   = 8;   // DIVIDE: cut into N equal parts
    int sliceIndex       = 0;

    /**
     * ⚠️ Kept for its ONE use: `undo` clamps it, and the two SYNC ops rescale it, so a length-changing
     * operation does not leave it pointing past the end. It is NOT what the slice rows read — they call
     * `effective_slice_position()`, which derives the position from the method and the index. Kotlin
     * carries the same otherwise-dead field, and dropping it would silently change what UNDO restores.
     */
    int64_t slicePosition = 0;

    /**
     * The slice boundaries: N markers → N+1 slices. In TRANSIENT they are DETECTED (the feed runs the
     * engine's detector when the method or the sensitivity changes); in OFF they are whatever the WAV's
     * `cue ` chunk carried, shown read-only. DIVIDE ignores them and computes its own.
     */
    std::vector<int> transientMarkers;

    // ── Row 16: the FX row ───────────────────────────────────────────────────────────────────────
    int fxType   = 0;   // 0 = OTT, 1 = DUST, 2 = DRIVE, 3 = EQ, 4 = SYNC
    int fxValue  = 0;   // the amount — or, for EQ, the slot in the 128-preset bank
    int syncType = 0;   // when fxType == SYNC: 0 = RPITCH (resample), 1 = TSTRETCH (SOLA)

    // ── Flags ────────────────────────────────────────────────────────────────────────────────────
    /** An unsaved destructive edit is in the engine's buffer. B asks before dropping it. */
    bool isModified       = false;
    bool showConfirmClose = false;

    /** 0..1 while the sample is sounding, −1 when it is not. The playhead line over the waveform. */
    float playbackPosition = -1.0f;

    // ── Derived (Kotlin's computed properties, and the same arithmetic) ──────────────────────────

    /** (start, end) of slice `idx` under the current method. OFF = the whole sample. */
    void slice_bounds(int idx, int64_t& start, int64_t& end) const;

    /** The START of the slice under the cursor — what row 11's position column reads. */
    int64_t effective_slice_position() const;

    /**
     * The visible frame window. At zoom 0 it is the whole sample; above that it is a window of
     * `totalFrames >> zoom` frames CENTRED on whatever the cursor is pointing at — the selection edge
     * under the cursor, or the active slice, or the playhead while it is running (so a zoomed-in view
     * scrolls with playback rather than sitting still while the audio leaves the window).
     */
    int64_t view_start() const;
    int64_t view_end() const;

    /** "MM:SS.CC", scaled by the PENDING pitch shift — so DURATION previews what SAVE will bake. */
    std::string duration_display() const;
};

/** What `handle_input` changed — enough for the dispatcher to know which engine calls to make. */
struct SampleEditorInputResult {
    bool modified = false;
    /**
     * RATE is the one row whose edit is DESTRUCTIVE: it re-decimates the audio in the engine, which
     * changes the sample's length and its rate ratio. The dispatcher owns those calls, so the module
     * reports the transition rather than making them (a module that reached for the engine could not
     * be drawn by ptshot — see ui/engine_feed.h).
     */
    bool rateModeChanged = false;
};

class SampleEditorModule {
public:
    static constexpr int WIDTH  = 640;
    static constexpr int HEIGHT = 480;   // full-screen: no top strip, no right bar

    // The waveform panel: a 5px gap under the three header rows, 155px tall, 620 wide at x + 10.
    static constexpr int WAVEFORM_Y = 73;
    static constexpr int WAVEFORM_H = 155;
    static constexpr int WAVEFORM_W = 620;

    /** Rows 8..19 sit under the waveform: `(row − 8) * ROW_HEIGHT + 228`. */
    static int content_y(int row);

    // ── The vocabularies ─────────────────────────────────────────────────────────────────────────
    static const std::vector<std::string>& source_values();    // LEFT / RIGHT / STEREO / MONO
    static const std::vector<std::string>& rate_values();      // HIGH / NORM / LOFI
    static const std::vector<std::string>& duration_values();  // 4 BAR … 1/32
    static const std::vector<std::string>& fx_types();         // OTT / DUST / DRIVE / EQ / SYNC
    static const std::vector<std::string>& sync_types();       // RPITCH / TSTRETCH
    static const std::vector<std::string>& slice_methods();    // TRANSIENT / DIVIDE / OFF
    static const std::vector<std::string>& ops_row1();         // CROP COPY CUT DUPL PASTE DEL
    static const std::vector<std::string>& ops_row2();         // NORM FADE+ FADE- SLNC REV UNDO

    /** The FX row's type indices, named — `fxValue` means a different thing under each. */
    static constexpr int FX_OTT   = 0;
    static constexpr int FX_DUST  = 1;
    static constexpr int FX_DRIVE = 2;
    static constexpr int FX_EQ    = 3;   // fxValue is an EQ SLOT (0..127), not an amount
    static constexpr int FX_SYNC  = 4;   // fxValue is unused; syncType picks RPITCH vs TSTRETCH

    static constexpr int SLICE_TRANSIENT = 0;
    static constexpr int SLICE_DIVIDE    = 1;
    static constexpr int SLICE_OFF       = 2;

    // ── The sparse row map ───────────────────────────────────────────────────────────────────────
    //
    // Rows 3–7 are the waveform and 9/12/15/17 are spacers, so neither is a step of one. `slice_method`
    // is a parameter because row 11 does not exist with slicing OFF — DOWN from 10 must reach 13.

    /** The next navigable row ABOVE `row`. Row 1 wraps to 19. */
    static int row_above(int row, int slice_method = -1);
    /** The next navigable row BELOW `row`. Row 19 wraps to 1. */
    static int row_below(int row, int slice_method = -1);
    /** The rightmost column of `row` — 5 on the two op rows, 0 on NAME, 2 or 3 on SAVE. */
    static int max_col_for_row(int row, int slice_method = SLICE_OFF);

    // ── The three questions every module answers ─────────────────────────────────────────────────

    void draw(Canvas& c, int x, int y, const SampleEditorState& s, const Theme& t) const;

    CursorContext cursor_context(const SampleEditorState& s) const;

    /**
     * Apply a resolved action to the editor state. Mutates `s` in place — the same shape every other
     * C++ module takes, and what lets `tools/ptinput` byte-compare the RESULTING CELL rather than only
     * the context and the action (S3's finding: a tool that compares context + action alone is
     * completely blind to a module that writes the right value into the wrong field).
     */
    SampleEditorInputResult handle_input(SampleEditorState& s, const InputAction& action) const;

private:
    /** The 620×155 panel: the wave, the S/E edges, the slice boundaries, and the playhead. */
    void draw_waveform(Canvas& c, int x, int y, const SampleEditorState& s, const Theme& t) const;

    /** "ARE YOU SURE?" — B on a modified sample. Full-screen, over everything. */
    void draw_confirm_dialog(Canvas& c, int x, int y, const Theme& t) const;
};

}  // namespace pt::ui
