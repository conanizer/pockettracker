// The sample editor: its buttons, the selection, the slice markers, the ops, SAVE and CHOP.

#include "ui/dispatch/dispatch_common.h"

#include "ui/std_filesystem.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

namespace {

/** The waveform panel is 620px wide, so it asks the engine for 620 (min, max) pairs. */
constexpr int WAVEFORM_BINS = SampleEditorModule::WAVEFORM_W;

/** SOURCE mode → the channel the waveform is drawn from: 0 = left, 1 = right, 2 = averaged. */
int waveform_channel(int source_mode) {
    return (source_mode == 0) ? 0 : (source_mode == 1) ? 1 : 2;
}

/** A+LEFT/RIGHT nudge by about one visible pixel, A+UP/DOWN by sixteen — both scaled by the zoom. */
int64_t fine_step(const SampleEditorState& se) {
    return std::max<int64_t>(1, static_cast<int64_t>(se.totalFrames) / (256LL << se.zoomLevel));
}
int64_t coarse_step(const SampleEditorState& se) {
    return std::max<int64_t>(1, static_cast<int64_t>(se.totalFrames) / (16LL << se.zoomLevel));
}

}  // namespace

// ─── The buttons ─────────────────────────────────────────────────────────────────────────────────

GestureResult InputDispatcher::sample_editor_screen(Gesture g) {
    SampleEditorState& se = s_.sampleEditor;

    // A+DPAD drags the selection's active edge on rows 3..8, the slice boundary on row 11's POSITION
    // cell; elsewhere it edits the cell like any screen.
    auto nudge = [&](int64_t delta) {
        if (on_sample_selection_row())        nudge_selection_edge(delta);
        else if (on_sample_slice_marker_row()) nudge_slice_marker(delta);
        else return GestureResult::PASS;
        return GestureResult::TAKEN;
    };

    switch (g) {
        // ⚠️ A cell that OPENS something first (the EQ cell), or it would run its FX APPLY instead.
        case Gesture::A:
            if (!open_sub_screen_at_cursor(/*peek=*/false)) sample_editor_confirm();
            return GestureResult::TAKEN;

        // ⚠️ B is BACK, but asks first if anything would be lost: the edits live in the ENGINE, not the
        // project.
        case Gesture::B:
            if (se.isModified) se.showConfirmClose = true;
            else               close_sample_editor();
            return GestureResult::TAKEN;

        case Gesture::START:
            sample_editor_audition();
            return GestureResult::TAKEN;

        case Gesture::A_UP:    return nudge(+coarse_step(se));
        case Gesture::A_DOWN:  return nudge(-coarse_step(se));
        case Gesture::A_LEFT:  return nudge(-fine_step(se));
        case Gesture::A_RIGHT: return nudge(+fine_step(se));

        case Gesture::A_B:
            // The SELECTION row: back to the sample's own bound — START to 0, END to the last frame.
            if (se.cursorRow == 8) {
                if (se.cursorCol == 0)      se.selectionStart = 0;
                else if (se.cursorCol == 1) se.selectionEnd   = se.totalFrames;
                return GestureResult::TAKEN;
            }
            // The SLICE DETAIL row: the boundary back where its method would put it; under MANUAL, gone.
            if (se.cursorRow == 11) {
                reset_slice_marker();
                return GestureResult::TAKEN;
            }
            return GestureResult::PASS;

        // ZOOM: `zoomLevel` 0 = 1× … 4 = 16×; the feed re-bins the waveform.
        case Gesture::R_UP:   se.zoomLevel = std::min(se.zoomLevel + 1, 4); return GestureResult::TAKEN;
        case Gesture::R_DOWN: se.zoomLevel = std::max(se.zoomLevel - 1, 0); return GestureResult::TAKEN;

        // ⚠️ Swallowed: the editor has no cell in the screen grid, so navigating would fall through to
        // PHRASE and bypass ARE YOU SURE?, silently discarding an unsaved edit.
        case Gesture::R_LEFT:
        case Gesture::R_RIGHT:
            return GestureResult::TAKEN;

        // Not the clipboard's or the mix's to clear from here: neither is on this screen.
        case Gesture::L_R:
            return GestureResult::TAKEN;

        default:
            return GestureResult::PASS;
    }
}

GestureResult InputDispatcher::sample_close_layer(Gesture g) {
    SampleEditorState& se = s_.sampleEditor;
    switch (g) {
        case Gesture::A:   // YES — discard and leave
            se.showConfirmClose = false;
            close_sample_editor();
            break;
        case Gesture::B:   // NO — stay
            se.showConfirmClose = false;
            break;
        default:
            break;
    }
    return GestureResult::TAKEN;
}

