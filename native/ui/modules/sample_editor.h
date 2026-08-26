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
// A+UP/DOWN nudges the selection edge coarsely (±totalFrames/16), A+LEFT/RIGHT finely (±/256), both
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
/**
 * A slice boundary the user has moved or made: where it is, and where its METHOD had put it.
 *
 * ⚠️ **`originFrame` is the boundary's IDENTITY, and it is what makes A+B mean anything now that a
 * boundary may be dragged past its neighbours.** The list is sorted by `frame`, so a boundary's INDEX
 * is its place on the SCREEN and changes under it as it is dragged — the index is not the boundary.
 * Reading the reset target off the index is how "put slice 01 back" became "put whatever is now at 01
 * back", which lands on a position another boundary already holds.
 *
 * ⭐ It carries the FRAME rather than which computed cut it was, so a reset needs no arithmetic and no
 * arm per method: put it back where it says. −1 means MADE BY HAND — there is no computed position to
 * return to, and A+B removes it instead.
 */
struct SliceMarker {
    int frame       = 0;
    int originFrame = -1;
};

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
    // 0 = LEFT, 1 = RIGHT, 2 = STEREO, 3 = MONO. A session opens on STEREO whenever there IS a right
    // channel — `init_sample_editor_state` sets it, because only that mode saves both (sample_edit.h).
    int  sourceMode    = 0;
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
    int sliceMethod      = 2;   // 0 = TRANSIENT, 1 = DIVIDE, 2 = OFF, 3 = MANUAL
    int sliceSensitivity = 64;  // TRANSIENT: the detector's threshold
    int sliceDivisions   = 8;   // DIVIDE: cut into N equal parts
    /**
     * Row 11's cell: which SLICE, under every method. ⚠️ Slice 00's left edge is the sample's own
     * start rather than a marker, so the boundary the cursor is on is the one BEFORE it —
     * `slice_marker_index()`. Under MANUAL the index reaches one slice past the last real one; that
     * slot is where the next boundary is made.
     */
    int sliceIndex       = 0;

    /**
     * ⚠️ Kept for its ONE use: `undo` clamps it, and the two SYNC ops rescale it, so a length-changing
     * operation does not leave it pointing past the end. It is NOT what the slice rows read — they call
     * `effective_slice_position()`, which derives the position from the method and the index. Kotlin
     * carries the same otherwise-dead field, and dropping it would silently change what UNDO restores.
     */
    int64_t slicePosition = 0;

    /**
     * The markers the FILE carries: the WAV's `cue ` chunk, as the project loaded it. Snapshotted at
     * open, and CLEARED by an op that changes the sample's length — a frame index into audio that has
     * been replaced describes nothing (`refresh_sample_view`).
     *
     * ⚠️ This is what SLICE = OFF *means* — the sample as it already is on disk — and it is what a save
     * under OFF writes back. Keeping it apart from the detector's output is what stops a detour through
     * TRANSIENT adding slices to a file the user turned slicing off for.
     *
     * ⭐ It is also where MANUAL STARTS, which is what makes MANUAL an edit of the slicing a sample
     * already has rather than a fresh start over the top of it.
     *
     * ⚠️ SORTED, UNIQUE and strictly inside the sample — a `cue ` chunk promises none of the three, so
     * `init_sample_editor_state` makes it so on the way in. Every marker reader rests on all three.
     */
    std::vector<int> fileMarkers;

    /**
     * The DETECTOR's output, TRANSIENT only: N markers → N+1 slices.
     *
     * ⚠️ EMPTY IS THE TRIGGER — the feed re-runs the detector whenever it finds TRANSIENT with no
     * markers, which is the state a method or sensitivity change leaves behind (engine_feed.h), and a
     * length-changing op too: emptied there, the detector comes back with the cuts in the NEW audio.
     * DIVIDE carries no list at all; it computes its boundaries from `sliceDivisions` on every read.
     */
    std::vector<int> transientMarkers;

    /**
     * The boundaries the USER has placed or moved, sorted by frame and strictly increasing.
     *
     * It starts as a copy of whatever the method already shows — the detected cuts under TRANSIENT, the
     * arithmetic ones under DIVIDE, the FILE's own cue points under MANUAL — and OVERRIDES that source
     * from then on. Each entry remembers which computed cut it is, because the order can change
     * afterwards; ⚠️ a MANUAL entry remembers −1 instead, so A+B on it DELETES rather than restores, and
     * that is the whole of "drop a slice the sample came with".
     *
     * ⚠️ Only live while the stamp below still matches — see `manual_markers_live()`. ⚠️ And EMPTY is a
     * meaningful live value: MANUAL with every boundary deleted, which must not fall back to the file's.
     */
    std::vector<SliceMarker> manualMarkers;

    /**
     * The (method, parameter) pair `manualMarkers` was made under. ⚠️ Not bookkeeping: it is what
     * makes "changing the method or its setting resets the hand-placed markers" DERIVED rather than
     * something every future write to `sliceMethod`, `sliceSensitivity` and `sliceDivisions` has to
     * remember to do. The feed clears a stale list once per frame, above every reader.
     */
    int manualKeyMethod = -1;
    int manualKeyParam  = -1;

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

    /** The parameter `manualMarkers` is keyed to: SENS under TRANSIENT, BY under DIVIDE, none else. */
    int manual_key_param() const;

    /**
     * True while the hand-placed markers still belong to the method and parameter now on screen.
     * ⚠️ The stamp alone — an EMPTY `manualMarkers` can be live, and has to be.
     */
    bool manual_markers_live() const;

    /**
     * What the METHOD alone says, with no hand-placed boundaries on top: the file's own under OFF **and
     * under MANUAL**, the detector's under TRANSIENT, and nothing under DIVIDE (its boundaries are
     * arithmetic, so there is no list to hand back and its callers compute instead).
     *
     * ⚠️ Almost nothing wants this. Read `marker_count()` / `marker_position()` instead — they are the
     * seam every reader goes through, and they are where a live hand-placed set overrides this one.
     * One vector answering several questions is what made a save under OFF write the detector's
     * markers into the file.
     */
    const std::vector<int>& method_markers() const;

    /** How many boundaries are in force. ⚠️ **Every reader counts through here.** */
    int marker_count() const;

    /**
     * Boundary `k`'s frame — its own once it has been placed, and otherwise the boundary it was BORN
     * on: its predecessor's frame, or frame 0 below the first. ⚠️ Not clamped to the end of the sample;
     * an unplaced boundary sits on the one to its left, which is what makes it a place to step away from.
     */
    int64_t marker_position(int k) const;

    /**
     * Which boundary the row-11 cursor is on: the marker at the LEFT edge of slice `sliceIndex`.
     *
     * ⚠️ −1 on slice 00, whose left edge is the sample's own start — there is no marker there and
     * nothing to drag. ⚠️ And one PAST the end of the list on MANUAL's free slot, where the boundary
     * has not been made yet; `marker_position` answers for that one with the boundary it would be born
     * on, which is the place it is dragged away from.
     */
    int slice_marker_index() const;

    /**
     * Row 11's top index. N markers make N + 1 slices, so it is N — except under MANUAL, which reaches
     * one further: that slot is the next boundary, and moving it off the boundary to its left is what
     * makes it exist. ⭐ There is no separate "how many are unlocked" to keep in step, and none of the
     * duplicate-position checks the request asked for — the list's own length is the whole rule.
     */
    int slice_index_ceiling() const;

    /** (start, end) of slice `idx` under the current method. OFF = the whole sample. */
    void slice_bounds(int idx, int64_t& start, int64_t& end) const;

    /** What row 11's position column reads: the START of the slice under the cursor. */
    int64_t effective_slice_position() const;

    /**
     * The visible frame window. At zoom 0 it is the whole sample; above that it is a window of
     * `totalFrames >> zoom` frames CENTRED on whatever the cursor is pointing at — the selection edge
     * under the cursor, or the active slice.
     *
     * A running playhead overrides that centre, so a zoomed-in view scrolls with the audio rather than
     * sitting still while it leaves the window — but ONLY while the selection is too wide to fit. Once
     * both edges are inside the window there is nothing off-screen to follow, and the view holds still.
     */
    int64_t view_start() const;
    int64_t view_end() const;

    /** "MM:SS.CC", scaled by the PENDING pitch shift — so DURATION previews what SAVE will bake. */
    std::string duration_display() const;

    /**
     * "NNNBPM" — the tempo this sample IS, if it holds exactly the DURATION row's bar count. The
     * inverse of what SYNC does: SYNC stretches the audio to fit `duration_beats` at the project's
     * tempo, this reads the tempo off the audio it already has. With no pending pitch, this number
     * reaching the project's tempo is SYNC's "already on the grid" test — both sides reduce to
     * `beats * 60 / seconds`.
     *
     * ⚠️ Scaled by the pending PITCH, which SYNC is NOT. The clock beside this one previews the same
     * shift (`duration_display`), and one length shown in two units cannot be allowed to disagree with
     * itself. So a pending shift moves this away from the tempo SYNC would still fit — which is the
     * honest reading, because SYNC's RPITCH discards that shift rather than baking it.
     *
     * "---BPM" when there is no sample, or when the answer is outside 1..999: a five-digit tempo is
     * the DURATION row being wrong about the sample, and printing it would dress that up as a reading.
     */
    std::string bpm_display() const;
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

    /**
     * How many beats one DURATION index is worth — "1 BAR" is 4. The list halves at every step, so
     * this is `16 >> index` rather than a second table beside `duration_values()`: two tables that
     * must agree row for row is how a new duration gets the beat count of its neighbour.
     *
     * SYNC fits the sample to this many beats at the project's tempo; `bpm_display` runs it backwards
     * to say what tempo the sample is already at.
     */
    static double duration_beats(int duration_index);
    static const std::vector<std::string>& fx_types();         // OTT / DUST / DRIVE / EQ / SYNC
    static const std::vector<std::string>& sync_types();       // RPITCH / TSTRETCH
    static const std::vector<std::string>& slice_methods();    // TRANSIENT / DIVIDE / OFF / MANUAL
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
    // ⚠️ APPENDED after OFF, never inserted. `sliceMethod` is an index into `slice_methods()` that
    // survives a session, and `SLICE_OFF` is compared by name in a dozen places across the module, the
    // dispatcher and the feed — every one of which keeps meaning what it means only because 2 is still 2.
    static constexpr int SLICE_MANUAL    = 3;

    /** Row 10's second cell: TRANSIENT has SENS and DIVIDE has BY. OFF and MANUAL have neither. */
    static constexpr bool slice_has_parameter(int slice_method) {
        return slice_method == SLICE_TRANSIENT || slice_method == SLICE_DIVIDE;
    }

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
