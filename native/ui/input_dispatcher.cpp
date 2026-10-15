// The dispatcher's spine: the frame tick, loads, the autosave, the cursor, applying an edit, MIDI
// learn and the D-pad. Each screen's own buttons are in `ui/dispatch/`.

#include "ui/input_dispatcher.h"
#include "ui/dispatch/dispatch_common.h"

#include "ui/cursor_move.h"
#include "ui/helpers.h"
#include "ui/lifecycle.h"
#include "ui/song_pointer.h"
#include "common/load_progress.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

// ─── The frame tick ──────────────────────────────────────────────────────────────────────────────

void InputDispatcher::set_now(long long now_ms) {
    now_ms_ = now_ms;
    run_due_sample_preview_restore();
    run_due_autosave();
    run_midi_hotplug();                 // before the dismiss, so its message is seen this frame
    run_due_status_dismiss();
    run_instrument_entry_push();
    run_selection_recency();
    run_mapped_cc_dirty();
    // What a live MIDI key plays: on PHRASE the instrument of the note under the cursor, else the one
    // last picked, on the SONG cursor's track. Every frame; the host republishes only on a change.
    int keysInstrument = s_.lastEditedInstrument;
    if (s_.currentScreen == ScreenType::PHRASE) {
        const songcore::PhraseStep& step =
            s_.project->phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        if (songcore::step_has_note(step) && step.instrument >= 0) keysInstrument = step.instrument;
    }
    keysInstrument = std::min(static_cast<int>(s_.project->instruments.size()) - 1, std::max(0, keysInstrument));
    host_.set_midi_in_play(keysInstrument, pointer_track(s_), s_.settings.midiInVoices,
                           s_.settings.midiVelocity);
    run_midi_learn();
    s_.midiInCcChannel = host_.last_cc_channel();
    const AudioEngine::BlockTiming bt = host_.block_timing();
    s_.audioLoad = AudioLoad{bt.blockFrames, bt.sampleRate, bt.meanLoad, bt.worstLoad};
}

// ─── A slow load ─────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::begin_load(long long now_ms, std::string detail) {
    before_long_operation();
    pt::begin_load();                 // clears the engine-side cancel flag
    s_.loading = AppState::LoadingState{};
    s_.loading.running = true;
    s_.loading.detail  = std::move(detail);
    loadStartMs_       = now_ms;
    lastLoadPaintMs_   = now_ms;   // the first paint is one cadence in
}

bool InputDispatcher::load_tick(long long now_ms, float fraction) {
    if (!s_.loading.running) return true;

    s_.loading.progress  = fraction;
    s_.loading.elapsedMs = static_cast<int>(now_ms - loadStartMs_);

    // Once up the strip stays up.
    if (s_.loading.elapsedMs >= LOADING_DELAY_MS) s_.loading.shown = true;

    // ⚠️ Throttled: reports come every few ms, and a repaint each time slowed the load itself.
    if (now_ms - lastLoadPaintMs_ >= LOADING_REPAINT_MS) {
        lastLoadPaintMs_ = now_ms;

        // The pump also keeps the app's lifecycle events flowing during a load.
        if (render_.load_pump && render_.load_pump()) s_.loading.cancelRequested = true;

        before_long_operation();   // keep a playing song fed

        if (s_.loading.shown && render_.repaint) render_.repaint();
    }

    return !s_.loading.cancelRequested;
}

void InputDispatcher::end_load() {
    pt::end_load();
    s_.loading = AppState::LoadingState{};
}

void InputDispatcher::run_instrument_entry_push() {
    if (s_.currentScreen != lastScreenSeen_) {
        // ⚠️ No refresh of sounding notes: arriving edits nothing, and a refresh would strip a table's
        // or phrase's filter off them mid-note.
        if (s_.currentScreen == ScreenType::INSTRUMENT)
            host_.push_instrument(std::min(127, std::max(0, s_.currentInstrument)), /*refreshSounding=*/false);
        lastScreenSeen_ = s_.currentScreen;
    }
}

// ─── The crash-recovery autosave ─────────────────────────────────────────────────────────────────

