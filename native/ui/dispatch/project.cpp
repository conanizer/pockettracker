// The confirm dialog, NEW and LOAD, the RENDER dialog, export and resample, and the PROJECT and
// SETTINGS screens' buttons.

#include "ui/dispatch/dispatch_common.h"

#include "ui/lifecycle.h"        // the crash-recovery autosave — write / clear / load
#include "ui/navigation.h"
#include "ui/song_pointer.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace pt::ui {


void InputDispatcher::confirm_accept() {
    const ConfirmDialogState::Kind kind = s_.confirm.kind;
    // ⚠️ Read `arg` before `close()` resets it — the CHANGE_TYPE arm needs the direction.
    const int argv = s_.confirm.arg;
    s_.confirm.close();   // FIRST — every arm below can re-open a dialog, and none should be stacked

    switch (kind) {
        case ConfirmDialogState::Kind::CLEAN_SEQ:
            host_.clean_seq();
            mark_modified();
            s_.statusMessage = "SEQ CLEANED";
            s_.statusSuccess = true;
            break;

        case ConfirmDialogState::Kind::CLEAN_INST:
            // ⚠️ `clean_inst` also reloads the media: the emptied slots' buffers are still in the
            // engine, and without the reload the RAM would not drop until the next save and open.
            host_.clean_inst(fs_.samples_directory());
            mark_modified();
            {
                // The reload can fail like a project load — see load_project_done.
                const int failed = host_.last_media_load().failed;
                s_.statusMessage =
                    failed > 0 ? "CLEANED: " + std::to_string(failed) + " MISSING" : "INST CLEANED";
                s_.statusSuccess = (failed == 0);
            }
            break;

        case ConfirmDialogState::Kind::NEW_PROJECT:
            start_new_project();
            break;

        case ConfirmDialogState::Kind::CHANGE_TYPE:
            // Switching type frees a loaded source, hence the dialog. ⚠️ The direction comes back out
            // of the dialog (`arg`), so A+LEFT still steps backwards after a yes.
            toggle_instrument_type(argv);
            break;

        case ConfirmDialogState::Kind::EXIT:
            // ⚠️ A confirmed EXIT is the app's one clean death, so it leaves nothing to recover. That
            // is also why EXIT still asks despite the autosave: it is the only way to deliberately
            // discard a session. Every unclean death (SIGTERM, flat battery, crash, F10) keeps the work
            // via `flush_autosave()`.
            autosave_clear(fs_);
            s_.shouldQuit = true;
            break;

        case ConfirmDialogState::Kind::RECOVER:
            // A = recover. The document comes back DIRTY and the file STAYS (see recover_from_autosave).
            if (!recover_from_autosave()) autosave_clear(fs_);
            break;

        case ConfirmDialogState::Kind::NONE:
            break;
    }
}

void InputDispatcher::confirm_cancel() {
    const ConfirmDialogState::Kind kind = s_.confirm.kind;
    s_.confirm.close();

    // ⚠️ The one question whose NO is an action: discard the unsaved work. Leaving the file would bring
    // the prompt back every launch, teaching people to dismiss it unread.
    if (kind == ConfirmDialogState::Kind::RECOVER) autosave_clear(fs_);
}

/**
 * The editing context, back to zero. Shared by NEW and LOAD: otherwise INSTRUMENT would sit on a slot
 * the user never chose, and A,A on SONG would insert a chain number from the previous song.
 */
