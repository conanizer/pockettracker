#pragma once

// The sample editor: trim, slice, normalise, pitch to the grid, chop into one-shots. Full-screen
// (640×480): a waveform needs the width. All DSP is in the engine (`native/sample-editor.cpp`); this
// draws and walks the cursor.
//
// The row map is sparse: the cursor walks 1, 2, 8, 10, 11, 13, 14, 16, 18, 19 — rows 3–7 are the
// waveform, 9/12/15/17 spacers. Row 11 is unreachable with slicing OFF.
// ⚠️ On rows 3..8 the D-pad drags a selection edge (the dispatcher does it), so `cursor_context()`
// answers `none()` there.

#include <cstdint>
#include <string>
#include <vector>

#include "songcore/model.h"
#include "ui/canvas.h"
#include "ui/cursor.h"
#include "ui/theme.h"

namespace pt::ui {

// A slice boundary the user has moved or made.
// ⚠️ `originFrame` is its identity (the list is sorted by `frame`, so an index moves): A+B puts it
// back there; −1 = made by hand, so A+B removes it.
struct SliceMarker {
    int frame       = 0;
    int originFrame = -1;
};

// The TSTRETCH box's cells in cursor order. PREVIEW and APPLY share the last line.
enum class StretchCell { DURATION, BPM, PITCH, GRAIN, PREVIEW, APPLY };

// The editor's session state. The audio is in the engine (only the waveform's min/max pairs are
// here); name, path and slice markers reach the document only when saved.
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

    // 620 (min, max) pairs, one per pixel column. Refilled by the feed on zoom/scroll.
    std::vector<float> waveformData;

    // ── Row 1: the view ──────────────────────────────────────────────────────────────────────────
    int  zoomLevel     = 0;      // 0 = 1×, 1 = 2×, … 4 = 16×
    // 0 = LEFT, 1 = RIGHT, 2 = STEREO, 3 = MONO. Opens on STEREO when there is a right channel —
    // only that mode saves both.
    int  sourceMode    = 0;
    int  rateMode      = 0;      // 0 = HIGH, 1 = NORM, 2 = LOFI  (a DESTRUCTIVE decimation)
    bool hasStereoData = false;  // the loaded sample has a right channel

    // ── Row 2: the edit controls ─────────────────────────────────────────────────────────────────
    // A pending shift in semitones; SAVE bakes it.
    int  pitchSemitones = 0;
    int  durationIndex  = 2;     // "1 BAR" — the target SYNC stretches/pitches to
    // BIT: the depth the sample came in at, and the depth now (never above it). A destructive
    // requantise, and the depth SAVE writes. ⚠️ Not the instrument's CRUSH.
    int  sourceBitDepth = 16;
    int  bitDepth       = 16;
    // Snap a dragged edge to a zero crossing. ⚠️ Has no cell (BIT took its place), so always on.
    bool snapEnabled    = true;

    // ── The selection, in FRAMES (not the 0..255 the instrument stores) ──────────────────────────
    int64_t selectionStart = 0;
    int64_t selectionEnd   = 0;

    // ── Slicing ──────────────────────────────────────────────────────────────────────────────────
    int sliceMethod      = 2;   // 0 = TRANSIENT, 1 = DIVIDE, 2 = OFF, 3 = MANUAL
    int sliceSensitivity = 64;  // TRANSIENT: the detector's threshold
    int sliceDivisions   = 8;   // DIVIDE: cut into N equal parts
    // Row 11: which slice, under every method. ⚠️ Slice 00's left edge is the sample start, not a
    // marker (`slice_marker_index()`). Under MANUAL it reaches one past the last slice: the next one.
    int sliceIndex       = 0;

    // ⚠️ Kept for UNDO and the two SYNC ops; the slice rows read `effective_slice_position()`.
    int64_t slicePosition = 0;

    // The file's own markers (its `cue ` chunk), cleared by a length-changing op. What SLICE = OFF
    // shows and saves back, and where MANUAL starts. Sorted, unique, strictly inside the sample.
    std::vector<int> fileMarkers;

    // The detector's output, TRANSIENT only: N markers → N+1 slices.
    // ⚠️ Empty is the trigger: the feed re-runs the detector when it finds TRANSIENT with none.
    std::vector<int> transientMarkers;