void InputDispatcher::sample_editor_audition() {
    SampleEditorState& se = s_.sampleEditor;

    // Only while the TRANSPORT IS STOPPED: `playbackPosition` also tracks song voices on this sample.
    if (se.playbackPosition >= 0.0f && !host_.is_playing()) {
        host_.stop_preview();
        se.playbackPosition = -1.0f;
        return;
    }

    // ⚠️ The rapid double-START guard: a pending restore from the PREVIOUS preview must land first, or
    // it would strip this audition's EQ, sends and modulation mid-preview.
    run_due_sample_preview_restore(/*force=*/true);

    samplePending_.previewRestore     = true;
    samplePending_.previewRestoreAtMs = now_ms_ + 100;
    samplePending_.previewRestoreInst = se.instrumentId;

    // The FX row is auditioned by APPLYING it for real and restoring the clean audio after — a
    // destructive chain has no dry/wet path. (EQ always previews; the others need a nonzero amount.)
    const bool hasFxPreview = (se.fxType == SampleEditorModule::FX_EQ) ||
                              (se.fxType <= SampleEditorModule::FX_DRIVE && se.fxValue > 0);
    host_.restore_fx_preview_backup();
    if (hasFxPreview) {
        before_long_operation();
        host_.save_fx_preview_backup(se.instrumentId);
        host_.apply_sample_fx(se.instrumentId, se.fxType, se.fxValue);
    }

    host_.set_preview_track(-1);   // a waveform being edited is not in the arrangement
    host_.preview_sample_editor(se.instrumentId, se.sourceMode, se.selectionStart, se.selectionEnd,
                                se.totalFrames, se.pitchSemitones);
}

// ─── Opening and closing ─────────────────────────────────────────────────────────────────────────

void InputDispatcher::open_sample_editor() {
    const Instrument& ins = s_.project->instruments[static_cast<size_t>(s_.currentInstrument)];
    if (ins.instrumentType == songcore::InstrumentType::SOUNDFONT) return;

    s_.sampleEditor              = SampleEditorState{};   // a fresh session, every time
    s_.sampleEditor.sampleId     = s_.currentInstrument;
    s_.sampleEditor.instrumentId = s_.currentInstrument;
    s_.sampleEditor.cursorRow    = 1;
    s_.sampleEditor.cursorCol    = 0;

    s_.previousScreen = s_.currentScreen;
    s_.currentScreen  = ScreenType::SAMPLE_EDITOR;
    init_sample_editor_state();
}

void InputDispatcher::init_sample_editor_state() {
    SampleEditorState& se  = s_.sampleEditor;
    const Instrument&  ins = s_.project->instruments[static_cast<size_t>(se.instrumentId)];

    // The NAME is the FILE's — it is what you will save back over.
    se.sampleFilePath = ins.sampleFilePath.value_or("");
    se.sampleName     = se.sampleFilePath.empty() ? "" : path_stem(se.sampleFilePath);

    se.totalFrames   = host_.sample_length(se.instrumentId);
    se.sampleRate    = host_.sample_rate_of(se.instrumentId);
    se.hasStereoData = host_.has_stereo_data(se.instrumentId);
    // BIT opens at the sample's own depth, which is also the highest it can offer.
    se.sourceBitDepth = host_.sample_bit_depth(se.instrumentId);
    se.bitDepth       = se.sourceBitDepth;

    // ⚠️ SOURCE opens on STEREO for a stereo sample: every other mode SAVES one channel, so a trim
    // would silently drop the right channel. Mono shows "MONO", read-only.
    se.sourceMode = se.hasStereoData ? 2 /*STEREO*/ : 0;

    se.waveformData  = host_.sample_waveform(se.instrumentId, WAVEFORM_BINS, 0, 0,
                                             waveform_channel(se.sourceMode));

    // The selection opens on the instrument's own window, stored as 0..255 and scaled to frames here.
    if (se.totalFrames > 0) {
        se.selectionStart = (static_cast<int64_t>(ins.sampleStart) * se.totalFrames) / 255;
        se.selectionEnd   = (static_cast<int64_t>(ins.sampleEnd) * se.totalFrames) / 255;
        // ⚠️ An INVERTED window (start > end) runs to the end of the sample, as `derive_sample_window`
        // plays it — so the editor shows what the engine plays.
        if (se.selectionStart >= se.selectionEnd) {
            // A START of 0xFF scales to the last frame; the tail must be at least one frame wide.
            se.selectionStart = std::min<int64_t>(se.selectionStart, se.totalFrames - 1);
            se.selectionEnd   = se.totalFrames;
        }
    } else {
        se.selectionStart = 0;
        se.selectionEnd   = 0;
    }

    // The file's markers come from the PROJECT (read from its `cue ` chunk at load) — no file I/O here.
    //
    // ⚠️ Made SORTED, UNIQUE and strictly INSIDE the sample: nothing upstream promises it, and
    // `slice_bounds` reads neighbours as one slice's edges, so a bad pair is a negative or zero-length
    // slice. This is the only door markers come in through.
    se.fileMarkers.clear();
    se.fileMarkers.reserve(ins.sliceMarkers.size());
    for (const int64_t m : ins.sliceMarkers)
        if (m >= 1 && m < static_cast<int64_t>(se.totalFrames)) se.fileMarkers.push_back(static_cast<int>(m));
    std::sort(se.fileMarkers.begin(), se.fileMarkers.end());
    se.fileMarkers.erase(std::unique(se.fileMarkers.begin(), se.fileMarkers.end()), se.fileMarkers.end());

    // ⚠️ The detector's list opens EMPTY — that is what makes the feed re-detect on the new audio.
    se.transientMarkers.clear();
    // ⚠️ And the hand-placed ones: they are frame indices into audio just replaced.
    se.manualMarkers.clear();
    se.manualKeyMethod = -1;
    se.manualKeyParam  = -1;
    se.sliceIndex = 0;
    // ⚠️ sliceMethod is NOT reset: it survives a re-entry, so loading a second sample keeps TRANSIENT.

    se.isModified       = false;
    se.showConfirmClose = false;
    se.playbackPosition = -1.0f;
}