void InputDispatcher::reset_editing_context() {
    s_.currentPhrase = s_.currentChain = s_.currentInstrument = 0;
    s_.currentTable  = s_.currentGroove = 0;
    s_.lastEditedPhrase = s_.lastEditedChain = s_.lastEditedTable = 0;
    s_.lastEditedInstrument = s_.lastEditedTranspose = 0;
    s_.lastEditedNote   = songcore::Note::C4();
    s_.lastEditedVolume = 0x7F;

    // …and every secondary screen's own cursor, or they point into the previous song after a LOAD.
    s_.cursorRow = 0; s_.cursorColumn = 1;
    s_.songScrollPosition = 0;
    s_.instrumentCursorRow = 0; s_.instrumentCursorColumn = 1;
    // ⚠️ Both halves of the mixer cursor: a column reset alone can leave a master-strip-only row
    // over a track column, and the mixer shows no cursor.
    s_.mixerCursorColumn = 0;
    s_.mixerMasterRow    = 0;
    s_.effectsCursorRow  = 0;
    s_.tableCursorRow = 0; s_.tableCursorColumn = 1;
    s_.grooveCursorRow = 0; s_.grooveCursorColumn = GROOVE_COL_TICK;
    s_.groovePanelRow = 0;  s_.groovePanelColumn  = 0;
    // The quantize pointer is an editing aid, not a setting, so it resets with the project.
    s_.grooveQuantize = 0;
    s_.modCursorRow = 0; s_.modCursorPair = 0; s_.modCursorSide = 0;
    s_.projectCursorRow = 0; s_.projectCursorColumn = 1;

    // The REMEMBER slots, or REMEMBER mode restores a cursor from the previous song.
    s_.songCursorRow = 0;   s_.songCursorColumn = 1;
    s_.chainCursorRow = 0;  s_.chainCursorColumn = 1;
    s_.phraseCursorRow = 0; s_.phraseCursorColumn = 1;

    // ⚠️ Not the SETTINGS cursor and not poolCursorColumn: both persist on purpose.
    s_.selection = Selection{};

    // ⚠️ Under NAV = SONG the REMEMBER slots above ARE the pointer, and song row 0 / track 1 may be
    // empty, which would trap the user on SONG. So it is derived from the arrangement here.
    clamp_song_pointer(s_);
}

void InputDispatcher::start_new_project() {
    host_.new_project();

    // Blank document: nothing unsaved and no path — without the version reset the next NEW or EXIT
    // would ask about unsaved work.
    s_.projectVersion      = 0;
    s_.savedProjectVersion = 0;
    s_.projectPath.clear();

    // …and nothing to recover: a clean transition DELETES the autosave, or the next launch offers to
    // restore a song the user started over from. The pending deadline goes too, or it writes the blank
    // document back out in three seconds.
    autosavePending_ = false;
    autosave_clear(fs_);

    reset_editing_context();

    s_.statusMessage = "NEW PROJECT";
    s_.statusSuccess = true;
}

/** A .ptp just replaced the document. Leave the browser, and forget everything about the last one. */
void InputDispatcher::load_project_done(const std::string& path) {
    // A freshly loaded project is clean and has nothing to recover. ⚠️ Autosave recovery is the one
    // load that does neither — see recover_from_autosave.
    s_.projectVersion      = 0;
    s_.savedProjectVersion = 0;
    s_.projectPath         = path;

    autosavePending_ = false;   // …or it fires 3 s from now and re-creates the file this just deleted
    autosave_clear(fs_);

    reset_editing_context();

    close_file_browser();

    // ⚠️ A sample that did not load leaves its instrument silent; the target platforms have no console,
    // so this is the only report — including an out-of-memory sample on a small device.
    const int failed = host_.last_media_load().failed;
    s_.statusMessage = failed > 0 ? "LOADED: " + std::to_string(failed) + " MISSING" : "LOADED";
    s_.statusSuccess = (failed == 0);
}

// ─── The RENDER dialog ───────────────────────────────────────────────────────────────────────────

void InputDispatcher::open_render_dialog(RenderDialogState::Output output) {
    if (s_.isRendering) return;

    RenderDialogState& rd = s_.renderDialog;
    rd.isOpen    = true;
    rd.output    = output;
    rd.cursorRow = static_cast<int>(RenderRow::SONG_START);
    // ⚠️ The SONG screen's SAVED cursor: raised from PROJECT, the live `cursorRow` is PROJECT's.
    rd.startRow = songcore::song_section_start(host_.project(), s_.songCursorRow);
    rd.endRow   = -1;   // AUTO — follow the section, however it is edited between now and the render
}