    // The boundaries the user placed or moved, sorted, strictly increasing; they override the
    // method's. ⚠️ Live only while the stamp below matches (`manual_markers_live()`); empty is live.
    std::vector<SliceMarker> manualMarkers;

    // The (method, parameter) `manualMarkers` was made under; a change makes them stale.
    int manualKeyMethod = -1;
    int manualKeyParam  = -1;

    // ── Row 16: the FX row ───────────────────────────────────────────────────────────────────────
    int fxType   = 0;   // 0 = OTT, 1 = DUST, 2 = DRIVE, 3 = EQ, 4 = SYNC
    int fxValue  = 0;   // OTT / DUST / DRIVE: the amount. 0 = nothing to audition or apply
    int fxEqSlot = -1;  // EQ: the slot in the 128-preset bank. −1 = none, drawn "--"
    int syncType = 0;   // when fxType == SYNC: 0 = RPITCH (resample), 1 = TSTRETCH (SOLA)

    // ── The TSTRETCH box (A on the TSTRETCH cell) ────────────────────────────────────────────────
    // Session settings: APPLY on the FX row uses them whether or not the box is up.
    bool        stretchOpen    = false;
    StretchCell stretchCursor  = StretchCell::DURATION;
    int         stretchBpm     = 0;    // the tempo to fit to. ⚠️ 0 = the song's, read when it is needed
    int         stretchGrainMs = 40;   // the length of each piece, in milliseconds (sola-stretch.h)

    // ── Flags ────────────────────────────────────────────────────────────────────────────────────
    // An unsaved destructive edit is in the engine's buffer. B asks before dropping it.
    bool isModified       = false;
    bool showConfirmClose = false;

    // 0..1 while the sample sounds, −1 when not: the playhead line.
    float playbackPosition = -1.0f;

    // ── Derived ──────────────────────────────────────────────────────────────────────────────────

    // The parameter `manualMarkers` is keyed to: SENS under TRANSIENT, BY under DIVIDE.
    int manual_key_param() const;

    // The hand-placed markers still belong to what is on screen. ⚠️ The stamp alone: empty can be live.
    bool manual_markers_live() const;

    // The method's own markers: the file's under OFF/MANUAL, the detector's under TRANSIENT, none
    // under DIVIDE. ⚠️ Read through `marker_count()`/`marker_position()`, where hand-placed ones win.
    const std::vector<int>& method_markers() const;

    // How many boundaries are in force. ⚠️ Every reader counts through here.
    int marker_count() const;

    // Boundary `k`'s frame; an unplaced one sits on its predecessor (or frame 0). Not clamped.
    int64_t marker_position(int k) const;

    // The marker at the left edge of slice `sliceIndex`. −1 on slice 00; one past the list on
    // MANUAL's free slot.
    int slice_marker_index() const;

    // Row 11's top index: N markers → N, MANUAL one further.
    int slice_index_ceiling() const;

    // (start, end) of slice `idx`. OFF = the whole sample.
    void slice_bounds(int idx, int64_t& start, int64_t& end) const;

    // Row 11's position: the start of the slice under the cursor.
    int64_t effective_slice_position() const;

    // The visible frame window: the whole sample at zoom 0, else `totalFrames >> zoom` centred on
    // what the cursor points at. A playhead takes the centre only while the selection does not fit.
    int64_t view_start() const;
    int64_t view_end() const;

    // "MM:SS.CC", scaled by the pending pitch — what SAVE will bake.
    std::string duration_display() const;

    // The tempo the sample is if it holds DURATION's bar count; 0 outside 1..999. Scaled by the
    // pending pitch, as `duration_display` is.
    int source_bpm() const;

    // "NNNBPM", "---BPM" when none.
    std::string bpm_display() const;

    // TSTRETCH's tempo: the box's BPM, or the song's while unmoved.
    int stretch_bpm(int song_tempo) const { return stretchBpm > 0 ? stretchBpm : song_tempo; }

    // DURATION, BPM, PITCH, GRAIN back to their opening values.
    void reset_stretch();

    // OTT / DUST / DRIVE has an amount, or EQ has a slot.
    bool fx_armed() const;

    // What the engine takes beside `fxType`: the amount, or the EQ slot.
    int fx_param() const;
};

// ── The TSTRETCH box: a modal over the lower half ───────────────────────────────────────────────