void InputDispatcher::close_sample_editor() {
    // Pay any pending audition restore now, or it lands on a slot no longer on screen.
    run_due_sample_preview_restore(/*force=*/true);

    host_.restore_fx_preview_backup();          // drop an un-applied FX preview
    host_.free_sample_undo(s_.sampleEditor.instrumentId);   // unreachable once the editor is gone
    host_.clear_previews();                     // the 254/255 scratch slots

    s_.currentScreen = s_.previousScreen;
}

// ─── The selection: A+DPAD on rows 3..8 ──────────────────────────────────────────────────────────

void InputDispatcher::nudge_selection_edge(int64_t delta) {
    SampleEditorState& se = s_.sampleEditor;

    // With no sample `totalFrames` is 0 and the clamps below would have lo > hi (UB). Reachable: EDIT on
    // an empty slot, DOWN, DOWN, A+RIGHT.
    if (se.totalFrames <= 0) return;

    const int64_t maxFrame = se.totalFrames;
    const int     dir      = (delta >= 0) ? 1 : -1;

    // SNAP moves the edge to the nearest zero crossing in the DIRECTION OF TRAVEL (the one just left is
    // always nearest). ⚠️ Through the source mode, so it is a crossing in the signal SAVE writes — on
    // stereo, both channels.
    auto snap = [&](int64_t f) -> int64_t {
        if (!se.snapEnabled) return f;
        return host_.find_zero_crossing(se.instrumentId, static_cast<int>(f), dir, se.sourceMode);
    };

    if (se.cursorCol == 0) {
        // START can never reach END — an inverted selection is not a selection.
        const int64_t raw = std::clamp<int64_t>(se.selectionStart + delta, 0, se.selectionEnd - 1);
        se.selectionStart = std::min<int64_t>(snap(raw), se.selectionEnd - 1);
    } else {
        const int64_t raw = std::clamp<int64_t>(se.selectionEnd + delta, se.selectionStart + 1, maxFrame);
        se.selectionEnd   = std::clamp<int64_t>(snap(raw), se.selectionStart + 1, maxFrame);
    }
}

// ─── The slice markers: A+DPAD and A+B on row 11 ─────────────────────────────────────────────────

namespace {

/**
 * Put boundary `k` at `want` (or MAKE one there when `k` is not in the list) and return its new index;
 * −1 when there is nowhere for it to go.
 *
 * ⭐ A boundary may be dragged past its neighbours; its NUMBER follows its POSITION, re-sorted here.
 * ⚠️ Two boundaries never share a frame (a zero-length slice) — a taken landing is stepped past in the
 * direction of travel. Frame 0 and the last frame are the sample's bounds, never boundaries.
 */
int place_slice_marker(std::vector<SliceMarker>& m, int k, int64_t want, int dir, int totalFrames) {
    const int64_t last = static_cast<int64_t>(totalFrames) - 1;
    if (last < 1) return -1;   // no room for an interior boundary at all

    // ⚠️ `i != k` and not a value compare: the boundary being dragged must not collide with itself.
    const auto taken = [&](int64_t f) {
        for (int i = 0; i < static_cast<int>(m.size()); ++i)
            if (i != k && static_cast<int64_t>(m[static_cast<size_t>(i)].frame) == f) return true;
        return false;
    };

    int64_t at = std::clamp<int64_t>(want, 1, last);
    while (at >= 1 && at <= last && taken(at)) at += dir;
    if (at < 1 || at > last) return -1;   // walked off the end through a wall of boundaries

    // ⚠️ MOVE, never rebuild: `originFrame` is the boundary's identity. A boundary MADE here has none (−1).
    if (k >= 0 && k < static_cast<int>(m.size())) m[static_cast<size_t>(k)].frame = static_cast<int>(at);
    else                                          m.push_back(SliceMarker{static_cast<int>(at), -1});
    std::sort(m.begin(), m.end(),
              [](const SliceMarker& a, const SliceMarker& b) { return a.frame < b.frame; });

    for (int i = 0; i < static_cast<int>(m.size()); ++i)
        if (static_cast<int64_t>(m[static_cast<size_t>(i)].frame) == at) return i;
    return -1;   // unreachable: the frame was just written and the frames are unique
}

}  // namespace

/**
 * Copy whatever the method currently shows, so nudges have something to write into, stamped with its
 * (method, parameter). MANUAL starts from the sample's `cue ` chunk; with none it starts empty.
 */
void InputDispatcher::materialise_manual_markers() {
    SampleEditorState& se = s_.sampleEditor;
    if (se.manual_markers_live()) return;

    // ⚠️ Each copy remembers its origin frame: after a drag past its neighbours the index no longer
    // says which cut it was, and A+B needs to know.
    se.manualMarkers.clear();
    if (se.sliceMethod == SampleEditorModule::SLICE_TRANSIENT) {
        for (const int f : se.transientMarkers) se.manualMarkers.push_back(SliceMarker{f, f});
    } else if (se.sliceMethod == SampleEditorModule::SLICE_DIVIDE) {
        const int div = std::max(se.sliceDivisions, 1);
        for (int i = 1; i < div; ++i) {
            const int f = static_cast<int>((static_cast<int64_t>(i) * se.totalFrames) / div);
            se.manualMarkers.push_back(SliceMarker{f, f});
        }
    } else if (se.sliceMethod == SampleEditorModule::SLICE_MANUAL) {
        // ⚠️ Origin −1: a file cue point has no computed home, so A+B REMOVES it — the same A+B that
        // removes a hand-placed one.
        for (const int f : se.fileMarkers) se.manualMarkers.push_back(SliceMarker{f, -1});
    }
    se.manualKeyMethod = se.sliceMethod;
    se.manualKeyParam  = se.manual_key_param();
}