void InputDispatcher::run_due_autosave() {
    if (!autosavePending_ || now_ms_ < autosaveDueAtMs_) return;
    autosavePending_ = false;

    // ⚠️ A save inside the window does not cancel the deadline; firing anyway would bring the file
    // back and a false RECOVER WORK? with it.
    if (!s_.project_dirty()) return;

    before_save();
    autosave_write(host_, fs_);   // a failure is silent
}

void InputDispatcher::flush_autosave() {
    autosavePending_ = false;
    if (!s_.project_dirty()) return;
    before_save();
    autosave_write(host_, fs_);
}

// ─── The status lines' auto-dismiss ──────────────────────────────────────────────────────────────

namespace {

// One status field's watcher: a new message re-arms the window, an empty one cancels it, an identical
// one does not extend it.
void dismiss_status_field(std::string& message, bool& success, std::string& lastSeen,
                          long long& deadlineMs, long long nowMs, long long windowMs) {
    if (message != lastSeen) {
        lastSeen   = message;
        deadlineMs = message.empty() ? 0 : nowMs + windowMs;
    }

    if (deadlineMs != 0 && nowMs >= deadlineMs) {
        message.clear();
        lastSeen.clear();
        success    = true;
        deadlineMs = 0;
    }
}

}  // namespace

void InputDispatcher::run_due_status_dismiss() {
    dismiss_status_field(s_.statusMessage, s_.statusSuccess, statusLastSeen_, statusDismissAtMs_,
                         now_ms_, STATUS_DISMISS_MS);

    dismiss_status_field(s_.fileBrowser.statusMessage, s_.fileBrowser.statusSuccess,
                         browserStatusLastSeen_, browserStatusDismissAtMs_, now_ms_,
                         STATUS_DISMISS_MS);
}

bool InputDispatcher::recover_from_autosave() {
    // A full project load, so the same strip and the same cancel.
    const LoadScope recoverScope(*this, now_ms_, "RECOVERED WORK");

    if (!autosave_load(host_, fs_, mediaBaseDir_)) {
        s_.statusMessage = "RECOVER FAILED";
        s_.statusSuccess = false;
        return false;
    }

    reset_editing_context();

    // ⚠️ A cancelled recovery returns true: false reads as "bad file" and deletes the autosave. The
    // document goes blank and the file stays, so the next launch offers it again.
    if (pt::load_cancelled()) {
        host_.new_project();
        host_.push_params();
        reset_editing_context();
        s_.projectVersion      = 0;
        s_.savedProjectVersion = 0;
        s_.projectPath.clear();
        s_.statusMessage = "RECOVER CANCELLED";
        s_.statusSuccess = true;
        return true;
    }

    // Dirty on purpose: recovered work lives only in the crash file. The autosave is not armed — the
    // file it would write is the one just read.
    s_.projectVersion      = 1;
    s_.savedProjectVersion = 0;
    s_.projectPath.clear();

    s_.statusMessage = "RECOVERED";
    s_.statusSuccess = true;
    return true;
}

InputDispatcher::BootRecovery InputDispatcher::boot_recovery() {
    if (!autosave_exists(fs_)) return BootRecovery::NONE;

    if (!s_.settings.autosaveResumeAuto) {
        // ASK, without parsing first; a corrupt file fails on A and is dropped then.
        s_.confirm.open(ConfirmDialogState::Kind::RECOVER);
        return BootRecovery::ASKED;
    }

    // AUTO restores in silence. A corrupt autosave is dropped, or every launch would retry it.
    if (recover_from_autosave()) return BootRecovery::RESTORED;

    autosave_clear(fs_);
    return BootRecovery::DROPPED;
}

// ─── The cursor ──────────────────────────────────────────────────────────────────────────────────

bool InputDispatcher::on_instrument_screen() const {
    return s_.currentScreen == ScreenType::INSTRUMENT ||
           s_.currentScreen == ScreenType::INST_POOL ||
           s_.currentScreen == ScreenType::MODS;
}

bool InputDispatcher::on_globals_screen() const {
    // `isOpen`, not `send_editor_up`: a preset loaded through the editor's browser must reach the engine.
    return s_.currentScreen == ScreenType::MIXER || s_.currentScreen == ScreenType::EFFECTS ||
           s_.sendEditor.isOpen;
}

