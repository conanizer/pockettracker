// The sample editor's buttons. What they do to the sample is the session's (ui/sample_edit_session.h).

#include "ui/dispatch/dispatch_common.h"

#include "ui/filesystem.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

namespace {

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

    // A+DPAD drags the selection's active edge on SELECTION, the slice boundary on SLICE_AT's
    // POSITION cell; elsewhere it edits the cell like any screen.
    auto nudge = [&](int64_t delta) {
        if (on_sample_selection_row())        sampleSession_.nudge_selection_edge(delta);
        else if (on_sample_slice_marker_row()) sampleSession_.nudge_slice_marker(delta);
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
            else               sampleSession_.close();
            return GestureResult::TAKEN;

        case Gesture::START:
            sampleSession_.audition(now_ms_);
            return GestureResult::TAKEN;

        case Gesture::A_UP:    return nudge(+coarse_step(se));
        case Gesture::A_DOWN:  return nudge(-coarse_step(se));
        case Gesture::A_LEFT:  return nudge(-fine_step(se));
        case Gesture::A_RIGHT: return nudge(+fine_step(se));

        case Gesture::A_B:
            // SELECTION: back to the sample's own bound — START to 0, END to the last frame.
            if (se.cursorRow == SampleRow::SELECTION) {
                if (se.cursorCol == 0)      se.selectionStart = 0;
                else if (se.cursorCol == 1) se.selectionEnd   = se.totalFrames;
                return GestureResult::TAKEN;
            }
            // SLICE_AT: the boundary back where its method would put it; under MANUAL, gone.
            if (se.cursorRow == SampleRow::SLICE_AT) {
                sampleSession_.reset_slice_marker();
                return GestureResult::TAKEN;
            }
            // The TSTRETCH cell: everything its box dials, back as a session opens.
            if (se.cursorRow == SampleRow::FX && se.cursorCol == 1 && se.fxType == SampleEditorModule::FX_SYNC &&
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
            sampleSession_.close();
            break;
        case Gesture::B:   // NO — stay
            se.showConfirmClose = false;
            break;
        default:
            break;
    }
    return GestureResult::TAKEN;
}

// ─── Opening ─────────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::open_sample_editor() {
    const Instrument& ins = s_.project->instruments[static_cast<size_t>(s_.currentInstrument)];
    if (ins.instrumentType == songcore::InstrumentType::SOUNDFONT) return;

    s_.sampleEditor              = SampleEditorState{};   // a fresh session, every time
    s_.sampleEditor.instrumentId = s_.currentInstrument;

    s_.previousScreen = s_.currentScreen;
    s_.currentScreen  = ScreenType::SAMPLE_EDITOR;
    sampleSession_.init();
}

// ─── A on the name and the save buttons; the rest is the session's ───────────────────────────────

void InputDispatcher::sample_editor_confirm() {
    SampleEditorState& se = s_.sampleEditor;
    if (se.cursorRow != SampleRow::NAME && se.cursorRow != SampleRow::FILE) {
        sampleSession_.run_op();
        return;
    }
    before_long_operation();
    // ⚠️ First: an auditioned effect has processed the sample itself, and SAVE would take it.
    host_.sample_editor().restore_fx_preview_backup();

    switch (se.cursorRow) {
        case SampleRow::NAME:
            open_qwerty(QwertyContext::SAMPLE_NAME, se.sampleName, "SAMPLE NAME:",
                        fs_.samples_directory());
            break;

        // ── FILE: LOAD  SAVE  OVERWRITE  CHOP ───────────────────────────────────────────────────
        case SampleRow::FILE:
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
                    if (!sampleSession_.bake_pending_pitch()) break;

                    // Taken: suggest the next free name — SAVE is not OVERWRITE.
                    const std::string stem = free_wav_stem(fs_, dir, base);
                    if (stem == base)
                        sampleSession_.save_to(dir + "/" + base + ".wav", /*adopt_name=*/true);
                    else
                        open_qwerty(QwertyContext::SAMPLE_SAVE, stem, "SAVE AS:", dir,
                                    /*max_length=*/24, /*clear_on_first_b=*/true);
                    break;
                }

                case 2: {   // OVERWRITE — back over the file it came from. Nothing to do if it has none.
                    if (se.sampleFilePath.empty()) break;
                    if (!sampleSession_.bake_pending_pitch()) break;
                    if (to_lower_ascii(path_extension(se.sampleFilePath)) == "wav") {
                        sampleSession_.save_to(se.sampleFilePath, /*adopt_name=*/false);
                        break;
                    }
                    // ⚠️ What is written is a WAV, and the loader picks its decoder by extension: an MP3
                    // overwritten in place would never load again. Keep it; the WAV goes beside it.
                    const std::string dir  = fs_.parent_path(se.sampleFilePath);
                    const std::string stem = free_wav_stem(fs_, dir, path_stem(se.sampleFilePath));
                    if (sampleSession_.save_to(dir + "/" + stem + ".wav", /*adopt_name=*/false))
                        set_status(s_, "SAVED: " + stem + ".wav", true);
                    break;
                }

                case 3:   // CHOP
                    sampleSession_.chop();
                    break;

                default: break;
            }
            break;

        default:
            break;
    }
}

// ─── The cursor and the edit ─────────────────────────────────────────────────────────────────────

CursorContext InputDispatcher::sample_editor_context() const {
    return sample_.cursor_context(s_.sampleEditor);
}

bool InputDispatcher::sample_editor_edit(const InputAction& action) {
    SampleEditorState& se       = s_.sampleEditor;
    const int          rateMode = se.rateMode;
    const int          bitDepth = se.bitDepth;
    const SampleEditorInputResult r = sample_.handle_input(se, action);
    // Refused: the cells go back to what the audio still is.
    if ((r.rateModeChanged || r.bitDepthChanged) && !sampleSession_.apply_rate_and_bits()) {
        se.rateMode = rateMode;
        se.bitDepth = bitDepth;
    }

    // ⚠️ false: the editor's session is not the document (a held A+UP on ZOOM would arm an autosave).
    // RATE and BIT push for themselves.
    return false;
}

}  // namespace pt::ui