/**
 * The selection follows the slice the row-11 cursor is on. ⚠️ Every gesture that moves a boundary or
 * the cursor ends here, so START plays the slice's current shape.
 */
void InputDispatcher::select_current_slice() {
    SampleEditorState& se = s_.sampleEditor;
    int64_t start = 0, end = 0;
    se.slice_bounds(se.sliceIndex, start, end);
    se.selectionStart = start;
    se.selectionEnd   = end;
}

void InputDispatcher::nudge_slice_marker(int64_t delta) {
    SampleEditorState& se = s_.sampleEditor;

    // An empty slot reaches this screen; `std::clamp` with lo > hi is UB.
    if (se.totalFrames <= 0 || delta == 0) return;

    const bool manual = se.sliceMethod == SampleEditorModule::SLICE_MANUAL;
    const int  k      = se.slice_marker_index();

    // Slice 00's left edge is the sample's start: nothing to drag. ⭐ Under MANUAL a rightward drag MAKES
    // a boundary there, and the cursor follows it.
    if (k < 0 && (!manual || delta < 0)) return;

    materialise_manual_markers();
    std::vector<SliceMarker>& m = se.manualMarkers;

    // A boundary past the end of the list is MANUAL's next free slot, born on the boundary to its left.
    if (k >= static_cast<int>(m.size()) && !manual) return;
    const int64_t from = (k >= 0 && k < static_cast<int>(m.size()))
                             ? static_cast<int64_t>(m[static_cast<size_t>(k)].frame)
                             : (k < 0 || m.empty() ? 0 : static_cast<int64_t>(m.back().frame));

    const int dir  = (delta > 0) ? 1 : -1;
    int64_t   want = from + delta;
    // SNAP (row 2) applies to boundaries as to selection edges, searching in the direction of travel.
    if (se.snapEnabled)
        want = host_.find_zero_crossing(
            se.instrumentId,
            static_cast<int>(std::clamp<int64_t>(want, 0, static_cast<int64_t>(se.totalFrames) - 1)),
            dir, se.sourceMode);

    const int landed = place_slice_marker(m, k, want, dir, se.totalFrames);
    if (landed < 0) return;

    se.sliceIndex = landed + 1;   // the boundary's own number — marker `j` is the left edge of slice j+1
    select_current_slice();
}

void InputDispatcher::reset_slice_marker() {
    SampleEditorState& se = s_.sampleEditor;

    const int k = se.slice_marker_index();
    if (k < 0) return;   // slice 00's left edge is the sample's own start: no boundary, nothing to undo

    // ⚠️ Materialise first, like the drag and the tap: under MANUAL the boundary may be a cue point
    // nothing has copied yet, and A+B is how it is deleted.
    materialise_manual_markers();
    std::vector<SliceMarker>& m = se.manualMarkers;
    if (k >= static_cast<int>(m.size())) return;      // MANUAL's free slot holds no boundary yet

    // ⭐ The boundary itself says where it goes — never its index, which after a crossing is another cut's.
    const int origin = m[static_cast<size_t>(k)].originFrame;

    if (origin < 0) {
        // Placed by hand or from the file's cue points: nothing to go back to, so A+B REMOVES it and the
        // cursor names the slice that grew into the gap.
        //
        // ⚠️ Emptying the list does NOT retire the stamp: without it the read falls back to the file's
        // cue points, undoing every delete at once.
        m.erase(m.begin() + k);
    } else {
        // ⚠️ Through `place_slice_marker`: a neighbour may have been dragged across that position since.
        const int landed = place_slice_marker(m, k, origin, +1, se.totalFrames);
        if (landed < 0) return;
        se.sliceIndex = landed + 1;
    }

    select_current_slice();
}

/**
 * Cut a boundary at the playhead — "slice it by ear". MANUAL only, and only while the sample sounds.
 *
 * ⭐ The frame is `playbackPosition` — the line the waveform draws — as it stood when A went DOWN: the
 * user taps to the line they see, and by the release the playhead has run on.
 *
 * SNAP searches BACKWARD: a tap has no direction and is always late, so the crossing that matters is
 * before the hit.
 */
void InputDispatcher::tap_slice_marker() {
    SampleEditorState& se = s_.sampleEditor;

    // Consumed: a press that ended in an A+DPAD must not leave a snapshot for the next tap.
    const float at                  = samplePending_.sliceTapPlayhead;
    samplePending_.sliceTapPlayhead = -1.0f;

    if (se.sliceMethod != SampleEditorModule::SLICE_MANUAL) return;
    if (se.totalFrames <= 0 || at < 0.0f) return;

    const int64_t last = static_cast<int64_t>(se.totalFrames) - 1;
    int64_t       want = std::clamp<int64_t>(
        static_cast<int64_t>(at * static_cast<float>(se.totalFrames)), 0, last);
    if (se.snapEnabled)
        want = host_.find_zero_crossing(se.instrumentId, static_cast<int>(want), -1, se.sourceMode);

    materialise_manual_markers();
    std::vector<SliceMarker>& m = se.manualMarkers;

    // ⚠️ Past the end of the list on purpose: a tap always MAKES a boundary (origin −1, so A+B removes it).
    const int landed = place_slice_marker(m, static_cast<int>(m.size()), want, +1, se.totalFrames);
    if (landed < 0) return;

    se.sliceIndex = landed + 1;   // the cursor follows the cut onto the slice it opens
    select_current_slice();
}