// `d_row` walks the column, `d_col` crosses between PREVIEW and APPLY.
StretchCell stretch_step(StretchCell from, int d_row, int d_col);

CursorContext stretch_cursor_context(const SampleEditorState& s, int song_tempo);

void stretch_handle_input(SampleEditorState& s, const InputAction& action, int song_tempo);

// No-op when the box is not open.
void draw_stretch_editor(Canvas& c, const SampleEditorState& s, int song_tempo, const Theme& t);

// What `handle_input` changed, so the dispatcher knows which engine calls to make.
struct SampleEditorInputResult {
    bool modified = false;
    // RATE and BIT are destructive: the dispatcher rebuilds the audio from the cached original.
    bool rateModeChanged = false;
    bool bitDepthChanged = false;
};

class SampleEditorModule {
public:
    static constexpr int WIDTH  = 640;
    static constexpr int HEIGHT = 480;   // full-screen: no top strip, no right bar

    // The waveform panel, under the three header rows.
    static constexpr int WAVEFORM_Y = 73;
    static constexpr int WAVEFORM_H = 155;
    static constexpr int WAVEFORM_W = 620;

    // Rows 8..19 sit under the waveform.
    static int content_y(int row);

    static const std::vector<std::string>& source_values();    // LEFT / RIGHT / STEREO / MONO
    static const std::vector<std::string>& rate_values();      // HIGH / NORM / LOFI
    // That depth and every standard one below it, highest first. No 12: WAV has none.
    static std::vector<std::string> bit_depth_choices(int source_bits);
    static const std::vector<std::string>& duration_values();  // 4 BAR … 1/32

    // Beats per DURATION index: "1 BAR" = 4, halving each step.
    static double duration_beats(int duration_index);
    static const std::vector<std::string>& fx_types();         // OTT / DUST / DRIVE / EQ / SYNC
    static const std::vector<std::string>& sync_types();       // RPITCH / TSTRETCH
    static const std::vector<std::string>& slice_methods();    // TRANSIENT / DIVIDE / OFF / MANUAL
    static const std::vector<std::string>& ops_row1();         // CROP COPY CUT DUPL PASTE DEL
    static const std::vector<std::string>& ops_row2();         // NORM FADE+ FADE- SLNC REV UNDO

    // `fxValue` means something different under each.
    static constexpr int FX_OTT   = 0;
    static constexpr int FX_DUST  = 1;
    static constexpr int FX_DRIVE = 2;
    static constexpr int FX_EQ    = 3;   // reads `fxEqSlot`, not the amount
    static constexpr int FX_SYNC  = 4;   // fxValue is unused; syncType picks RPITCH vs TSTRETCH

    static constexpr int SYNC_RPITCH   = 0;   // resample: faster is higher
    static constexpr int SYNC_TSTRETCH = 1;   // the pitch held; A on the cell opens its box

    static constexpr int SLICE_TRANSIENT = 0;
    static constexpr int SLICE_DIVIDE    = 1;
    static constexpr int SLICE_OFF       = 2;
    // ⚠️ Appended after OFF: `sliceMethod` survives a session.
    static constexpr int SLICE_MANUAL    = 3;

    // Row 10's second cell: SENS (TRANSIENT) or BY (DIVIDE).
    static constexpr bool slice_has_parameter(int slice_method) {
        return slice_method == SLICE_TRANSIENT || slice_method == SLICE_DIVIDE;
    }

    // Rows 3–7 and the spacers are skipped, and row 11 too with slicing OFF; row 1 and 19 wrap.
    static int row_above(int row, int slice_method = -1);
    static int row_below(int row, int slice_method = -1);
    static int max_col_for_row(int row, int slice_method = SLICE_OFF);


    void draw(Canvas& c, int x, int y, const SampleEditorState& s, const Theme& t) const;

    CursorContext cursor_context(const SampleEditorState& s) const;

    SampleEditorInputResult handle_input(SampleEditorState& s, const InputAction& action) const;

private:
    // The wave, the S/E edges, slice boundaries and the playhead.
    void draw_waveform(Canvas& c, int x, int y, const SampleEditorState& s, const Theme& t) const;

    // B on a modified sample.
    void draw_confirm_dialog(Canvas& c, int x, int y, const Theme& t) const;
};

}  // namespace pt::ui