CursorContext InputDispatcher::cursor_context() const {
    const ScreenHandler* h = screen();
    if (!h || !h->context) return cc::none();
    return (this->*h->context)();
}

// ─── Applying an edit ────────────────────────────────────────────────────────────────────────────

bool InputDispatcher::apply_edit(const InputAction& action) {
    const ScreenHandler* h = screen();
    return h && h->edit && (this->*h->edit)(action);
}

void InputDispatcher::mark_dirty_and_arm_autosave() {
    // ⚠️ "The document changed" and nothing else — never bump it to force a redraw.
    s_.projectVersion++;

    autosavePending_ = true;   // re-armed: it writes 3 s after the last edit
    autosaveDueAtMs_ = now_ms_ + AUTOSAVE_DEBOUNCE_MS;
}

void InputDispatcher::mark_modified(bool table_touched) {
    mark_dirty_and_arm_autosave();

    // ⚠️ The engine caches the tables it was given; an in-place edit must invalidate them.
    if (table_touched || s_.currentScreen == ScreenType::TABLE) host_.invalidate_tables();

    // ⚠️ An instrument's params are engine state a sounding voice reads; no event carries them.
    if (on_instrument_screen()) host_.push_instrument(s_.currentInstrument);

    // The mixer, master bus and sends, pushed whole so a deleted EQ slot reaches the engine too.
    if (on_globals_screen()) host_.push_globals();
}

void InputDispatcher::run_mapped_cc_dirty() {
    const uint64_t writes = host_.mapped_cc_writes();
    if (writes == mappedCcSeen_) return;
    mappedCcSeen_ = writes;

    mark_dirty_and_arm_autosave();   // once a frame, however many messages
}

// ─── MIDI learn — hold R, turn a knob ────────────────────────────────────────────────────────────

void InputDispatcher::on_r_held(bool down) { host_.set_midi_learn_armed(down); }

songcore::MapTarget InputDispatcher::map_target() const {
    // ⚠️ Under a modal the cursor underneath is not the target — a knob bypasses the mapper. The send
    // editor's cells are EFFECTS' own, so it maps.
    if (send_editor_on_top()) {
        EffectState es{*s_.project};
        es.cursorRow = s_.sendEditor.cursorRow;
        return effects_.map_target(es);
    }
    if (modal_backdrop_active(s_) || s_.eq.isOpen) return {};

    const ScreenHandler* h = screen();
    if (!h || !h->knob) return {};
    return (this->*h->knob)();
}

void InputDispatcher::run_midi_learn() {
    const uint64_t events = host_.midi_learn_events();
    if (events == learnSeen_) return;
    learnSeen_ = events;

    // The knob is on a channel CTL CH does not cover (it is OFF by default): say which.
    if (!songcore::ctl_ch_covers(host_.midi_control_channel(), host_.midi_learn_channel())) {
        s_.statusMessage = "KNOB ON CH " + dec2(host_.midi_learn_channel() + 1) + " - SET CTL CH";
        s_.statusSuccess = false;
        return;
    }

    const songcore::MapTarget target = map_target();
    if (!target.named()) {
        s_.statusMessage = "NOTHING HERE TO MAP";   // or it looks like a knob not plugged in
        s_.statusSuccess = false;
        return;
    }

    const int cc  = host_.midi_learn_controller();
    const int row = songcore::learn_mapping(*s_.project, target, cc);
    if (row < 0) {
        s_.statusMessage = "MAPPING LIST FULL";
        s_.statusSuccess = false;
        return;
    }

    // The scope in the list's own numbering (track from 1, instrument in hex).
    const songcore::MapDest* d = songcore::map_dest(target.id);
    std::string where = d ? d->name : "?";
    if (d && d->scope == songcore::MapScope::TRACK)
        where += " " + std::to_string(target.scope + 1);
    else if (d && d->scope == songcore::MapScope::INSTRUMENT)
        where += " " + songcore::hex2(target.scope);
    s_.statusMessage = "MAPPED " + songcore::hex2(cc) + " > " + where;
    s_.statusSuccess = true;

    mark_dirty_and_arm_autosave();   // nothing to push: a mapping moves nothing until a knob turns
}