// ─── RATE and BIT: the two cells that rebuild the audio ──────────────────────────────────────────

void InputDispatcher::apply_sample_rate_and_bits() {
    before_long_operation();
    SampleEditorState& se     = s_.sampleEditor;
    const int          factor = (se.rateMode == 1) ? 2 : (se.rateMode == 2) ? 4 : 1;
    const int          oldLen = se.totalFrames;

    // ⚠️ Notes already scheduled keep the OLD base frequency — up to LONG_OPERATION_MS of them, filled
    // just above.
    host_.apply_rate_and_bits(se.instrumentId, factor, se.bitDepth);

    const int newLen = host_.sample_length(se.instrumentId);
    auto scale = [&](int64_t f) -> int64_t {
        if (oldLen <= 0) return 0;
        return std::clamp<int64_t>((f * newLen) / oldLen, 0, newLen);
    };
    se.selectionStart = scale(se.selectionStart);
    se.selectionEnd   = scale(se.selectionEnd);
    se.slicePosition  = scale(se.slicePosition);

    se.totalFrames = newLen;
    se.sampleRate  = host_.sample_rate_of(se.instrumentId);
    se.waveformData = host_.sample_waveform(se.instrumentId, WAVEFORM_BINS, 0, 0,
                                            waveform_channel(se.sourceMode));
    se.isModified = true;
}

// ─── The view, after an op ───────────────────────────────────────────────────────────────────────

void InputDispatcher::refresh_sample_view(bool reset_selection) {
    SampleEditorState& se     = s_.sampleEditor;
    const int          newLen = host_.sample_length(se.instrumentId);
    se.totalFrames = newLen;

    // ⚠️ RESET, not clamp: after an op that shortens the sample, a clamped selection would be one the
    // user never made. The whole result is always true.
    if (reset_selection) {
        se.selectionStart = 0;
        se.selectionEnd   = newLen;

        // ⚠️ And the instrument's own START/END and LOOP cells: they are 0-255 FRACTIONS of the buffer,
        // so a length change re-aims them at audio the user never chose. The ops reset the engine's
        // copy; this is the PROJECT's, or the next push puts the stale fraction back.
        Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(se.instrumentId)];
        if (ins.sampleStart != 0x00 || ins.sampleEnd != 0xFF ||
            ins.loopStart   != 0x00 || ins.loopEnd   != 0xFF) {
            ins.sampleStart = 0x00;
            ins.sampleEnd   = 0xFF;
            ins.loopStart   = 0x00;
            ins.loopEnd     = 0xFF;
            host_.push_instrument(se.instrumentId);
            mark_modified();
        }

        // ⚠️⚠️ And every marker list: a boundary is a frame index into audio just replaced, and a stale
        // line over a waveform looks exactly like a right one. All three — `manualMarkers` overrides the
        // others while live, and an empty `transientMarkers` makes the feed re-detect. This also keeps a
        // save after a resize from writing back the surviving half of the old cue points.
        se.fileMarkers.clear();
        se.transientMarkers.clear();
        se.manualMarkers.clear();
        se.manualKeyMethod = -1;
        se.manualKeyParam  = -1;
        se.sliceIndex      = 0;   // the ceiling has just collapsed to a single slice, the whole sample
    }

    se.waveformData = host_.sample_waveform(se.instrumentId, WAVEFORM_BINS,
                                            static_cast<int>(se.view_start()),
                                            static_cast<int>(se.view_end()),
                                            waveform_channel(se.sourceMode));
}

// ─── A on the op rows, the FX row, the name and the save buttons ─────────────────────────────────