void InputDispatcher::render_dialog_move_cursor(int delta) {
    const int last = static_cast<int>(RenderRow::COUNT) - 1;
    s_.renderDialog.cursorRow = std::max(0, std::min(last, s_.renderDialog.cursorRow + delta));
}

void InputDispatcher::render_dialog_edit(int delta) {
    RenderDialogState& rd = s_.renderDialog;
    if (s_.isRendering) return;

    switch (static_cast<RenderRow>(rd.cursorRow)) {
        case RenderRow::SONG_START: {
            rd.startRow = std::max(0, std::min(255, rd.startRow + delta));
            // A start past a hand-typed end drags the end with it, so the range never runs backwards.
            if (rd.endRow >= 0 && rd.endRow < rd.startRow) rd.endRow = rd.startRow;
            break;
        }
        case RenderRow::SONG_END: {
            // ⚠️ AUTO sits below the smallest end, SONG START. Stepping UP off AUTO lands on the row
            // AUTO was resolving to, so dialling starts from the number already shown.
            if (rd.endRow < 0) {
                if (delta > 0) rd.endRow = render_dialog_end_row(rd, host_.project());
                break;
            }
            const int next = rd.endRow + delta;
            rd.endRow = (next < rd.startRow) ? -1 : std::min(255, next);
            break;
        }
        case RenderRow::REPEAT: {
            // Same shape: OFF sits below 2. There is no "×1" — that is what OFF says.
            const int next = rd.repeat + delta;
            rd.repeat = next < 2 ? 1 : std::min(RENDER_REPEAT_MAX, next);
            break;
        }
        case RenderRow::RENDER:
        case RenderRow::COUNT:
            break;
    }
}

void InputDispatcher::render_dialog_step_section(int delta) {
    if (s_.isRendering) return;
    RenderDialogState& rd = s_.renderDialog;
    // ⚠️ Moves the whole range from any row: a part is a start and an end, so the end goes back to AUTO.
    rd.startRow = songcore::adjacent_section_start(host_.project(), rd.startRow, delta);
    rd.endRow   = -1;
}

void InputDispatcher::render_dialog_fire() {
    if (s_.isRendering) return;
    export_song(s_.renderDialog.output == RenderDialogState::Output::STEMS);
    // ⚠️ Closed on the way out, worked or not: the answer goes on the status line, which this panel dims.
    s_.renderDialog.isOpen = false;
}

void InputDispatcher::export_song(bool stems) {
    if (s_.isRendering) return;   // a second press while one runs is a mis-press, not a request

    host_.stop();                                    // the session ends; a render is not playback
    if (render_.suspend_audio) render_.suspend_audio(true);

    s_.isRendering    = true;
    s_.renderProgress = 0.0f;
    s_.statusMessage  = stems ? "RENDERING STEMS..." : "RENDERING...";
    s_.statusSuccess  = true;
    if (render_.repaint) render_.repaint();          // …so the message is on screen before we block

    const auto progress = [this](float p) {
        s_.renderProgress = p;
        if (render_.repaint) render_.repaint();      // the EXPORT row's "43%" — a readout, not a decoration
    };

    // The panel's rows, with AUTO resolved against the project as it stands right now.
    RenderRange range;
    range.startRow = s_.renderDialog.startRow;
    range.endRow   = render_dialog_end_row(s_.renderDialog, host_.project());
    range.repeat   = s_.renderDialog.repeat;

    const ActionResult r = stems ? render_stems(host_, fs_, s_, range, progress)
                                 : render_mix(host_, fs_, s_, range, progress);

    s_.isRendering    = false;
    s_.renderProgress = 0.0f;
    s_.statusMessage  = r.message;
    s_.statusSuccess  = r.ok;

    if (render_.suspend_audio) render_.suspend_audio(false);
}

