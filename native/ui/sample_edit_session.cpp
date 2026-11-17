#include "ui/sample_edit_session.h"

#include "ui/filesystem.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace pt::ui {

using songcore::Instrument;

namespace {

/** The selection, moved with audio that went from `oldLen` frames to `newLen`. */
void rescale_positions(SampleEditorState& se, int oldLen, int newLen) {
    auto scale = [&](int64_t f) -> int64_t {
        return oldLen > 0 ? std::clamp<int64_t>((f * newLen) / oldLen, 0, newLen) : 0;
    };
    se.selectionStart = scale(se.selectionStart);
    se.selectionEnd   = scale(se.selectionEnd);
}

/** DIVIDE's boundaries: `divisions` equal slices. */
std::vector<int> divide_points(int totalFrames, int divisions) {
    const int        div = std::max(divisions, 1);
    std::vector<int> out;
    for (int i = 1; i < div; ++i)
        out.push_back(static_cast<int>((static_cast<int64_t>(i) * totalFrames) / div));
    return out;
}


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

// ─── Opening and closing ─────────────────────────────────────────────────────────────────────────

void SampleEditSession::init() {
    SampleEditorState& se  = s_.sampleEditor;
    const Instrument&  ins = s_.project->instruments[static_cast<size_t>(se.instrumentId)];

    // The NAME is the FILE's — it is what you will save back over.
    se.sampleFilePath = ins.sampleFilePath.value_or("");
    se.sampleName     = se.sampleFilePath.empty() ? "" : path_stem(se.sampleFilePath);

    se.totalFrames   = ops_.sample_length(se.instrumentId);
    se.sampleRate    = ops_.sample_rate_of(se.instrumentId);
    se.hasStereoData = ops_.has_stereo_data(se.instrumentId);
    // BIT opens at the sample's own depth, which is also the highest it can offer.
    se.sourceBitDepth = ops_.sample_bit_depth(se.instrumentId);
    se.bitDepth       = se.sourceBitDepth;

    // ⚠️ SOURCE opens on STEREO for a stereo sample: any other mode saves one channel.
    se.sourceMode = se.hasStereoData ? 2 /*STEREO*/ : 0;

    ++se.audioVersion;   // the feed re-bins the view

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

void SampleEditSession::close() {
    // Pay any pending audition restore now, or it lands on a slot no longer on screen.
    finish_preview();

    ops_.restore_fx_preview_backup();          // drop an un-applied FX preview
    ops_.free_sample_undo(s_.sampleEditor.instrumentId);   // unreachable once the editor is gone
    host_.clear_previews();                     // the 254/255 scratch slots

    s_.currentScreen = s_.previousScreen;
}

// ─── The audition ────────────────────────────────────────────────────────────────────────────────

void SampleEditSession::audition(long long now_ms) {
    SampleEditorState& se = s_.sampleEditor;

    // Only while the TRANSPORT IS STOPPED: `playbackPosition` also tracks song voices on this sample.
    if (se.playbackPosition >= 0.0f && !host_.is_playing()) {
        host_.stop_preview();
        se.playbackPosition = -1.0f;
        return;
    }

    // ⚠️ A pending restore from the previous preview lands first, or it strips this one's EQ and sends.
    finish_preview();

    previewRestore_     = true;
    previewRestoreAtMs_ = now_ms + 100;
    previewRestoreInst_ = se.instrumentId;

    // The FX row is auditioned by applying it, and the clean audio restored after.
    ops_.restore_fx_preview_backup();
    if (se.fx_armed()) {
        before_long_operation();
        // ⚠️ No clean copy, no audition: the effect would stay in the sample.
        if (ops_.save_fx_preview_backup(se.instrumentId))
            ops_.apply_sample_fx(se.instrumentId, se.fxType, se.fx_param());
        else
            set_status(s_, "OUT OF MEMORY", false);
    }

    // SYNC is auditioned from a copy (it changes the length), taking the pending PITCH with it.
    const songcore::SampleSync sync = planned_sync();
    const bool synced = (sync.kind != songcore::SampleSync::Kind::NONE);
    if (synced) before_long_operation();
    const int pitch = synced ? 0 : se.pitchSemitones;

    host_.set_preview_track(-1);   // a waveform being edited is not in the arrangement
    ops_.preview_sample_editor(se.instrumentId, se.sourceMode, se.selectionStart, se.selectionEnd,
                                se.totalFrames, pitch, sync);
}

void SampleEditSession::finish_preview() {
    if (!previewRestore_) return;
    previewRestore_ = false;
    ops_.finish_sample_preview(previewRestoreInst_);
}

// ─── The selection: A+DPAD on rows 3..8 ──────────────────────────────────────────────────────────

void SampleEditSession::nudge_selection_edge(int64_t delta) {
    SampleEditorState& se = s_.sampleEditor;

    // An empty slot reaches this (EDIT, DOWN, DOWN, A+RIGHT), and the clamps would have lo > hi.
    if (se.totalFrames <= 0) return;

    const int64_t maxFrame = se.totalFrames;
    const int     dir      = (delta >= 0) ? 1 : -1;

    // SNAP: the nearest zero crossing in the direction of travel, ⚠️ in the signal SAVE writes.
    auto snap = [&](int64_t f) -> int64_t {
        if (!se.snapEnabled) return f;
        return ops_.find_zero_crossing(se.instrumentId, static_cast<int>(f), dir, se.sourceMode);
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

// ─── The slice markers: A+DPAD and A+B on SLICE_AT ───────────────────────────────────────────────

/** Copy what the method shows into the editable list, stamped with its (method, parameter). */
void SampleEditSession::materialise_manual_markers() {
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

/** The selection follows SLICE_AT's slice. ⚠️ Every boundary or cursor move ends here, so START plays
 *  the slice as it now is. */
void SampleEditSession::select_current_slice() {
    SampleEditorState& se = s_.sampleEditor;
    int64_t start = 0, end = 0;
    se.slice_bounds(se.sliceIndex, start, end);
    se.selectionStart = start;
    se.selectionEnd   = end;
}

void SampleEditSession::nudge_slice_marker(int64_t delta) {
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
    // SNAP applies to boundaries as to selection edges, searching in the direction of travel.
    if (se.snapEnabled)
        want = ops_.find_zero_crossing(
            se.instrumentId,
            static_cast<int>(std::clamp<int64_t>(want, 0, static_cast<int64_t>(se.totalFrames) - 1)),
            dir, se.sourceMode);

    const int landed = place_slice_marker(m, k, want, dir, se.totalFrames);
    if (landed < 0) return;

    se.sliceIndex = landed + 1;   // the boundary's own number — marker `j` is the left edge of slice j+1
    select_current_slice();
}

void SampleEditSession::reset_slice_marker() {
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
void SampleEditSession::tap_slice_marker() {
    SampleEditorState& se = s_.sampleEditor;

    // Consumed: a press that ended in an A+DPAD must not leave a snapshot for the next tap.
    const float at                  = sliceTapPlayhead_;
    sliceTapPlayhead_ = -1.0f;

    if (se.sliceMethod != SampleEditorModule::SLICE_MANUAL) return;
    if (se.totalFrames <= 0 || at < 0.0f) return;

    const int64_t last = static_cast<int64_t>(se.totalFrames) - 1;
    int64_t       want = std::clamp<int64_t>(
        static_cast<int64_t>(at * static_cast<float>(se.totalFrames)), 0, last);
    if (se.snapEnabled)
        want = ops_.find_zero_crossing(se.instrumentId, static_cast<int>(want), -1, se.sourceMode);

    materialise_manual_markers();
    std::vector<SliceMarker>& m = se.manualMarkers;

    // ⚠️ Past the end of the list on purpose: a tap always MAKES a boundary (origin −1, so A+B removes it).
    const int landed = place_slice_marker(m, static_cast<int>(m.size()), want, +1, se.totalFrames);
    if (landed < 0) return;

    se.sliceIndex = landed + 1;   // the cursor follows the cut onto the slice it opens
    select_current_slice();
}

// ─── RATE and BIT: the two cells that rebuild the audio ──────────────────────────────────────────

bool SampleEditSession::apply_rate_and_bits() {
    before_long_operation();
    SampleEditorState& se     = s_.sampleEditor;
    const int          factor = (se.rateMode == 1) ? 2 : (se.rateMode == 2) ? 4 : 1;
    const int          oldLen = se.totalFrames;

    // ⚠️ Notes already scheduled (up to LONG_OPERATION_MS) keep the old base frequency.
    if (!ops_.apply_rate_and_bits(se.instrumentId, factor, se.bitDepth)) {
        set_status(s_, "OUT OF MEMORY", false);
        return false;
    }

    const int newLen = ops_.sample_length(se.instrumentId);
    rescale_positions(se, oldLen, newLen);
    se.totalFrames = newLen;
    se.sampleRate  = ops_.sample_rate_of(se.instrumentId);
    ++se.audioVersion;   // the feed re-bins the view
    se.isModified = true;
    return true;
}

// ─── The view, after an op ───────────────────────────────────────────────────────────────────────

void SampleEditSession::refresh_view(bool reset_selection) {
    SampleEditorState& se     = s_.sampleEditor;
    const int          newLen = ops_.sample_length(se.instrumentId);
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
            modified_();
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

    ++se.audioVersion;   // the feed re-bins the view
}

// ─── A on the op rows and the FX row ─────────────────────────────────────────────────────────────

void SampleEditSession::run_op() {
    before_long_operation();
    SampleEditorState& se     = s_.sampleEditor;
    // ⚠️ First: an auditioned effect has processed the sample itself, and COPY or an op would take it.
    // Not on SLICE_AT, whose tap must not stop the audition it is tapped against.
    if (se.cursorRow != SampleRow::SLICE_AT) ops_.restore_fx_preview_backup();
    const int          instId = se.instrumentId;
    const int          startF = static_cast<int>(se.selectionStart);
    const int          endF   = static_cast<int>(se.selectionEnd);

    // Memory refused: the sample is as it was, so the view, the selection and the markers stay too.
    auto out_of_memory = [&] { set_status(s_, "OUT OF MEMORY", false); return false; };
    // ⚠️ No undo copy, no op: the user could not take it back.
    auto begin_destructive = [&] { return ops_.backup_sample(instId) || out_of_memory(); };
    auto in_place = [&](auto&& op) {   // an op that does NOT change the length
        if (!begin_destructive()) return;
        op();
        refresh_view(/*reset_selection=*/false);
        se.isModified = true;
    };
    auto resizing = [&](auto&& op) {   // an op that DOES; `op` returns false when memory was refused
        if (!begin_destructive()) return;
        if (!op()) { out_of_memory(); return; }
        refresh_view(/*reset_selection=*/true);
        se.isModified = true;
    };

    switch (se.cursorRow) {
        // ── SLICE_AT: cut a boundary at the playhead, mid-audition ───────────────────────────────
        // On A's release (defer_a_to_release), at the playhead of the press. Editor state: no backup.
        case SampleRow::SLICE_AT:
            tap_slice_marker();
            break;

        // ── The two rows of buttons ──────────────────────────────────────────────────────────────
        case SampleRow::OPS1:
        case SampleRow::OPS2: {
            const SampleOpCell* cell = sample_op_at(se.cursorRow, se.cursorCol);
            if (cell == nullptr) break;
            switch (cell->op) {
                case SampleOp::CROP:   // keep the selection, discard the rest
                    if (startF < endF) resizing([&] { return ops_.crop_sample(instId, startF, endF); });
                    break;
                case SampleOp::COPY:   // the ONE op that changes nothing, so it takes no backup
                    if (!ops_.copy_region(instId, startF, endF)) out_of_memory();
                    break;
                case SampleOp::CUT:    // copy, then delete
                    if (startF < endF) resizing([&] {
                        return ops_.copy_region(instId, startF, endF) &&
                               ops_.delete_sample_region(instId, startF, endF);
                    });
                    break;
                case SampleOp::DUPL:   // copy the selection and paste it at the END
                    if (startF < endF) resizing([&] {
                        return ops_.copy_region(instId, startF, endF) &&
                               ops_.paste_region(instId, se.totalFrames);
                    });
                    break;
                case SampleOp::PASTE:  // inserts at the selection's START
                    if (ops_.clipboard_length() > 0)
                        resizing([&] { return ops_.paste_region(instId, startF); });
                    break;
                case SampleOp::DEL:
                    if (startF < endF)
                        resizing([&] { return ops_.delete_sample_region(instId, startF, endF); });
                    break;

                case SampleOp::NORM:     in_place([&] { ops_.normalize_sample(instId, startF, endF); }); break;
                case SampleOp::FADE_IN:  in_place([&] { ops_.fade_in_sample(instId, startF, endF); }); break;
                case SampleOp::FADE_OUT: in_place([&] { ops_.fade_out_sample(instId, startF, endF); }); break;
                case SampleOp::SILENCE:  in_place([&] { ops_.silence_region(instId, startF, endF); }); break;
                case SampleOp::REVERSE:  in_place([&] { ops_.reverse_sample(instId, startF, endF); }); break;

                case SampleOp::UNDO: {   // one level. Resets the window, loop and markers only when
                                         // the length changed: after NORM, a fade or REV they still fit.
                    const int lenBefore = se.totalFrames;
                    if (!ops_.undo_sample(instId)) { out_of_memory(); break; }
                    refresh_view(ops_.sample_length(instId) != lenBefore);
                    // ⚠️ `isModified` is NOT cleared: one undo is one step back, not back to the file.
                    break;
                }
            }
            break;
        }

        // ── FX: col 2 is APPLY; the other two are dialled with A+DPAD. ───────────────────────────
        case SampleRow::FX: {
            if (se.cursorCol != 2) break;

            if (se.fxType <= SampleEditorModule::FX_EQ) {
                if (!se.fx_armed()) break;

                if (!begin_destructive()) break;
                ops_.apply_sample_fx(instId, se.fxType, se.fx_param());
                refresh_view(/*reset_selection=*/false);
                se.isModified = true;
                // Spent — left in the cell, START would audition it over its own result.
                if (se.fxType == SampleEditorModule::FX_EQ) se.fxEqSlot = -1;
                else                                        se.fxValue  = 0;
                break;
            }

            apply_sync();
            break;
        }

        default:
            break;
    }
}

// ─── SYNC: fit the sample to the grid ────────────────────────────────────────────────────────────

songcore::SampleSync SampleEditSession::planned_sync() const {
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

void SampleEditSession::apply_sync() {
    SampleEditorState&         se   = s_.sampleEditor;
    const songcore::SampleSync sync = planned_sync();
    if (sync.kind == songcore::SampleSync::Kind::NONE) return;

    const int instId = se.instrumentId;
    const int oldLen = se.totalFrames;

    ops_.restore_fx_preview_backup();   // act on the clean audio
    if (!ops_.backup_sample(instId)) {   // no undo copy, no op
        set_status(s_, "OUT OF MEMORY", false);
        return;
    }
    if (!ops_.sync_sample(instId, sync)) {
        set_status(s_, "OUT OF MEMORY", false);
        // ⚠️ Refused after its pitch went through? Then the length moved: show what is there.
        if (ops_.sample_length(instId) == oldLen) return;
    }
    // Spent either way: TSTRETCH has baked the pending shift in, RPITCH has replaced it.
    se.pitchSemitones = 0;

    rescale_positions(se, oldLen, ops_.sample_length(instId));
    // ⚠️ RATE resets (the result is the new original; it would decimate twice). BIT stays: SAVE writes it.
    se.rateMode = 0;
    refresh_view(/*reset_selection=*/true);
    se.isModified = true;
}

// ─── The pending pitch shift ─────────────────────────────────────────────────────────────────────

bool SampleEditSession::bake_pending_pitch() {
    SampleEditorState& se = s_.sampleEditor;
    if (se.pitchSemitones == 0) return true;

    const int oldLen = se.totalFrames;
    if (!ops_.pitch_shift_sample(se.instrumentId, static_cast<float>(se.pitchSemitones))) {
        set_status(s_, "OUT OF MEMORY", false);
        return false;
    }
    const int newLen = ops_.sample_length(se.instrumentId);
    // Everything frame-measured moves with the audio; the selection is scaled, not reset.
    rescale_positions(se, oldLen, newLen);
    se.totalFrames    = newLen;
    se.pitchSemitones = 0;   // spent
    se.rateMode       = 0;   // the shifted buffer IS the new original — see sample_edit.h
    // ⚠️ BIT is NOT reset. This runs on the way INTO a save, and BIT is the depth that save writes.
    ++se.audioVersion;   // the feed re-bins the view

    // The instrument's playback params were derived from the old buffer's length.
    host_.push_instrument(se.instrumentId);
    modified_();
    return true;
}

// ─── The slices ──────────────────────────────────────────────────────────────────────────────────

std::vector<int> SampleEditSession::cue_points() const {
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

std::vector<std::pair<int64_t, int64_t>> SampleEditSession::current_slices() const {
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

bool SampleEditSession::save_to(const std::string& path, bool adopt_name) {
    before_long_operation();
    SampleEditorState& se   = s_.sampleEditor;
    const std::vector<int> cues = cue_points();

    if (!ops_.save_sample_wav(se.instrumentId, path, cues, se.sourceMode, se.hasStereoData,
                               se.bitDepth)) {
        set_status(s_, "SAVE FAILED", false);
        return false;
    }
    ops_.adopt_saved_sample(se.instrumentId, se.bitDepth);

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
    se.hasStereoData = ops_.has_stereo_data(se.instrumentId);

    modified_();
    s_.currentScreen = s_.previousScreen;   // a save LEAVES the editor, as it does on Android
    return true;
}

// ─── CHOP ────────────────────────────────────────────────────────────────────────────────────────

std::string SampleEditSession::home_folder() {
    const std::string& src = s_.sampleEditor.sampleFilePath;
    return src.empty() ? fs_.samples_directory() : fs_.parent_path(src);
}

void SampleEditSession::chop() {
    SampleEditorState& se = s_.sampleEditor;
    if (se.sliceMethod == SampleEditorModule::SLICE_OFF) return;

    const std::vector<std::pair<int64_t, int64_t>> slices = current_slices();
    if (slices.empty()) return;

    // A slice becomes a file name, sanitized like every other the app writes.
    const std::string base = safe_name(se.sampleName.empty() ? "SAMPLE" : se.sampleName);

    // A folder of its own beside the source, so a 32-slice break does not bury the folder it came from.
    const std::string home = home_folder();
    fs_.create_folder(home, base);                           // "" if it already exists — either is fine
    const std::string dir = home + "/" + base;

    const int written = ops_.chop_sample(se.instrumentId, dir, base, slices, se.sourceMode,
                                          se.hasStereoData, se.bitDepth);
    set_status(s_, written > 0 ? ("CHOPPED " + std::to_string(written)) : "CHOP FAILED", written > 0);
}

}  // namespace pt::ui