void InputDispatcher::sample_editor_confirm() {
    before_long_operation();
    SampleEditorState& se     = s_.sampleEditor;
    const int          instId = se.instrumentId;
    const int          startF = static_cast<int>(se.selectionStart);
    const int          endF   = static_cast<int>(se.selectionEnd);

    // Every destructive op drops any un-applied FX preview (so it acts on the clean audio) and takes
    // an undo backup.
    auto begin_destructive = [&] {
        host_.restore_fx_preview_backup();
        host_.backup_sample(instId);
    };
    auto in_place = [&](auto&& op) {   // an op that does NOT change the length
        begin_destructive();
        op();
        refresh_sample_view(/*reset_selection=*/false);
        se.isModified = true;
    };
    auto resizing = [&](auto&& op) {   // an op that DOES
        begin_destructive();
        op();
        refresh_sample_view(/*reset_selection=*/true);
        se.isModified = true;
    };

    switch (se.cursorRow) {
        // ── Row 11: cut a boundary at the playhead, mid-audition ─────────────────────────────────
        //
        // Both columns. It arrives on A's RELEASE (`defer_a_to_release`) and cuts at the playhead as it
        // stood on the PRESS — the row's other gestures start with the same A held. Not destructive:
        // a boundary is editor state, so no undo backup.
        case 11:
            tap_slice_marker();
            break;

        // ── Row 13: CROP  COPY  CUT  DUPL  PASTE  DEL ────────────────────────────────────────────
        case 13:
            switch (se.cursorCol) {
                case 0:   // CROP — keep the selection, discard the rest
                    if (startF < endF) resizing([&] { host_.crop_sample(instId, startF, endF); });
                    break;
                case 1:   // COPY — the ONE op on this row that changes nothing, so it takes no backup
                    host_.copy_region(instId, startF, endF);
                    break;
                case 2:   // CUT = copy, then delete
                    if (startF < endF) resizing([&] {
                        host_.copy_region(instId, startF, endF);
                        host_.delete_sample_region(instId, startF, endF);
                    });
                    break;
                case 3:   // DUPL = copy the selection and paste it at the END
                    if (startF < endF) resizing([&] {
                        host_.copy_region(instId, startF, endF);
                        host_.paste_region(instId, se.totalFrames);
                    });
                    break;
                case 4:   // PASTE — inserts at the selection's START
                    if (host_.clipboard_length() > 0)
                        resizing([&] { host_.paste_region(instId, startF); });
                    break;
                case 5:   // DEL
                    if (startF < endF)
                        resizing([&] { host_.delete_sample_region(instId, startF, endF); });
                    break;
                default: break;
            }
            break;

        // ── Row 14: NORM  FADE+  FADE-  SLNC  REV  UNDO ──────────────────────────────────────────
        case 14:
            switch (se.cursorCol) {
                case 0: in_place([&] { host_.normalize_sample(instId, startF, endF); }); break;
                case 1: in_place([&] { host_.fade_in_sample(instId, startF, endF); }); break;
                case 2: in_place([&] { host_.fade_out_sample(instId, startF, endF); }); break;
                case 3: in_place([&] { host_.silence_region(instId, startF, endF); }); break;
                case 4: in_place([&] { host_.reverse_sample(instId, startF, endF); }); break;

                case 5: {   // UNDO — one level, and it may restore a DIFFERENT length
                    host_.restore_fx_preview_backup();
                    host_.undo_sample(instId);
                    refresh_sample_view(/*reset_selection=*/true);
                    // ⚠️ `isModified` is NOT cleared: one undo is one step back, not back to the file.
                    se.slicePosition = std::clamp<int64_t>(se.slicePosition, 0, se.totalFrames);
                    break;
                }
                default: break;
            }
            break;

        // ── Row 16: the FX row. Col 2 is APPLY; the other two are dialled with A+DPAD. ───────────
        case 16: {
            if (se.cursorCol != 2) break;

            if (se.fxType <= SampleEditorModule::FX_EQ) {
                // OTT / DUST / DRIVE need an AMOUNT; EQ's value is a slot, so it always applies.
                const bool worth_doing =
                    (se.fxValue > 0) || (se.fxType == SampleEditorModule::FX_EQ);
                if (!worth_doing) break;

                begin_destructive();
                host_.apply_sample_fx(instId, se.fxType, se.fxValue);
                refresh_sample_view(/*reset_selection=*/false);
                se.isModified = true;
                break;
            }

            // ── SYNC: fit the sample to the project's grid ───────────────────────────────────────
            //
            // RPITCH resamples (faster is higher — for breakbeats); TSTRETCH holds the pitch (SOLA).
            const int    bpm     = s_.project->tempo;
            const double rawSecs = (se.sampleRate > 0)
                                       ? static_cast<double>(se.totalFrames) / se.sampleRate
                                       : 0.0;
            if (rawSecs <= 0.0 || bpm <= 0) break;

            const double targetSecs = SampleEditorModule::duration_beats(se.durationIndex) * 60.0 / bpm;
            const int    oldLen     = se.totalFrames;

            auto rescale_after = [&](bool clear_pitch) {
                const int newLen = host_.sample_length(instId);
                auto scale = [&](int64_t f) -> int64_t {
                    if (oldLen <= 0) return 0;
                    return std::clamp<int64_t>((f * newLen) / oldLen, 0, newLen);
                };
                se.slicePosition = scale(se.slicePosition);
                if (clear_pitch) se.pitchSemitones = 0;
                // ⚠️ Both resamplers drop RATE: the result is the new original, and a stale RATE would
                // decimate it twice. BIT is kept — it is also the depth SAVE writes.
                se.rateMode     = 0;
                refresh_sample_view(/*reset_selection=*/true);
                se.isModified = true;
            };

    // ⚠️ "Already on the grid" is asked in FRAMES by both branches; a ratio window of ±0.1 % is most of
    // a tick on a 4-bar loop.
            const double  ratio      = targetSecs / rawSecs;
            const int64_t wantFrames = std::llround(static_cast<double>(se.totalFrames) * ratio);
            if (ratio <= 0.001 || wantFrames == se.totalFrames) break;

            if (se.syncType == 0) {   // RPITCH
                // ⚠️ Fractional: rounding to whole semitones (a 5.9 % length step) can miss a 4-bar
                // loop by ~230 ms.
                const double exact     = 12.0 * std::log(rawSecs / targetSecs) / std::log(2.0);
                const float  semitones = static_cast<float>(std::clamp(exact, -24.0, 24.0));
                begin_destructive();
                host_.pitch_shift_sample(instId, semitones);
                // The shift is baked, so the pending one on row 2 is spent.
                rescale_after(/*clear_pitch=*/true);
            } else {                  // TSTRETCH
                begin_destructive();
                host_.time_stretch_sample(instId, static_cast<float>(ratio));
                rescale_after(/*clear_pitch=*/false);   // a stretch does not change the pitch
            }
            break;
        }

        // ── Row 18: NAME ────────────────────────────────────────────────────────────────────────
        case 18:
            open_qwerty(QwertyContext::SAMPLE_NAME, se.sampleName, "SAMPLE NAME:",
                        fs_.samples_directory());
            break;

        // ── Row 19: LOAD  SAVE  OVERWRITE  CHOP ─────────────────────────────────────────────────
        case 19:
            switch (se.cursorCol) {
                case 0: {   // LOAD — a different sample, into the slot the editor is already open on
                    // ⚠️ Keep `previousScreen` as the EDITOR's return target, or B on the editor would go
                    // back to the browser.
                    const ScreenType keep = s_.previousScreen;
                    open_file_browser(AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR,
                                      browser_dir(BrowserDir::SAMPLES), {"wav"});
                    s_.previousScreen = keep;
                    break;
                }

                case 1: {   // SAVE — to <name>.wav, or ask for a name if that one is taken
                    const std::string base = se.sampleName.empty() ? "SAMPLE" : se.sampleName;
                    const std::string dir  = fs_.samples_directory();
                    bake_pending_pitch();

                    const std::string target = dir + "/" + base + ".wav";
                    if (!fs_.file_exists(target)) {
                        save_sample_to(target, /*adopt_name=*/true);
                        break;
                    }
                    // Taken: suggest the next free `<base>_0001` — SAVE is not OVERWRITE.
                    std::string suggested = base;
                    for (int n = 1; fs_.file_exists(dir + "/" + suggested + ".wav"); ++n) {
                        char suffix[16];   // "_%04d" of an int can be 12 bytes (-Wformat-truncation)
                        std::snprintf(suffix, sizeof(suffix), "_%04d", n);
                        suggested = base + suffix;
                    }
                    open_qwerty(QwertyContext::SAMPLE_SAVE, suggested, "SAVE AS:", dir,
                                /*max_length=*/24, /*clear_on_first_b=*/true);
                    break;
                }

                case 2:   // OVERWRITE — back over the file it came from. Nothing to do if it has none.
                    if (!se.sampleFilePath.empty()) {
                        bake_pending_pitch();
                        save_sample_to(se.sampleFilePath, /*adopt_name=*/false);
                    }
                    break;

                case 3:   // CHOP
                    sample_editor_chop();
                    break;

                default: break;
            }
            break;

        default:
            break;
    }
}

