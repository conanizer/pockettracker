// The sample editor: its buttons, the selection, the slice markers, the ops, SAVE and CHOP.

#include "ui/dispatch/dispatch_common.h"

#include "ui/std_filesystem.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

namespace {

/** The waveform panel's width: one (min, max) pair per pixel. */
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

/** The selection and the slice position, moved with audio that went from `oldLen` frames to `newLen`. */
void rescale_positions(SampleEditorState& se, int oldLen, int newLen) {
    auto scale = [&](int64_t f) -> int64_t {
        return oldLen > 0 ? std::clamp<int64_t>((f * newLen) / oldLen, 0, newLen) : 0;
    };
    se.selectionStart = scale(se.selectionStart);
    se.selectionEnd   = scale(se.selectionEnd);
    se.slicePosition  = scale(se.slicePosition);
}

/** DIVIDE's boundaries: `divisions` equal slices. */
std::vector<int> divide_points(int totalFrames, int divisions) {
    const int        div = std::max(divisions, 1);
    std::vector<int> out;
    for (int i = 1; i < div; ++i)
        out.push_back(static_cast<int>((static_cast<int64_t>(i) * totalFrames) / div));
    return out;
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
        // ⚠️ A cell that opens something (EQ) goes first, or its FX APPLY would run.
        case Gesture::A:
            if (!open_sub_screen_at_cursor(/*peek=*/false)) sample_editor_confirm();
            return GestureResult::TAKEN;

        // ⚠️ B asks first if anything would be lost: the edits live in the engine, not the project.
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
            // The TSTRETCH cell: everything its box dials, back as a session opens.
            if (se.cursorRow == 16 && se.cursorCol == 1 && se.fxType == SampleEditorModule::FX_SYNC &&
                se.syncType == SampleEditorModule::SYNC_TSTRETCH) {
                se.reset_stretch();
                return GestureResult::TAKEN;
            }
            return GestureResult::PASS;

        // ZOOM: `zoomLevel` 0 = 1× … 4 = 16×; the feed re-bins the waveform.
        case Gesture::R_UP:   se.zoomLevel = std::min(se.zoomLevel + 1, 4); return GestureResult::TAKEN;
        case Gesture::R_DOWN: se.zoomLevel = std::max(se.zoomLevel - 1, 0); return GestureResult::TAKEN;

        // ⚠️ Swallowed: navigating would bypass ARE YOU SURE? and discard an unsaved edit.
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

    // ⚠️ A pending restore from the previous preview lands first, or it strips this one's EQ and sends.
    run_due_sample_preview_restore(/*force=*/true);

    samplePending_.previewRestore     = true;
    samplePending_.previewRestoreAtMs = now_ms_ + 100;
    samplePending_.previewRestoreInst = se.instrumentId;

    // The FX row is auditioned by applying it, and the clean audio restored after.
    host_.restore_fx_preview_backup();
    if (se.fx_armed()) {
        before_long_operation();
        host_.save_fx_preview_backup(se.instrumentId);
        host_.apply_sample_fx(se.instrumentId, se.fxType, se.fx_param());
    }

    // SYNC is auditioned from a copy (it changes the length), taking the pending PITCH with it.
    const songcore::SampleSync sync = planned_sample_sync();
    const bool synced = (sync.kind != songcore::SampleSync::Kind::NONE);
    if (synced) before_long_operation();
    const int pitch = synced ? 0 : se.pitchSemitones;

    host_.set_preview_track(-1);   // a waveform being edited is not in the arrangement
    host_.preview_sample_editor(se.instrumentId, se.sourceMode, se.selectionStart, se.selectionEnd,
                                se.totalFrames, pitch, sync);
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

    // ⚠️ SOURCE opens on STEREO for a stereo sample: any other mode saves one channel.
    se.sourceMode = se.hasStereoData ? 2 /*STEREO*/ : 0;

    se.waveformData  = host_.sample_waveform(se.instrumentId, WAVEFORM_BINS, 0, 0,
                                             waveform_channel(se.sourceMode));

    // The selection opens on the instrument's own window, stored as 0..255 and scaled to frames here.
    if (se.totalFrames > 0) {
        se.selectionStart = (static_cast<int64_t>(ins.sampleStart) * se.totalFrames) / 255;
        se.selectionEnd   = (static_cast<int64_t>(ins.sampleEnd) * se.totalFrames) / 255;
        // ⚠️ An inverted window runs to the end, as the engine plays it.
        if (se.selectionStart >= se.selectionEnd) {
            // A START of 0xFF scales to the last frame; the tail must be at least one frame wide.
            se.selectionStart = std::min<int64_t>(se.selectionStart, se.totalFrames - 1);
            se.selectionEnd   = se.totalFrames;
        }
    } else {
        se.selectionStart = 0;
        se.selectionEnd   = 0;
    }

    // The file's markers, from the project. ⚠️ Made sorted, unique and strictly inside the sample — the
    // only door they come in through, and slice_bounds reads neighbours as one slice's edges.
    se.fileMarkers.clear();
    se.fileMarkers.reserve(ins.sliceMarkers.size());
    for (const int64_t m : ins.sliceMarkers)
        if (m >= 1 && m < static_cast<int64_t>(se.totalFrames)) se.fileMarkers.push_back(static_cast<int>(m));
    std::sort(se.fileMarkers.begin(), se.fileMarkers.end());
    se.fileMarkers.erase(std::unique(se.fileMarkers.begin(), se.fileMarkers.end()), se.fileMarkers.end());

    // ⚠️ Both other lists open empty: an empty detector list re-detects, and hand-placed frames index
    // the old audio.
    se.transientMarkers.clear();
    se.manualMarkers.clear();
    se.manualKeyMethod = -1;
    se.manualKeyParam  = -1;
    se.sliceIndex = 0;
    // ⚠️ sliceMethod survives, so a second sample keeps TRANSIENT.

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

    // An empty slot reaches this (EDIT, DOWN, DOWN, A+RIGHT), and the clamps would have lo > hi.
    if (se.totalFrames <= 0) return;

    const int64_t maxFrame = se.totalFrames;
    const int     dir      = (delta >= 0) ? 1 : -1;

    // SNAP: the nearest zero crossing in the direction of travel, ⚠️ in the signal SAVE writes.
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
 * Put boundary `k` at `want` (or make one when `k` is past the list) and return its new index, −1 when
 * there is no room. Its number follows its position. ⚠️ Two never share a frame: a taken landing is
 * stepped past. Frame 0 and the last frame are the bounds, never boundaries.
 */
int place_slice_marker(std::vector<SliceMarker>& m, int k, int64_t want, int dir, int totalFrames) {
    const int64_t last = static_cast<int64_t>(totalFrames) - 1;
    if (last < 1) return -1;   // no room for an interior boundary at all

    // ⚠️ By index: the dragged boundary must not collide with itself.
    const auto taken = [&](int64_t f) {
        for (int i = 0; i < static_cast<int>(m.size()); ++i)
            if (i != k && static_cast<int64_t>(m[static_cast<size_t>(i)].frame) == f) return true;
        return false;
    };

    int64_t at = std::clamp<int64_t>(want, 1, last);
    while (at >= 1 && at <= last && taken(at)) at += dir;
    if (at < 1 || at > last) return -1;   // walked off the end through a wall of boundaries

    // ⚠️ Move, never rebuild: originFrame is the identity. A new one has none (−1).
    if (k >= 0 && k < static_cast<int>(m.size())) m[static_cast<size_t>(k)].frame = static_cast<int>(at);
    else                                          m.push_back(SliceMarker{static_cast<int>(at), -1});
    std::sort(m.begin(), m.end(),
              [](const SliceMarker& a, const SliceMarker& b) { return a.frame < b.frame; });

    for (int i = 0; i < static_cast<int>(m.size()); ++i)
        if (static_cast<int64_t>(m[static_cast<size_t>(i)].frame) == at) return i;
    return -1;   // unreachable: the frame was just written and the frames are unique
}

}  // namespace

/** Copy what the method shows into the editable list, stamped with its (method, parameter). */
void InputDispatcher::materialise_manual_markers() {
    SampleEditorState& se = s_.sampleEditor;
    if (se.manual_markers_live()) return;

    // ⚠️ Each copy keeps its origin: after a drag the index no longer says which cut it was.
    se.manualMarkers.clear();
    if (se.sliceMethod == SampleEditorModule::SLICE_TRANSIENT) {
        for (const int f : se.transientMarkers) se.manualMarkers.push_back(SliceMarker{f, f});
    } else if (se.sliceMethod == SampleEditorModule::SLICE_DIVIDE) {
        for (const int f : divide_points(se.totalFrames, se.sliceDivisions))
            se.manualMarkers.push_back(SliceMarker{f, f});
    } else if (se.sliceMethod == SampleEditorModule::SLICE_MANUAL) {
        // ⚠️ Origin −1: a file cue point has no computed home, so A+B removes it.
        for (const int f : se.fileMarkers) se.manualMarkers.push_back(SliceMarker{f, -1});
    }
    se.manualKeyMethod = se.sliceMethod;
    se.manualKeyParam  = se.manual_key_param();
}

/** The selection follows row 11's slice. ⚠️ Every boundary or cursor move ends here, so START plays
 *  the slice as it now is. */
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