void InputDispatcher::resample_selection(const std::string& customBaseName) {
    if (s_.isRendering) return;         // a render is already running — a second APPLY is a mis-press
    if (!s_.selection.active) return;   // the selection lapsed between opening the keyboard and APPLY

    // The selected TRACKS: SONG column − 1, clamped to the eight that exist.
    const SelectionBounds b = s_.selection.bounds();
    std::set<int> tracks;
    for (int c = b.topLeftColumn - 1; c <= b.bottomRightColumn - 1; ++c)
        if (c >= 0 && c <= 7) tracks.insert(c);
    if (tracks.empty()) return;

    // The same synchronous shape as export_song: stop the session, hand the device to the render, and
    // put the "RESAMPLING..." line on screen before we block.
    host_.stop();
    if (render_.suspend_audio) render_.suspend_audio(true);

    s_.isRendering    = true;
    s_.renderProgress = 0.0f;
    s_.statusMessage  = "RESAMPLING...";
    s_.statusSuccess  = true;
    if (render_.repaint) render_.repaint();

    const auto progress = [this](float p) {
        s_.renderProgress = p;
        if (render_.repaint) render_.repaint();
    };

    std::string        outPath;
    const ActionResult r = render_resample(host_, fs_, b.topLeftRow, b.bottomRightRow, tracks,
                                           customBaseName, outPath, progress);

    s_.isRendering    = false;
    s_.renderProgress = 0.0f;

    if (r.ok) {
        const int instId = create_resampled_instrument(host_, outPath);
        if (instId >= 0) {
            char msg[40];
            // ASCII arrow: the 5×5 font has no →.
            std::snprintf(msg, sizeof(msg), "RESAMPLED -> INST %02X", instId);
            s_.statusMessage = msg;
            s_.statusSuccess = true;
            mark_modified();   // a new instrument is unsaved work
        } else {
            // The WAV rendered but no slot could take it (pool full, or the file will not reload).
            s_.statusMessage = "NO FREE INSTRUMENT";
            s_.statusSuccess = false;
        }
    } else {
        s_.statusMessage = r.message;   // "RESAMPLE FAILED"
        s_.statusSuccess = false;
    }

    if (render_.suspend_audio) render_.suspend_audio(false);
}

void InputDispatcher::project_action() {
    switch (static_cast<ProjectRow>(s_.projectCursorRow)) {
        case ProjectRow::NAME:
            // ⚠️ Empty on purpose: NAME is a deferred cell, already handled by
            // `open_sub_screen_at_cursor`. A silent `default:` would invite a second opener.
            break;

        case ProjectRow::PROJECT:
            switch (s_.projectCursorColumn) {
                case 1: {   // SAVE
                    before_long_operation();
                    const ActionResult r = save_project(host_, fs_, s_);
                    s_.statusMessage = r.message;
                    s_.statusSuccess = r.ok;
                    break;
                }
                case 2:     // LOAD
                    open_file_browser(AppState::BrowserPurpose::LOAD_PROJECT,
                                      browser_dir(BrowserDir::PROJECTS), {"ptp"});
                    break;
                case 3:     // NEW
                    // Only ask if there is something to lose.
                    if (s_.project_dirty()) s_.confirm.open(ConfirmDialogState::Kind::NEW_PROJECT);
                    else                    start_new_project();
                    break;
                default: break;
            }
            break;

        case ProjectRow::EXPORT:
            // Neither button renders directly: both open the RENDER panel, and which was pressed
            // picks stereo WAV or stems.
            if (s_.projectCursorColumn == 1)      open_render_dialog(RenderDialogState::Output::MIX);
            else if (s_.projectCursorColumn == 2) open_render_dialog(RenderDialogState::Output::STEMS);
            break;

        case ProjectRow::COMPACT:
            if (s_.projectCursorColumn == 1)
                s_.confirm.open(ConfirmDialogState::Kind::CLEAN_SEQ);
            else if (s_.projectCursorColumn == 2)
                s_.confirm.open(ConfirmDialogState::Kind::CLEAN_INST);
            break;

        case ProjectRow::SYSTEM: {
            // A shortcut into SETTINGS, which owns no column, so R+UP later returns to the main-row
            // screen you were on. B's way out is captured separately (see AppState::settingsReturnScreen).
            s_.settingsReturnScreen = s_.currentScreen;
            NavResult nav;
            nav.screen = ScreenType::SETTINGS;
            nav.column = s_.previousColumn;
            go_to_screen(s_, nav);
            break;
        }

        case ProjectRow::MIDI: {
            // ⚠️ The row is hidden where the build hides MIDI, and PROJECT is the MIDI screen's only
            // door — so this guard is the real gate, turning back a stale cursor or a future caller.
            if (!s_.caps.midi) break;

            // Like SYSTEM above, minus the nav grid; B is the only way back. ⚠️ Both port lists are
            // enumerated here, on the way in — a port list is only true at the moment it is read.
            refresh_midi_devices();
            refresh_midi_in_devices();
            s_.midiStatusText.clear();   // last visit's "TEST SENT" is not this visit's news
            s_.midiReturnScreen = s_.currentScreen;
            NavResult nav;
            nav.screen = ScreenType::MIDI;
            nav.column = s_.previousColumn;
            go_to_screen(s_, nav);
            break;
        }

        case ProjectRow::EXIT:
            // ⚠️ Only where the platform can exit, and it still asks like NEW: the dialog is the app's
            // one deliberate way to discard a session, and its YES the one clean death.
            if (!s_.caps.appExit) break;
            if (s_.project_dirty()) s_.confirm.open(ConfirmDialogState::Kind::EXIT);
            else                    s_.shouldQuit = true;
            break;

        // TAP — the TEMPO row's second cell, and A on it alone.
        //
        // ⚠️⚠️ The column guard is the feature: the mapper fires plain A on A's own press, so every
        // A+UP used to nudge the BPM was also counted as a tap.
        case ProjectRow::TEMPO:
            if (s_.projectCursorColumn == 2) tap_tempo();
            break;

        // TRANSPOSE is an A+DPAD cell; plain A does nothing.
        default:
            break;
    }
}