// ─── The pending pitch shift ─────────────────────────────────────────────────────────────────────

void InputDispatcher::bake_pending_pitch() {
    SampleEditorState& se = s_.sampleEditor;
    if (se.pitchSemitones == 0) return;

    const int oldLen = se.totalFrames;
    host_.pitch_shift_sample(se.instrumentId, static_cast<float>(se.pitchSemitones));
    const int newLen = host_.sample_length(se.instrumentId);

    auto scale = [&](int64_t f) -> int64_t {
        if (oldLen <= 0) return 0;
        return std::clamp<int64_t>((f * newLen) / oldLen, 0, newLen);
    };

    // Every frame-measured thing moves with the audio. The selection is scaled, not reset: the user
    // asked to SAVE, not to change anything.
    se.selectionStart = scale(se.selectionStart);
    se.selectionEnd   = scale(se.selectionEnd);
    se.slicePosition  = scale(se.slicePosition);

    se.totalFrames    = newLen;
    se.pitchSemitones = 0;   // spent
    se.rateMode       = 0;   // the shifted buffer IS the new original — see sample_edit.h
    // ⚠️ BIT is NOT reset. This runs on the way INTO a save, and BIT is the depth that save writes.
    se.waveformData   = host_.sample_waveform(se.instrumentId, WAVEFORM_BINS, 0, 0,
                                              waveform_channel(se.sourceMode));

    // The instrument's playback params were derived from the old buffer's length.
    host_.push_instrument(se.instrumentId);
    mark_modified();
}

// ─── The slices ──────────────────────────────────────────────────────────────────────────────────

std::vector<int> InputDispatcher::compute_slice_cue_points() const {
    const SampleEditorState& se = s_.sampleEditor;

    // DIVIDE only while it is still arithmetic — once a boundary is dragged, it is a list like any other.
    if (se.sliceMethod == SampleEditorModule::SLICE_DIVIDE && se.marker_count() == 0) {
        const int div = std::max(se.sliceDivisions, 1);
        std::vector<int> cues;
        cues.reserve(static_cast<size_t>(std::max(div - 1, 0)));
        for (int i = 1; i < div; ++i)
            cues.push_back(static_cast<int>((static_cast<int64_t>(i) * se.totalFrames) / div));
        return cues;
    }

    // Whichever list the method answers with: the detector's under TRANSIENT, the hand-placed one under
    // MANUAL (seeded from the file's, so a MANUAL save keeps the original boundaries unless moved), and
    // under OFF the file's own — ⚠️ a save with slicing OFF can neither add nor drop a slice, even after
    // a detour through TRANSIENT left markers behind.
    //
    // Frame 0 and the end frame are dropped: a cue point at 0 gives every reader a zero-length slice.
    std::vector<int> cues;
    for (int i = 0; i < se.marker_count(); ++i) {
        const int m = static_cast<int>(se.marker_position(i));
        if (m > 0 && m < se.totalFrames) cues.push_back(m);
    }
    return cues;
}