int InputDispatcher::remembered_song_track() const { return s_.songCursorColumn - 1; }

int InputDispatcher::audition_track() const {
    const Project& p = *s_.project;
    const int track = remembered_song_track();
    if (track < 0 || track >= static_cast<int>(p.tracks.size())) return -1;
    const std::vector<int>& refs = p.tracks[static_cast<size_t>(track)].chainRefs;
    // ⚠️ chainRefs may be shorter than the 256-row screen.
    if (s_.songCursorRow < 0 || s_.songCursorRow >= static_cast<int>(refs.size())) return -1;
    return refs[static_cast<size_t>(s_.songCursorRow)] >= 0 ? track : -1;
}

void InputDispatcher::preview_held_note() {
    if (!s_.settings.notePreviewEnabled || s_.selection.active) return;
    if (s_.currentScreen != ScreenType::PHRASE || s_.cursorColumn != 1) return;
    if (host_.is_playing()) return;

    const Project& p = *s_.project;
    const songcore::PhraseStep& step =
        p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
    if (!songcore::step_has_note(step)) return;

    host_.set_preview_track(audition_track());
    host_.preview_note(std::min(std::max(step.instrument, 0), 127), step.note, /*durationFrames=*/0);
    heldNotePreview_ = true;
}

// ─── The three generic paths ─────────────────────────────────────────────────────────────────────

void InputDispatcher::generic_input(InputAction (*fn)(const CursorContext&)) {
    const InputAction action = fn(cursor_context());
    if (action.type == ActionType::NONE) return;
    if (apply_edit(action)) mark_modified();
}

void InputDispatcher::selection_or_single(InputAction (*fn)(const CursorContext&)) {
    const GridCursor* g = grid();
    if (!s_.selection.active || !g) {
        generic_input(fn);
        return;
    }
    // Every row through the same path, the cursor walked down and put back.
    const SelectionBounds b        = s_.selection.bounds();
    int&                  row      = s_.*g->row;
    const int             savedRow = row;
    bool                  any      = false;

    for (int r = b.topLeftRow; r <= b.bottomRightRow; ++r) {
        row = r;
        const InputAction action = fn(cursor_context());
        if (action.type != ActionType::NONE && apply_edit(action)) any = true;
    }
    row = savedRow;
    if (any) mark_modified();
}

void InputDispatcher::dpad_nav(NavDir direction) {
    const GridCursor* g = grid();
    if (s_.selection.active && g) {
        const CursorPosition edgeBefore = s_.selection.end;
        s_.selection.expand(direction, g->maxRow, g->maxColumn);

        // The cursor follows the active edge, only when it moved.
        const CursorPosition edge = s_.selection.end;
        if (edge != edgeBefore) {
            s_.*g->row    = edge.row;
            s_.*g->column = edge.column;
            if (s_.currentScreen == ScreenType::SONG) scroll_song_to_row(s_, edge.row);
        }
        return;
    }

    switch (direction) {
        case NavDir::UP:    move_cursor_up(s_);    break;
        case NavDir::DOWN:  move_cursor_down(s_);  break;
        case NavDir::LEFT:  move_cursor_left(s_);  break;
        case NavDir::RIGHT: move_cursor_right(s_); break;
    }
}

// ─── D-pad alone: the layers, then the screen, then this ─────────────────────────────────────────

void InputDispatcher::on_dpad_up() {
    if (route(Gesture::DPAD_UP)) return;
    dpad_nav(NavDir::UP);
}

void InputDispatcher::on_dpad_down() {
    if (route(Gesture::DPAD_DOWN)) return;
    dpad_nav(NavDir::DOWN);
}

void InputDispatcher::on_dpad_left() {
    if (route(Gesture::DPAD_LEFT)) return;
    dpad_nav(NavDir::LEFT);
}

void InputDispatcher::on_dpad_right() {
    if (route(Gesture::DPAD_RIGHT)) return;
    dpad_nav(NavDir::RIGHT);
}

}  // namespace pt::ui