    // Slice 00's left edge is the sample's start. Under MANUAL a rightward drag makes a boundary there.
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

    // ⚠️ Materialise first: under MANUAL it may be a cue point not yet copied.
    materialise_manual_markers();
    std::vector<SliceMarker>& m = se.manualMarkers;
    if (k >= static_cast<int>(m.size())) return;      // MANUAL's free slot holds no boundary yet

    // Its own origin, never its index (after a crossing, another cut's).
    const int origin = m[static_cast<size_t>(k)].originFrame;

    if (origin < 0) {
        // Placed by hand or from the file: A+B removes it. ⚠️ An empty list keeps its stamp, or the read
        // falls back to the file's cue points and undoes every delete.
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
 * Cut a boundary at the playhead — MANUAL only, while the sample sounds. The frame is the playhead as A
 * went down (the line the user tapped to); SNAP searches backward, since a tap is always late.
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

    // ⚠️ Notes already scheduled (up to LONG_OPERATION_MS) keep the old base frequency.
    host_.apply_rate_and_bits(se.instrumentId, factor, se.bitDepth);

    const int newLen = host_.sample_length(se.instrumentId);
    rescale_positions(se, oldLen, newLen);
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

    // ⚠️ Reset, not clamp: a clamped selection after a shortening op is one the user never made.
    if (reset_selection) {
        se.selectionStart = 0;
        se.selectionEnd   = newLen;

        // ⚠️ And the instrument's START/END and LOOP: 0-255 fractions of the buffer, re-aimed by a
        // length change. The ops reset the engine's copy; this is the project's.
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

        // ⚠️ And every marker list: they index audio just replaced, and a stale line looks right. This
        // also keeps a save after a resize from writing back half the old cue points.
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
    // ⚠️ First: an auditioned effect has processed the sample itself, and SAVE or COPY would take it.
    // Not on row 11, whose tap must not stop the audition it is tapped against.
    if (se.cursorRow != 11) host_.restore_fx_preview_backup();
    const int          instId = se.instrumentId;
    const int          startF = static_cast<int>(se.selectionStart);
    const int          endF   = static_cast<int>(se.selectionEnd);

    auto begin_destructive = [&] { host_.backup_sample(instId); };
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
        // On A's release (defer_a_to_release), at the playhead of the press. Editor state: no backup.
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
                if (!se.fx_armed()) break;

                begin_destructive();
                host_.apply_sample_fx(instId, se.fxType, se.fx_param());
                refresh_sample_view(/*reset_selection=*/false);
                se.isModified = true;
                // Spent — left in the cell, START would audition it over its own result.
                if (se.fxType == SampleEditorModule::FX_EQ) se.fxEqSlot = -1;
                else                                        se.fxValue  = 0;
                break;
            }

            apply_sample_sync();
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
                    // ⚠️ Keep previousScreen as the editor's return target.
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

                    // Taken: suggest the next free name — SAVE is not OVERWRITE.
                    const std::string stem = free_wav_stem(fs_, dir, base);
                    if (stem == base)
                        save_sample_to(dir + "/" + base + ".wav", /*adopt_name=*/true);
                    else
                        open_qwerty(QwertyContext::SAMPLE_SAVE, stem, "SAVE AS:", dir,
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

// ─── SYNC: fit the sample to the grid ────────────────────────────────────────────────────────────

songcore::SampleSync InputDispatcher::planned_sample_sync() const {
    const SampleEditorState& se = s_.sampleEditor;
    if (se.fxType != SampleEditorModule::FX_SYNC) return {};

    // RPITCH resamples to the song's tempo (faster is higher); TSTRETCH holds the pitch and fits its
    // box's BPM.
    const bool   stretch = (se.syncType == SampleEditorModule::SYNC_TSTRETCH);
    const int    bpm     = stretch ? se.stretch_bpm(s_.project->tempo) : s_.project->tempo;
    const double rawSecs = (se.sampleRate > 0)
                               ? static_cast<double>(se.totalFrames) / se.sampleRate
                               : 0.0;
    if (rawSecs <= 0.0 || bpm <= 0) return {};

    const double targetSecs = SampleEditorModule::duration_beats(se.durationIndex) * 60.0 / bpm;

    // ⚠️ "Already on the grid" is asked in frames: ±0.1 % is most of a tick on a 4-bar loop.
    const double  ratio      = targetSecs / rawSecs;
    const int64_t wantFrames = std::llround(static_cast<double>(se.totalFrames) * ratio);
    if (ratio <= 0.001 || wantFrames < 1) return {};
    const bool fits = (wantFrames == se.totalFrames);

    songcore::SampleSync sync;
    if (stretch) {
        // The pending PITCH is part of a stretch, so a sample that fits can still have work to do.
        if (fits && se.pitchSemitones == 0) return {};
        sync.kind      = songcore::SampleSync::Kind::STRETCH;
        sync.frames    = static_cast<int>(wantFrames);
        sync.semitones = static_cast<float>(se.pitchSemitones);
        sync.grainMs   = static_cast<float>(se.stretchGrainMs);
    } else {
        if (fits) return {};
        // ⚠️ Fractional: whole semitones can miss a 4-bar loop by ~230 ms.
        const double exact = 12.0 * std::log(rawSecs / targetSecs) / std::log(2.0);
        sync.kind      = songcore::SampleSync::Kind::REPITCH;
        sync.semitones = static_cast<float>(std::clamp(exact, -24.0, 24.0));
    }
    return sync;
}

void InputDispatcher::apply_sample_sync() {
    SampleEditorState&         se   = s_.sampleEditor;
    const songcore::SampleSync sync = planned_sample_sync();
    if (sync.kind == songcore::SampleSync::Kind::NONE) return;

    const int instId = se.instrumentId;
    const int oldLen = se.totalFrames;

    host_.restore_fx_preview_backup();   // act on the clean audio
    host_.backup_sample(instId);
    host_.sync_sample(instId, sync);
    // Spent either way: TSTRETCH has baked the pending shift in, RPITCH has replaced it.
    se.pitchSemitones = 0;

    rescale_positions(se, oldLen, host_.sample_length(instId));
    // ⚠️ RATE resets (the result is the new original; it would decimate twice). BIT stays: SAVE writes it.
    se.rateMode = 0;
    refresh_sample_view(/*reset_selection=*/true);
    se.isModified = true;
}

// ─── The pending pitch shift ─────────────────────────────────────────────────────────────────────

void InputDispatcher::bake_pending_pitch() {
    SampleEditorState& se = s_.sampleEditor;
    if (se.pitchSemitones == 0) return;

    const int oldLen = se.totalFrames;
    host_.pitch_shift_sample(se.instrumentId, static_cast<float>(se.pitchSemitones));
    const int newLen = host_.sample_length(se.instrumentId);
    // Everything frame-measured moves with the audio; the selection is scaled, not reset.
    rescale_positions(se, oldLen, newLen);
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
    if (se.sliceMethod == SampleEditorModule::SLICE_DIVIDE && se.marker_count() == 0)
        return divide_points(se.totalFrames, se.sliceDivisions);

    // Whichever list the method answers with (MANUAL is seeded from the file's). ⚠️ Under OFF the file's
    // own: a save with slicing OFF can neither add nor drop a slice. Frames 0 and the end are dropped.
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
        set_status(s_, "SAVE FAILED", false);
        return;
    }
    host_.adopt_saved_sample(se.instrumentId, se.bitDepth);

    // ⚠️ A mono save reloads the file: the buffer may still be stereo, and the slot must match the file.
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

    // A slice becomes a file name, sanitized like every other the app writes.
    const std::string base = safe_file_name(se.sampleName.empty() ? "SAMPLE" : se.sampleName);

    // Samples/Chops/<base>/ — its own folder, so a 32-slice break does not bury the sample directory.
    const std::string samples = fs_.samples_directory();
    fs_.create_folder(samples, "Chops");                     // "" if it already exists — either is fine
    const std::string chops = samples + "/Chops";
    fs_.create_folder(chops, base);
    const std::string dir = chops + "/" + base;

    const int written = host_.chop_sample(se.instrumentId, dir, base, slices, se.bitDepth);
    set_status(s_, written > 0 ? ("CHOPPED " + std::to_string(written)) : "CHOP FAILED", written > 0);
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

    // ⚠️ false: the editor's session is not the document (a held A+UP on ZOOM would arm an autosave).
    // RATE and BIT push for themselves.
    return false;
}

}  // namespace pt::ui