std::vector<std::pair<int64_t, int64_t>> InputDispatcher::current_slices() const {
    const SampleEditorState& se = s_.sampleEditor;

    // N markers → N+1 slices. OFF, TRANSIENT before detection, and MANUAL with no boundaries are one
    // slice — the whole sample.
    const int markers = se.marker_count();
    const int count   = (se.sliceMethod == SampleEditorModule::SLICE_OFF)
                            ? 0
                            : (markers > 0)
                                  ? markers + 1
                                  : (se.sliceMethod == SampleEditorModule::SLICE_DIVIDE)
                                        ? std::max(se.sliceDivisions, 1)
                                        : 1;

    std::vector<std::pair<int64_t, int64_t>> out;
    out.reserve(static_cast<size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i) {
        int64_t start = 0, end = 0;
        se.slice_bounds(i, start, end);
        out.emplace_back(start, end);
    }
    return out;
}

// ─── SAVE ────────────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::save_sample_to(const std::string& path, bool adopt_name) {
    before_long_operation();
    SampleEditorState& se   = s_.sampleEditor;
    const std::vector<int> cues = compute_slice_cue_points();

    if (!host_.save_sample_wav(se.instrumentId, path, cues, se.sourceMode, se.hasStereoData,
                               se.bitDepth)) {
        s_.statusMessage = "SAVE FAILED";
        s_.statusSuccess = false;
        return;
    }
    host_.adopt_saved_sample(se.instrumentId, se.bitDepth);

    // ⚠️ A MONO save is reloaded from the file just written: the editor's buffer may still be stereo
    // (SOURCE=LEFT), and the slot must match the file. A stereo save already matches.
    const bool wrote_mono = !(se.hasStereoData && se.sourceMode == 2);
    if (wrote_mono) host_.load_sample(se.instrumentId, path);

    Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(se.instrumentId)];
    ins.sampleFilePath = path;
    // The markers go into the PROJECT too — the .ptp is what a reload reads first.
    ins.sliceMarkers.clear();
    ins.sliceMarkers.reserve(cues.size());
    for (const int c : cues) ins.sliceMarkers.push_back(static_cast<int64_t>(c));

    se.sampleFilePath = path;
    if (adopt_name) se.sampleName = path_stem(path);
    se.isModified    = false;
    se.hasStereoData = host_.has_stereo_data(se.instrumentId);

    mark_modified();
    s_.currentScreen = s_.previousScreen;   // a save LEAVES the editor, as it does on Android
}

// ─── CHOP ────────────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::sample_editor_chop() {
    SampleEditorState& se = s_.sampleEditor;
    if (se.sliceMethod == SampleEditorModule::SLICE_OFF) return;

    const std::vector<std::pair<int64_t, int64_t>> slices = current_slices();
    if (slices.empty()) return;

    // A slice becomes a FILE NAME, so anything a filesystem would choke on goes.
    std::string base = se.sampleName.empty() ? "SAMPLE" : se.sampleName;
    for (char& c : base) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!safe) c = '_';
    }

    // Samples/Chops/<base>/ — its own folder, so a 32-slice break does not bury the sample directory.
    const std::string samples = fs_.samples_directory();
    fs_.create_folder(samples, "Chops");                     // "" if it already exists — either is fine
    const std::string chops = samples + "/Chops";
    fs_.create_folder(chops, base);
    const std::string dir = chops + "/" + base;

    const int written = host_.chop_sample(se.instrumentId, dir, base, slices, se.bitDepth);
    s_.statusMessage   = written > 0 ? ("CHOPPED " + std::to_string(written)) : "CHOP FAILED";
    s_.statusSuccess   = written > 0;
}

// ─── The audition's deferred restore ─────────────────────────────────────────────────────────────

void InputDispatcher::run_due_sample_preview_restore(bool force) {
    if (!samplePending_.previewRestore) return;
    if (!force && now_ms_ < samplePending_.previewRestoreAtMs) return;

    samplePending_.previewRestore = false;
    host_.finish_sample_preview(samplePending_.previewRestoreInst);
}

// ─── The cursor and the edit ─────────────────────────────────────────────────────────────────────

CursorContext InputDispatcher::sample_editor_context() const {
    return sample_.cursor_context(s_.sampleEditor);
}

bool InputDispatcher::sample_editor_edit(const InputAction& action) {
    const SampleEditorInputResult r = sample_.handle_input(s_.sampleEditor, action);
    if (r.rateModeChanged || r.bitDepthChanged) apply_sample_rate_and_bits();

    // ⚠️ `false`: the editor's session (zoom, selection, slice index, pending pitch) is not the
    // document. `true` would dirty the song and arm an autosave on a held A+UP on ZOOM.
    // RATE and BIT, which DO rebuild the buffer, push for themselves
    // (`apply_sample_rate_and_bits()`).
    return false;
}

}  // namespace pt::ui
