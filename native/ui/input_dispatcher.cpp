// The dispatcher's spine: the frame tick, loads, the autosave, the cursor, applying an edit, MIDI
// learn and the D-pad. Each screen's own buttons are in `ui/dispatch/`.

#include "ui/input_dispatcher.h"
#include "ui/dispatch/dispatch_common.h"

#include "ui/cursor_move.h"
#include "ui/helpers.h"        // dec2 — a MIDI channel is the one number shown in decimal
#include "ui/lifecycle.h"        // the crash-recovery autosave — write / clear / load
#include "ui/song_pointer.h"     // NAV = SONG — the pointer, the entry gate and the load-time clamp
#include "common/load_progress.h"       // begin_load / end_load — where the engine reports a slow load

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

// ─── The frame tick ──────────────────────────────────────────────────────────────────────────────

void InputDispatcher::set_now(long long now_ms) {
    now_ms_ = now_ms;
    run_due_sample_preview_restore();   // the sample editor's 100 ms audition restore
    run_due_autosave();                 // the crash-recovery autosave's 3 s debounce
    run_midi_hotplug();                 // a MIDI device plugged in or pulled out — before the dismiss,
                                        // so a message it posts is seen this frame
    run_due_status_dismiss();           // the status lines' auto-dismiss
    run_instrument_entry_push();        // the on-entry instrument param push
    run_selection_recency();            // which rung L+R takes first
    run_mapped_cc_dirty();              // a knob on the cable moved something in the song
    // What a live MIDI key plays: on PHRASE the instrument of the note under the cursor, elsewhere the
    // one last typed or picked; on the SONG cursor's track (remembered while another screen is up),
    // over KEYS' voices. Every frame — a cursor has no change notification; the host republishes only
    // on change.
    int keysInstrument = s_.lastEditedInstrument;
    if (s_.currentScreen == ScreenType::PHRASE) {
        const songcore::PhraseStep& step =
            s_.project->phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        if (step.note != Note::EMPTY() && step.instrument >= 0) keysInstrument = step.instrument;
    }
    keysInstrument = std::min(static_cast<int>(s_.project->instruments.size()) - 1, std::max(0, keysInstrument));
    host_.set_midi_in_play(keysInstrument, pointer_track(s_), s_.settings.midiInVoices,
                           s_.settings.midiVelocity);
    run_midi_learn();                   // …or, with R held, was pointed at the cell under the cursor
    // The channel the cable's knobs are on, copied once a frame for a screen built in two places.
    s_.midiInCcChannel = host_.last_cc_channel();
    const AudioEngine::BlockTiming bt = host_.block_timing();
    s_.audioLoad = AudioLoad{bt.blockFrames, bt.sampleRate, bt.meanLoad, bt.worstLoad};
}

// ─── A slow load ─────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::begin_load(long long now_ms, std::string detail) {
    before_long_operation();
    pt::begin_load();                 // clears the engine-side cancel flag; see load_progress.h
    s_.loading = AppState::LoadingState{};
    s_.loading.running = true;
    s_.loading.detail  = std::move(detail);
    loadStartMs_       = now_ms;
    lastLoadPaintMs_   = now_ms;   // the first paint is one cadence in, never on the opening report
}

bool InputDispatcher::load_tick(long long now_ms, float fraction) {
    if (!s_.loading.running) return true;   // a tick from a load nobody opened — nothing to draw on

    s_.loading.progress  = fraction;
    s_.loading.elapsedMs = static_cast<int>(now_ms - loadStartMs_);

    // ⚠️ The strip is raised HERE, never at `begin_load` (LOADING_DELAY_MS). Once up it stays up: a bar
    // vanishing because one file was quick is a flicker, not a report.
    if (s_.loading.elapsedMs >= LOADING_DELAY_MS) s_.loading.shown = true;

    // ⚠️⚠️ THROTTLED: a report arrives per sample header (~every 2.5 ms on a big `.sf3`) and each repaint
    // is a full redraw + present — unthrottled, the strip made the load it reports on far slower. The
    // cancel rides the same cadence, so a press is seen within a frame.
    if (now_ms - lastLoadPaintMs_ >= LOADING_REPAINT_MS) {
        lastLoadPaintMs_ = now_ms;

        // ⚠️ The pump first — it is also what keeps the app's lifecycle alive during a load
        // (`RenderHooks::load_pump`).
        if (render_.load_pump && render_.load_pump()) s_.loading.cancelRequested = true;

        // A song playing through the load keeps going: the frame loop that refills it is not running.
        before_long_operation();

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
        // Entering INSTRUMENT re-pushes the selected instrument's params — covering an instrument changed
        // anywhere the cursor was not (edits and loads push for themselves).
        // ⚠️ No refresh of sounding notes: arriving edits nothing, and a refresh would strip a table's or
        // phrase's filter off them mid-note.
        if (s_.currentScreen == ScreenType::INSTRUMENT)
            host_.push_instrument(std::min(127, std::max(0, s_.currentInstrument)), /*refreshSounding=*/false);
        lastScreenSeen_ = s_.currentScreen;
    }
}

