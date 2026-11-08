#pragma once

// The sample editor's session: what its buttons do to the sample the engine holds — the selection, the
// slice markers, the ops, the audition, SAVE and CHOP. What is drawn lives in `AppState::sampleEditor`;
// the buttons themselves, and the keyboard and browser they open, are the dispatcher's.
// ⚠️ The edits live in the engine, not the project, until a SAVE.

#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/filesystem.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pt::ui {

class SampleEditSession {
  public:
    // `modified`: the project changed (the dispatcher's dirty flag and engine push).
    SampleEditSession(AppState& s, songcore::SongcoreHost& host, FileSystem& fs,
                      std::function<void()> modified)
        : s_(s), host_(host), ops_(host.sample_editor()), fs_(fs), modified_(std::move(modified)) {}

    // Build the state from the sample the engine holds; separate from opening because the editor's
    // LOAD re-enters on different audio. ⚠️ Must not touch `previousScreen` (the editor's return target).
    void init();

    // Free the undo, drop the scratch slots and any un-applied FX preview, go back.
    void close();

    // START toggles the audition of the selection, with the FX row's effect on it (SYNC included).
    // The one audition a plain press does not silence.
    void audition(long long now_ms);

    // ⚠️ The audition's deferred half: the preview's window and stripped EQ, sends and mods are read
    // when the voice triggers, so they are restored a frame later.
    void run_due_preview_restore(long long now_ms) {
        if (previewRestore_ && now_ms >= previewRestoreAtMs_) finish_preview();
    }

    // A went down on the tap cell: the playhead it was aimed at (0..1, −1 = nothing sounding). The
    // release comes a reaction time later.
    void note_tap_playhead(float playhead) { sliceTapPlayhead_ = playhead; }

    // A+DPAD on rows 3..8: drag the selection's edge under the cursor.
    void nudge_selection_edge(int64_t delta);

    // A+DPAD on SLICE_AT's POSITION cell: drag the boundary under the cursor.
    void nudge_slice_marker(int64_t delta);

    // A+B on SLICE_AT: the marker back where its method would put it; under MANUAL, gone.
    void reset_slice_marker();

    // A on SLICE_AT, OPS1, OPS2 and FX: the tap, the ops, the FX apply.
    void run_op();

    // RATE and BIT rebuild the buffer; false (with a message) when memory was refused.
    bool apply_rate_and_bits();

    // What SYNC would do to the sample now (NONE off SYNC, or when it already fits). APPLY and the
    // audition both ask here, so START plays what APPLY will write.
    songcore::SampleSync planned_sync() const;

    // SYNC's APPLY: fit the sample to DURATION by RPITCH or TSTRETCH. The pending PITCH is spent.
    void apply_sync();

    // Bake the pending pitch into the buffer and rescale everything in frames — every save calls it,
    // since a sample is saved as it sounds. False (with a message) when memory was refused: no save.
    bool bake_pending_pitch();

    // SAVE / SAVE-AS / OVERWRITE: write the WAV, adopt it, leave the editor. False: nothing written.
    bool save_to(const std::string& path, bool adopt_name);

    // CHOP: every slice to `Samples/Chops/<name>/` as its own WAV.
    void chop();

  private:
    // The restore now: a second START, or the first restore would undo the second; a close, or it
    // lands on a slot no longer on screen.
    void finish_preview();

    // A on SLICE_AT under MANUAL: cut a boundary at the playhead — START, listen, tap on each hit.
    void tap_slice_marker();

    // Put the selection on the slice SLICE_AT's cursor points at; every boundary gesture ends here.
    void select_current_slice();

    // Copy the method's computed boundaries into the hand-placed list, so a nudge has a home.
    void materialise_manual_markers();

    // Re-read length and waveform after an op. `reset_selection`: the length changed, so the selection,
    // the sample and loop windows and ⚠️ every slice marker are reset, not clamped.
    void refresh_view(bool reset_selection);

    // The slice boundaries as WAV cue points: the markers, or DIVIDE's computed cuts.
    std::vector<int> cue_points() const;

    // The slice (start, end) pairs the current method defines.
    std::vector<std::pair<int64_t, int64_t>> current_slices() const;

    // At the top of an op that holds the UI thread long enough to be heard.
    void before_long_operation() { host_.keep_walking(songcore::Sequencer::LONG_OPERATION_MS); }

    AppState&               s_;
    songcore::SongcoreHost& host_;
    songcore::SampleEditorOps& ops_;   // host_'s sample editor
    FileSystem&             fs_;
    std::function<void()>   modified_;

    // What the buttons leave for later (not drawn, so not in `s_.sampleEditor`).
    bool      previewRestore_     = false;
    long long previewRestoreAtMs_ = 0;
    int       previewRestoreInst_ = 0;
    // Consumed by `tap_slice_marker`, so a press that became a combo leaves nothing.
    float sliceTapPlayhead_ = -1.0f;
};

}  // namespace pt::ui