void InputDispatcher::tap_tempo() {
    const long long now = now_ms_;

    // A gap this long is a pause, not a beat — start counting again from this tap.
    if (tapTempoLastMs_ == 0 || now - tapTempoLastMs_ > TAP_TEMPO_TIMEOUT_MS) {
        tapTempoLastMs_ = now;
        tapTempoCount_  = 0;
        return;
    }

    const long long gap = now - tapTempoLastMs_;
    // A bounce: keep the anchor, so the next tap measures from the last real one.
    if (gap < TAP_TEMPO_MIN_MS) return;
    tapTempoLastMs_ = now;

    // Shift the ring, newest last; four entries is cheaper than a write cursor.
    for (int i = TAP_TEMPO_KEEP - 1; i > 0; --i) tapTempoGaps_[i] = tapTempoGaps_[i - 1];
    tapTempoGaps_[0] = gap;
    if (tapTempoCount_ < TAP_TEMPO_KEEP) ++tapTempoCount_;

    long long sum = 0;
    for (int i = 0; i < tapTempoCount_; ++i) sum += tapTempoGaps_[i];
    const long long meanMs = sum / tapTempoCount_;
    if (meanMs <= 0) return;

    // Rounded, not truncated, so a 500 ms mean reads 120 and not 119.
    const int bpm = static_cast<int>((60000 + meanMs / 2) / meanMs);

    Project& p = host_.edit_project();
    const int clamped = std::min(999, std::max(20, bpm));
    if (p.tempo == clamped) return;   // no edit, so no dirty bump and no lookahead rollback
    p.tempo = clamped;
    mark_modified();
}

void InputDispatcher::settings_action() {
    switch (static_cast<SettingsRow>(s_.settingsCursorRow)) {
        case SettingsRow::THEME:
            // A opens the theme editor.
            open_theme_editor();
            break;

        case SettingsRow::TEMPLATE: {
            if (s_.settingsCursorColumn != 1 && s_.settingsCursorColumn != 2) break;
            const ActionResult r = (s_.settingsCursorColumn == 1) ? save_template(host_, fs_)
                                                                  : clear_template(fs_);
            s_.statusMessage = r.message;
            s_.statusSuccess = r.ok;
            break;
        }

        // Every other row is a value, changed with A+DPAD; single A is for actions only.
        default:
            break;
    }
}

}  // namespace pt::ui