// ─── The crash-recovery autosave ─────────────────────────────────────────────────────────────────

void InputDispatcher::run_due_autosave() {
    if (!autosavePending_ || now_ms_ < autosaveDueAtMs_) return;
    autosavePending_ = false;

    // ⚠️ RE-CHECK `project_dirty()`: a SAVE inside the 3 s window cleans the document and deletes the
    // autosave without cancelling this deadline (a save is not an edit). Firing anyway would put the file
    // back — and a spurious RECOVER WORK? on the next launch.
    if (!s_.project_dirty()) return;

    before_save();
    autosave_write(host_, fs_);   // a failure is silent — see lifecycle.h
}

void InputDispatcher::flush_autosave() {
    autosavePending_ = false;
    if (!s_.project_dirty()) return;
    before_save();
    autosave_write(host_, fs_);
}

// ─── The status lines' auto-dismiss ──────────────────────────────────────────────────────────────

namespace {

/**
 * One status field's watcher and deadline. A CHANGE in the message re-arms the window; a change TO
 * empty cancels it; re-setting an identical message does not extend it (the tests pin that). The
 * field is the funnel, so every assignment — present or future — gets the dismissal. Called per field,
 * never copied.
 */
void dismiss_status_field(std::string& message, bool& success, std::string& lastSeen,
                          long long& deadlineMs, long long nowMs, long long windowMs) {
    if (message != lastSeen) {
        lastSeen   = message;
        deadlineMs = message.empty() ? 0 : nowMs + windowMs;
    }

    if (deadlineMs != 0 && nowMs >= deadlineMs) {
        // The message goes, and success returns to true.
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

    // ⚠️ The BROWSER's own line, on the same clock — the browser draws its own bottom bar, and its
    // message would otherwise outlive the action it reported.
    dismiss_status_field(s_.fileBrowser.statusMessage, s_.fileBrowser.statusSuccess,
                         browserStatusLastSeen_, browserStatusDismissAtMs_, now_ms_,
                         STATUS_DISMISS_MS);
}

bool InputDispatcher::recover_from_autosave() {
    // ⚠️ A recovery reopens every source the crashed session had — a full project load, when the user
    // only answered a question. Same strip, same delay, same B. (On the AUTO path with no repaint hook,
    // nothing is drawn.)
    const LoadScope recoverScope(*this, now_ms_, "RECOVERED WORK");

    if (!autosave_load(host_, fs_, mediaBaseDir_)) {
        s_.statusMessage = "RECOVER FAILED";
        s_.statusSuccess = false;
        return false;
    }

    reset_editing_context();

    // ⚠️⚠️ A CANCELLED recovery returns TRUE: `confirm_accept` reads false as "bad file" and DELETES the
    // autosave. The document goes blank and the FILE STAYS, so the next launch offers it again.
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

    // ⚠️ DIRTY on purpose — the one load path that is. Recovered work lives only in the crash file; a
    // clean flag would call it safe. The next NEW or EXIT asks, nudging a save under a real name.
    // The debounce is not armed: the file it would write is the one just read.
    s_.projectVersion      = 1;
    s_.savedProjectVersion = 0;
    s_.projectPath.clear();   // the autosave is not a name the user can save over

    s_.statusMessage = "RECOVERED";
    s_.statusSuccess = true;
    return true;
}

InputDispatcher::BootRecovery InputDispatcher::boot_recovery() {
    if (!autosave_exists(fs_)) return BootRecovery::NONE;   // the last session ended cleanly

    if (!s_.settings.autosaveResumeAuto) {
        // ASK. The only dialog the user did not open, so it must be up before a keystroke can land
        // underneath (the confirm owns every button but A/B).
        // ⚠️ The file is NOT parsed first — that would charge every such launch for the recovery. A
        // corrupt one still asks, and A then fails and drops it (confirm_accept).
        s_.confirm.open(ConfirmDialogState::Kind::RECOVER);
        return BootRecovery::ASKED;
    }

    // AUTO. Restore in silence — on a handheld whose launcher kills the app at every menu, a prompt on
    // each return is noise.
    // ⚠️ A corrupt autosave is DROPPED (here and on the ASK path), or it would be retried — and fail — on
    // every launch.
    if (recover_from_autosave()) return BootRecovery::RESTORED;

    autosave_clear(fs_);
    return BootRecovery::DROPPED;
}

// ─── The cursor ──────────────────────────────────────────────────────────────────────────────────

int InputDispatcher::cursor_row() const {
    switch (s_.currentScreen) {
        case ScreenType::TABLE:  return s_.tableCursorRow;
        case ScreenType::GROOVE: return s_.grooveCursorRow;
        case ScreenType::SCALE:  return s_.scaleCursorRow;
        default:                 return s_.cursorRow;
    }
}

int InputDispatcher::cursor_column() const {
    switch (s_.currentScreen) {
        case ScreenType::TABLE:  return s_.tableCursorColumn;
        case ScreenType::GROOVE: return 1;
        case ScreenType::SCALE:  return 1;
        default:                 return s_.cursorColumn;
    }
}

void InputDispatcher::set_cursor_row(int row) {
    switch (s_.currentScreen) {
        case ScreenType::TABLE:  s_.tableCursorRow = row;  break;
        case ScreenType::GROOVE: s_.grooveCursorRow = row; break;
        case ScreenType::SCALE:  s_.scaleCursorRow = row;  break;
        default:                 s_.cursorRow = row;       break;
    }
}

int InputDispatcher::max_selection_column() const {
    switch (s_.currentScreen) {
        case ScreenType::PHRASE: return 9;
        case ScreenType::CHAIN:  return 2;
        case ScreenType::SONG:   return 8;
        case ScreenType::TABLE:  return 8;
        default:                 return 1;
    }
}

int InputDispatcher::max_selection_row() const {
    // SONG is 256 rows deep and shows 16: a SCREEN-scope selection there is the whole arrangement.
    return (s_.currentScreen == ScreenType::SONG) ? 255 : 15;
}

bool InputDispatcher::on_instrument_screen() const {
    return s_.currentScreen == ScreenType::INSTRUMENT ||
           s_.currentScreen == ScreenType::INST_POOL ||
           s_.currentScreen == ScreenType::MODS;
}

bool InputDispatcher::on_globals_screen() const {
    return s_.currentScreen == ScreenType::MIXER || s_.currentScreen == ScreenType::EFFECTS;
}

/** The GROOVE screen's state, assembled once for both the cursor context and the edit. */
GrooveState InputDispatcher::groove_state(const Project& p) const {
    GrooveState gs{p.grooves[static_cast<size_t>(s_.currentGroove)]};
    gs.cursorRow    = s_.grooveCursorRow;
    gs.cursorColumn = s_.grooveCursorColumn;
    gs.panelRow     = s_.groovePanelRow;
    gs.panelColumn  = s_.groovePanelColumn;
    gs.quantize     = s_.grooveQuantize;
    return gs;
}

CursorContext InputDispatcher::cursor_context() const {
    const Project& p = *s_.project;
    switch (s_.currentScreen) {
        case ScreenType::SONG: {
            SongEditorState ss{p};
            ss.cursorRow   = s_.cursorRow;
            ss.cursorTrack = s_.cursorColumn;  // on SONG the column IS the track
            return song_.cursor_context(ss);
        }
        case ScreenType::CHAIN: {
            ChainEditorState cs{p.chains[static_cast<size_t>(s_.currentChain)]};
            cs.cursorRow    = s_.cursorRow;
            cs.cursorColumn = s_.cursorColumn;
            return chain_.cursor_context(cs);
        }
        case ScreenType::PHRASE: {
            PhraseEditorState ps{p.phrases[static_cast<size_t>(s_.currentPhrase)]};
            ps.cursorRow        = s_.cursorRow;
            ps.cursorColumn     = s_.cursorColumn;
            ps.effectTypeCount  = visible_effect_type_count();
            // ⚠️ The CURSOR needs the project too: the NOTE cell's scale comes from it, and null reads as
            // chromatic with no error. `layout.cpp` sets its own, separate PhraseEditorState.
            ps.project          = &p;
            ps.insertInstrument = s_.lastEditedInstrument;
            return phrase_.cursor_context(ps);
        }
        case ScreenType::TABLE: {
            TableState ts{p.tables[static_cast<size_t>(s_.currentTable)]};
            ts.cursorRow       = s_.tableCursorRow;
            ts.cursorColumn    = s_.tableCursorColumn;
            ts.effectTypeCount = visible_effect_type_count();
            return table_.cursor_context(ts);
        }
        case ScreenType::GROOVE:
            return groove_.cursor_context(groove_state(p));
        case ScreenType::SCALE: {
            ScaleState cs{p.scales[static_cast<size_t>(s_.currentScale)]};
            cs.key          = p.scaleKey;
            cs.cursorRow    = s_.scaleCursorRow;
            cs.cursorColumn = s_.scaleCursorColumn;
            return scale_.cursor_context(cs);
        }

        case ScreenType::INSTRUMENT: {
            InstrumentEditorState is{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
            is.cursorRow     = s_.instrumentCursorRow;
            is.cursorColumn  = s_.instrumentCursorColumn;
            // The PRESET row's range is the SF2's own list length.
            is.sfPresetName  = s_.sfPresetName;
            is.sfPresetCount = s_.sfPresetCount;
            is.sfPresetIndex = s_.sfPresetIndex;
            is.allowOscLoop  = s_.caps.loopWindow;
            return instrument_.cursor_context(is);
        }

        case ScreenType::INST_POOL: {
            InstrumentPoolState ps{p};
            ps.selectedInstrument = s_.currentInstrument;
            ps.cursorColumn       = s_.poolCursorColumn;
            return pool_.cursor_context(ps);
        }

        case ScreenType::MODS: {
            ModulationState ms{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
            ms.cursorRow  = s_.modCursorRow;
            ms.cursorPair = s_.modCursorPair;
            ms.cursorSide = s_.modCursorSide;
            return mods_.cursor_context(ms);
        }

        case ScreenType::MIXER: {
            MixerState ms{p};
            ms.cursorColumn   = s_.mixerCursorColumn;
            ms.mixerMasterRow = s_.mixerMasterRow;
            return mixer_.cursor_context(ms);
        }

        case ScreenType::EFFECTS: {
            EffectState es{p};
            es.cursorRow = s_.effectsCursorRow;
            return effects_.cursor_context(es);
        }

        case ScreenType::PROJECT: {
            ProjectState prs{p};
            prs.cursorRow    = s_.projectCursorRow;
            prs.cursorColumn = s_.projectCursorColumn;
            prs.caps         = s_.caps;
            return project_.cursor_context(prs);
        }

        case ScreenType::SETTINGS: {
            SettingsState ss{s_.settings};
            ss.cursorRow    = s_.settingsCursorRow;
            ss.cursorColumn = s_.settingsCursorColumn;
            ss.caps         = s_.caps;
            ss.theme        = s_.theme;   // VISUALIZER's value lives on the theme
            return settings_.cursor_context(ss);
        }

        case ScreenType::MIDI: {
            MidiState ms{*s_.project, s_.settings, s_.midiDeviceNames, s_.midiInDeviceNames};
            ms.lastCcChannel  = s_.midiInCcChannel;
            ms.cursorRow      = s_.midiCursorRow;
            ms.cursorColumn   = s_.midiCursorColumn;
            ms.deviceIndex    = s_.midiDeviceIndex;
            ms.inDeviceIndex  = s_.midiInDeviceIndex;
            ms.autoOffsetMs   = s_.midiAutoOffsetMs;
            ms.caps           = s_.caps;
            return midi_.cursor_context(ms);
        }

        case ScreenType::MIDI_MAP: {
            MidiMapState mm{p};
            mm.cursorRow    = s_.midiMapCursorRow;
            mm.cursorColumn = s_.midiMapCursorColumn;
            return midiMap_.cursor_context(mm);
        }

        case ScreenType::SAMPLE_EDITOR:
            return sample_.cursor_context(s_.sampleEditor);

        default:
            return cc::none();  // a placeholder screen has nothing to edit
    }
}

// ─── Applying an edit ────────────────────────────────────────────────────────────────────────────

bool InputDispatcher::apply_edit(const InputAction& action) {
    Project& p = host_.edit_project();  // the SAME Project the Sequencer reads

    switch (s_.currentScreen) {
        case ScreenType::SONG: {
            const SongInputResult r = song_.handle_input(p, s_.cursorRow, s_.cursorColumn, action);
            if (r.hasChain) s_.lastEditedChain = r.lastEditedChain;
            return r.modified;
        }

        case ScreenType::CHAIN: {
            const ChainInputResult r = chain_.handle_input(
                p.chains[static_cast<size_t>(s_.currentChain)], s_.cursorRow, s_.cursorColumn, action);
            if (r.hasPhrase)    s_.lastEditedPhrase    = r.lastEditedPhrase;
            if (r.hasTranspose) s_.lastEditedTranspose = r.lastEditedTranspose;
            return r.modified;
        }

        case ScreenType::PHRASE: {
            Phrase& ph = p.phrases[static_cast<size_t>(s_.currentPhrase)];
            const PhraseInputResult r = phrase_.handle_input(ph, s_.cursorRow, s_.cursorColumn, action);
            if (!r.modified) return false;

            // "Last edited" and the audition: only a step WITH a note is remembered, and only a NOTE
            // edit auditions — dialling a velocity must not retrigger the voice.
            const songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];
            if ((r.hasNote || r.hasVolume || r.hasInstrument) && step.note != Note::EMPTY()) {
                s_.lastEditedNote       = step.note;
                s_.lastEditedVolume     = step.volume;
                s_.lastEditedInstrument = step.instrument;
                if (r.hasNote) preview_held_note();
            }
            // A+B under a held audition: the note is gone, so is the sound.
            if (heldNotePreview_ && step.note == Note::EMPTY()) {
                heldNotePreview_ = false;
                host_.stop_preview(/*cut=*/true);
            }
            return true;
        }

        case ScreenType::TABLE:
            return table_
                .handle_input(p.tables[static_cast<size_t>(s_.currentTable)], s_.tableCursorRow,
                              s_.tableCursorColumn, action)
                .modified;

        case ScreenType::GROOVE: {
            const GrooveInputResult r = groove_.handle_input(
                p.grooves[static_cast<size_t>(s_.currentGroove)], groove_state(p), action);
            // ⚠️ The quantize pointer is not song data: it comes back separately, and moving it must
            // not dirty the project or arm an autosave.
            if (r.newQuantize >= 0) s_.grooveQuantize = r.newQuantize;
            return r.modified;
        }

        case ScreenType::SCALE: {
            // ⚠️ The KEY row edits the PROJECT, not the scale handed in; the module returns the new key
            // rather than holding a Project.
            const ScaleInputResult r = scale_.handle_input(
                p.scales[static_cast<size_t>(s_.currentScale)], p.scaleKey, s_.scaleCursorRow,
                s_.scaleCursorColumn, action);
            if (r.newKey >= 0) p.scaleKey = r.newKey;
            return r.modified;
        }

        case ScreenType::INSTRUMENT: {
            const InstrumentInputResult r = instrument_.handle_input(
                p.instruments[static_cast<size_t>(s_.currentInstrument)], s_.instrumentCursorRow,
                s_.instrumentCursorColumn, action);

            // The PRESET row: the bank+preset behind an index live in the SF2's list, which only the
            // engine has opened — resolved here so the module stays a pure function of the Project.
            if (r.presetIndexChanged) host_.set_sf_preset_by_index(s_.currentInstrument, r.presetIndex);
            return r.modified;
        }

        case ScreenType::INST_POOL:
            return pool_.handle_input(p.instruments[static_cast<size_t>(s_.currentInstrument)],
                                      s_.poolCursorColumn, action);

        case ScreenType::MODS: {
            ModulationState ms{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
            ms.cursorPair = s_.modCursorPair;
            ms.cursorSide = s_.modCursorSide;
            return mods_
                .handle_input(p.instruments[static_cast<size_t>(s_.currentInstrument)],
                              ms.active_slot_index(), s_.modCursorRow, action)
                .modified;
        }

        // MIXER and EFFECTS take the whole PROJECT: their fields are scattered across it.
        case ScreenType::MIXER:
            return mixer_.handle_input(p, s_.mixerMasterRow, s_.mixerCursorColumn, action).modified;

        case ScreenType::EFFECTS:
            return effects_.handle_input(p, s_.effectsCursorRow, action).modified;

        case ScreenType::PROJECT:
            return project_
                .handle_input(p, s_.projectCursorRow, s_.projectCursorColumn, action)
                .modified;

        // ⚠️ SETTINGS edits the SETTINGS, not the project — `false`, so no mark_modified(): a visualizer
        // change must not make a song dirty or prompt at NEW / EXIT. The shell writes settings.json.
        case ScreenType::SETTINGS: {
            const bool navBefore = s_.settings.navSongRelative;
            settings_.handle_input(s_.settings, s_.theme, s_.caps, s_.settingsCursorRow,
                                   s_.settingsCursorColumn, action);
            // ⚠️ Turning NAV on must land the pointer on a real cell, or the first R+RIGHT is refused
            // silently.
            if (!navBefore && s_.settings.navSongRelative) clamp_song_pointer(s_);
            return false;
        }

        // ⚠️ MIDI edits BOTH: PROG CHG is a Project field (dirties the song, like TEMPO); OUTPUT, INPUT
        // and OFFSET are settings.json's and must not.
        case ScreenType::MIDI: {
            const MidiInputResult r =
                midi_.handle_input(p, s_.settings, s_.midiCursorRow, s_.midiCursorColumn,
                                   s_.midiDeviceNames, s_.midiInDeviceNames, action);
            // The side effects the module cannot perform — it has no port.
            if (r.deviceChanged)   apply_midi_device();
            if (r.inDeviceChanged) apply_midi_in_device();
            if (r.offsetChanged)   host_.set_midi_offset_ms(
                                       midi_offset_in_force(s_.settings, s_.midiAutoOffsetMs));
            if (r.syncChanged)     host_.set_midi_sync_out(s_.settings.midiSyncOut);
            if (r.controlChannelChanged)
                host_.set_midi_control_channel(s_.settings.midiControlChannel);
            return r.projectModified;
        }

        // ⚠️ Mappings are the SONG's, so every edit dirties it. Nothing is pushed: a mapping moves no
        // parameter until a knob turns.
        case ScreenType::MIDI_MAP: {
            const MidiMapInputResult r = midiMap_.handle_input(p, s_.midiMapCursorRow,
                                                               s_.midiMapCursorColumn, action);
            // A delete leaves the cursor one past the end — the ADD row, where it should land.
            if (r.rowDeleted) clamp_midi_map_cursor();
            return r.modified;
        }

        case ScreenType::SAMPLE_EDITOR: {
            const SampleEditorInputResult r = sample_.handle_input(s_.sampleEditor, action);
            if (r.rateModeChanged || r.bitDepthChanged) apply_sample_rate_and_bits();

            // ⚠️ `false`: the editor's session (zoom, selection, slice index, pending pitch) is not the
            // document. `true` would dirty the song and arm an autosave on a held A+UP on ZOOM.
            // RATE and BIT, which DO rebuild the buffer, push for themselves
            // (`apply_sample_rate_and_bits()`).
            return false;
        }

        default:
            return false;
    }
}

void InputDispatcher::mark_dirty_and_arm_autosave() {
    // The document changed: bumped here, in the one place every edit funnels through, so "is this
    // project dirty?" has a single answer. SAVE / LOAD / NEW align the saved version to it.
    // ⚠️ One counter, ONE job — never bump it to force a redraw, or a settings change writes a crash
    // autosave for a song with no edits.
    s_.projectVersion++;

    // ── Arm the autosave's debounce ──────────────────────────────────────────────────────────────
    //
    // ⚠️ RE-ARMED, so the write lands 3 s after the LAST edit (a held A+UP edits every 100 ms). Dirty and
    // armed travel together — hence one function; a bare `projectVersion++` is a dirty document with no
    // crash protection.
    autosavePending_ = true;
    autosaveDueAtMs_ = now_ms_ + AUTOSAVE_DEBOUNCE_MS;
}

void InputDispatcher::mark_modified(bool table_touched) {
    mark_dirty_and_arm_autosave();

    // ⚠️ The consumer caches which tables it pushed; an IN-PLACE edit must invalidate it itself.
    if (table_touched || s_.currentScreen == ScreenType::TABLE) host_.invalidate_tables();

    // ⚠️ An instrument's params are ENGINE STATE a voice reads as it runs (drive, crush, filter, window,
    // loop); no event carries them. Turning a filter under a ringing pad must be heard.
    if (on_instrument_screen()) host_.push_instrument(s_.currentInstrument);

    // MIXER and EFFECTS edit state the engine holds on its own (mixer, master bus, sends, master EQ).
    // Pushed wholesale, so a deleted EQ slot (−1, the engine's bypass) reaches the engine too.
    if (on_globals_screen()) host_.push_globals();
}

void InputDispatcher::run_mapped_cc_dirty() {
    const uint64_t writes = host_.mapped_cc_writes();
    if (writes == mappedCcSeen_) return;
    mappedCcSeen_ = writes;

    // ⚠️ Once a FRAME, not once a message (a sweep writes ~30 a second); the dirty flag still lands
    // within a frame of the first message, so the "unsaved work" question knows.
    mark_dirty_and_arm_autosave();
}

// ─── MIDI learn — hold R, turn a knob ────────────────────────────────────────────────────────────

void InputDispatcher::on_r_held(bool down) { host_.set_midi_learn_armed(down); }

songcore::MapTarget InputDispatcher::map_target() const {
    const Project& p = *s_.project;

    // ⚠️ A modal owns the screen; the cursor underneath is not what the user aims at. A knob is the one
    // "press" that bypasses the mapper.
    if (modal_backdrop_active(s_) || s_.eq.isOpen) return {};

    switch (s_.currentScreen) {
        case ScreenType::MIXER: {
            MixerState ms{p};
            ms.cursorColumn   = s_.mixerCursorColumn;
            ms.mixerMasterRow = s_.mixerMasterRow;
            return mixer_.map_target(ms);
        }

        case ScreenType::EFFECTS: {
            EffectState es{p};
            es.cursorRow = s_.effectsCursorRow;
            return effects_.map_target(es);
        }

        case ScreenType::INSTRUMENT: {
            InstrumentEditorState is{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
            is.cursorRow    = s_.instrumentCursorRow;
            is.cursorColumn = s_.instrumentCursorColumn;
            // The module holds the instrument by reference and never knew its number — finished here.
            songcore::MapTarget t = instrument_.map_target(is);
            t.scope = static_cast<uint8_t>(s_.currentInstrument);
            return t;
        }

        // Every other screen names no parameter a knob sweeps.
        default:
            return {};
    }
}

void InputDispatcher::run_midi_learn() {
    const uint64_t events = host_.midi_learn_events();
    if (events == learnSeen_) return;
    learnSeen_ = events;

    // ⚠️⚠️ The knob arrived on a channel nothing listens to — say WHICH. `CTL CH` is OFF by default (a
    // guess would steal CCs the tracks route), so a fresh install cannot learn until it is set, and
    // only the cable knows the number.
    if (!songcore::ctl_ch_covers(host_.midi_control_channel(), host_.midi_learn_channel())) {
        s_.statusMessage = "KNOB ON CH " + dec2(host_.midi_learn_channel() + 1) + " - SET CTL CH";
        s_.statusSuccess = false;
        return;
    }

    const songcore::MapTarget target = map_target();
    if (!target.named()) {
        // ⚠️ Said out loud: a knob that learns nothing looks exactly like one not plugged in.
        s_.statusMessage = "NOTHING HERE TO MAP";
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

    // ⚠️ The SCOPE is named in the list's own numbering (track from 1, instrument in hex), or the user
    // cannot tell which instrument the knob now owns.
    const songcore::MapDest* d = songcore::map_dest(target.id);
    std::string where = d ? d->name : "?";
    if (d && d->scope == songcore::MapScope::TRACK)
        where += " " + std::to_string(target.scope + 1);
    else if (d && d->scope == songcore::MapScope::INSTRUMENT)
        where += " " + songcore::hex2(target.scope);
    s_.statusMessage = "MAPPED " + songcore::hex2(cc) + " > " + where;
    s_.statusSuccess = true;

    // ⚠️ Dirty, and nothing pushed — a mapping moves nothing until a knob turns.
    mark_dirty_and_arm_autosave();
}

int InputDispatcher::remembered_song_track() const { return s_.songCursorColumn - 1; }

int InputDispatcher::audition_track() const {
    const Project& p = *s_.project;
    const int track = remembered_song_track();
    if (track < 0 || track >= static_cast<int>(p.tracks.size())) return -1;
    const std::vector<int>& refs = p.tracks[static_cast<size_t>(track)].chainRefs;
    // ⚠️ A track's chainRefs may be SHORTER than the 256-row screen.
    if (s_.songCursorRow < 0 || s_.songCursorRow >= static_cast<int>(refs.size())) return -1;
    return refs[static_cast<size_t>(s_.songCursorRow)] >= 0 ? track : -1;
}

void InputDispatcher::preview_held_note() {
    if (!s_.settings.notePreviewEnabled || s_.selection.active) return;
    if (s_.currentScreen != ScreenType::PHRASE || s_.cursorColumn != 1) return;
    if (host_.is_playing()) return;   // over a playing song it clashes with the track's own notes

    const Project& p = *s_.project;
    const songcore::PhraseStep& step =
        p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
    if (step.note == Note::EMPTY()) return;

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
    if (!s_.selection.active) {
        generic_input(fn);
        return;
    }
    // Every row of the selection through the SAME path, the cursor walked down and put back — so a
    // column can never behave differently under a selection.
    const SelectionBounds b       = s_.selection.bounds();
    const int             savedRow = cursor_row();
    bool                  any      = false;

    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
        case ScreenType::CHAIN:
        case ScreenType::SONG:
        case ScreenType::TABLE:
            for (int row = b.topLeftRow; row <= b.bottomRightRow; ++row) {
                set_cursor_row(row);
                const InputAction action = fn(cursor_context());
                if (action.type != ActionType::NONE && apply_edit(action)) any = true;
            }
            set_cursor_row(savedRow);
            if (any) mark_modified();
            break;

        default:
            generic_input(fn);
            break;
    }
}

void InputDispatcher::dpad_nav(NavDir direction) {
    if (s_.selection.active) {
        const CursorPosition edgeBefore = s_.selection.end;
        s_.selection.expand(direction, max_selection_row(), max_selection_column());

        // Drag the CURSOR with the selection's active edge so it stays on screen (a SONG selection past
        // row 16). Only when the edge MOVED, so a clamp or SCREEN scope cannot teleport it.
        const CursorPosition edge = s_.selection.end;
        if (edge != edgeBefore) {
            const ScreenType sc = s_.currentScreen;
            if (sc == ScreenType::PHRASE || sc == ScreenType::CHAIN || sc == ScreenType::SONG) {
                s_.cursorRow    = edge.row;
                s_.cursorColumn = edge.column;
                if (sc == ScreenType::SONG) scroll_song_to_row(s_, edge.row);
            } else if (sc == ScreenType::TABLE) {
                s_.tableCursorRow    = edge.row;
                s_.tableCursorColumn = edge.column;
            }
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

// ─── D-pad alone ─────────────────────────────────────────────────────────────────────────────────
//
// ⚠️ THE MODAL RULE (input_dispatcher.h): the layers first, then the screen's handler, then this.

void InputDispatcher::on_dpad_up() {
    if (route(Gesture::DPAD_UP)) return;
    if (overlay_swallows(Overlay::NONE)) return;
    dpad_nav(NavDir::UP);
}

void InputDispatcher::on_dpad_down() {
    if (route(Gesture::DPAD_DOWN)) return;
    if (overlay_swallows(Overlay::NONE)) return;
    dpad_nav(NavDir::DOWN);
}

void InputDispatcher::on_dpad_left() {
    if (route(Gesture::DPAD_LEFT)) return;
    if (overlay_swallows(Overlay::NONE)) return;
    dpad_nav(NavDir::LEFT);
}

void InputDispatcher::on_dpad_right() {
    if (route(Gesture::DPAD_RIGHT)) return;
    if (overlay_swallows(Overlay::NONE)) return;
    dpad_nav(NavDir::RIGHT);
}

}  // namespace pt::ui
