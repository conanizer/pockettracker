// The dispatcher's spine: the frame tick, loads, the autosave, the cursor, applying an edit, MIDI
// learn and the D-pad. Each screen's own buttons are in `ui/dispatch/`.

#include "ui/input_dispatcher.h"
#include "ui/dispatch/dispatch_common.h"

#include "ui/cursor_move.h"
#include "ui/helpers.h"        // dec2 — a MIDI channel is the one number this app shows in decimal
#include "ui/lifecycle.h"        // the crash-recovery autosave — write / clear / load
#include "ui/song_pointer.h"     // NAV = SONG — the pointer, the entry gate and the load-time clamp
#include "load_progress.h"       // begin_load / end_load — where the engine reports a slow load

#include <algorithm>
#include <string>
#include <vector>

namespace pt::ui {

// ─── The frame tick ──────────────────────────────────────────────────────────────────────────────

void InputDispatcher::set_now(long long now_ms) {
    now_ms_ = now_ms;
    run_due_sample_preview_restore();   // the sample editor's 100 ms audition restore (S6b)
    run_due_autosave();                 // the crash-recovery autosave's 3 s debounce  (S10)
    run_midi_hotplug();                 // a MIDI device plugged in or pulled out — before the dismiss,
                                        // so a message it posts is seen this frame
    run_due_status_dismiss();           // the status line's auto-dismiss (parity finding 5)
    run_instrument_entry_push();        // Android's on-entry instrument push (parity finding 8)
    run_selection_recency();            // which rung L+R takes first
    run_mapped_cc_dirty();              // a knob on the cable moved something in the song
    // What a live MIDI key plays: on PHRASE the instrument of the note under the cursor, anywhere
    // else the one last typed or picked; on the SONG cursor's track — the remembered one while
    // another screen is up — over KEYS' voices. Every frame, because a cursor has no change
    // notification; the host republishes only when it changed.
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
    // The cable reporting which channel its knobs are on — one copy a frame, for a screen that is
    // built in two places and can ask no host of its own.
    s_.midiInCcChannel = host_.last_cc_channel();
    const AudioEngine::BlockTiming bt = host_.block_timing();
    s_.audioLoad = AudioLoad{bt.blockFrames, bt.sampleRate, bt.meanLoad, bt.worstLoad};
}

// ─── A slow load ─────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::begin_load(long long now_ms, std::string detail) {
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

    // ⚠️ The strip is raised HERE and never at `begin_load`. Below the delay a load draws nothing at
    // all — see LOADING_DELAY_MS. Once raised it stays up: a bar that vanishes because one file
    // in a project happened to be quick is a flicker, not a report.
    if (s_.loading.elapsedMs >= LOADING_DELAY_MS) s_.loading.shown = true;

    // ⚠️⚠️ **THROTTLED, AND WITHOUT THIS THE STRIP MAKES THE LOAD IT IS REPORTING ON DRAMATICALLY
    // SLOWER.** A report arrives per SAMPLE HEADER — measured, 1628 of them for a 43 MB `.sf3`,
    // roughly one every 2.5 ms — and each repaint is a FULL canvas redraw plus a present. Painting
    // every report turned a 4 s load into one still running after 6 s, with a bar that looked frozen
    // because each frame advanced it by 0.06% (⅟₁₆₂₈ of the bar is a fifth of a pixel). The instrument
    // was changing what it measured, and only a run of the real app could show it.
    //
    // ⭐ Thirty frames a second is more than a progress bar can use, and it puts the drawing back
    // under the load instead of on top of it. The cancel rides the same cadence, so a press is seen
    // within a frame — which is the same latency every other button in the app has.
    if (now_ms - lastLoadPaintMs_ >= LOADING_REPAINT_MS) {
        lastLoadPaintMs_ = now_ms;

        // ⚠️ **THE PUMP COMES FIRST, AND IT IS WHAT MAKES THE FRAME WORTH DRAWING.** It is also the
        // only thing keeping the app's lifecycle alive while a load runs — see `RenderHooks::load_pump`.
        if (render_.load_pump && render_.load_pump()) s_.loading.cancelRequested = true;

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
        // Entering INSTRUMENT re-pushes the selected instrument's params, as Android's screen setter
        // does on every entry (TrackerController.kt:46–48 → updateInstrumentPlaybackParams). Belt and
        // braces by design: edits push for themselves (mark_modified), loads push wholesale — this
        // covers an instrument changed anywhere the cursor was not, one frame after arrival.
        if (s_.currentScreen == ScreenType::INSTRUMENT)
            host_.push_instrument(std::min(127, std::max(0, s_.currentInstrument)));
        lastScreenSeen_ = s_.currentScreen;
    }
}

// ─── The crash-recovery autosave (S10) ───────────────────────────────────────────────────────────

void InputDispatcher::run_due_autosave() {
    if (!autosavePending_ || now_ms_ < autosaveDueAtMs_) return;
    autosavePending_ = false;

    // ⚠️ **RE-CHECK `project_dirty()`, and this line is not belt-and-braces.** A SAVE inside the 3 s
    // window makes the document clean AND deletes the autosave — and nothing re-arms or cancels this
    // deadline when it does (a save is not an edit, so it does not go through mark_modified). Without
    // the re-check the deadline would then fire anyway and PUT THE FILE BACK: a crash-recovery autosave
    // for a project that is safely on disk, and a spurious RECOVER WORK? on the next launch. Kotlin
    // carries the identical second check, with the identical comment, for the identical reason.
    if (!s_.project_dirty()) return;

    autosave_write(host_, fs_);   // a failure is silent — see lifecycle.h
}

void InputDispatcher::flush_autosave() {
    autosavePending_ = false;
    if (!s_.project_dirty()) return;
    autosave_write(host_, fs_);
}

// ─── The status line's auto-dismiss (MainActivity.kt:734–747) ────────────────────────────────

namespace {

/**
 * One status field's watcher and deadline, for the window both status lines run.
 *
 * The WATCHER half: a CHANGE in the message re-arms the window; a change TO empty cancels it. The
 * field is the funnel, not its call sites — any site that assigns it, including ones not written
 * yet, gets the dismissal for free. And it matches Kotlin's key semantics exactly:
 * LaunchedEffect(statusMessage) restarts only when the VALUE changes, so an identical message re-set
 * inside the window does NOT extend it (ptdispatch §34 pins that case).
 *
 * Written once and called per field rather than copied: two clocks whose rules drift apart is
 * exactly the failure the funnel is here to prevent.
 */
void dismiss_status_field(std::string& message, bool& success, std::string& lastSeen,
                          long long& deadlineMs, long long nowMs, long long windowMs) {
    if (message != lastSeen) {
        lastSeen   = message;
        deadlineMs = message.empty() ? 0 : nowMs + windowMs;
    }

    if (deadlineMs != 0 && nowMs >= deadlineMs) {
        // TrackerController.clearStatus: the message goes, and success returns to true.
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

    // ⚠️ The BROWSER's own line, on the same clock. It is a second field because the browser is a
    // full-screen overlay that draws its own bottom bar and never sees the global one — but a user
    // does not know that, and a message that sits there until the directory changes is the only one
    // in the app that outlives the action it reported.
    dismiss_status_field(s_.fileBrowser.statusMessage, s_.fileBrowser.statusSuccess,
                         browserStatusLastSeen_, browserStatusDismissAtMs_, now_ms_,
                         STATUS_DISMISS_MS);
}

bool InputDispatcher::recover_from_autosave() {
    // ⚠️ A recovery opens every source the crashed session had open, so it costs exactly what loading
    // that project from the browser costs — and it is the load the user is LEAST expecting to wait
    // for, because they only pressed A on a question. Same strip, same delay, same B.
    //
    // ⚠️ The AUTO path reaches this too (`boot_recovery`), which is the one caller with no window
    // behind it on some platforms; the strip simply never gets a repaint hook there and nothing is drawn.
    const LoadScope recoverScope(*this, now_ms_, "RECOVERED WORK");

    if (!autosave_load(host_, fs_, mediaBaseDir_)) {
        s_.statusMessage = "RECOVER FAILED";
        s_.statusSuccess = false;
        return false;
    }

    reset_editing_context();

    // ⚠️⚠️ **A CANCELLED RECOVERY MUST RETURN TRUE, WHICH LOOKS BACKWARDS AND IS THE WHOLE POINT.**
    // `confirm_accept` reads a false as "this file is no good" and DELETES the autosave — so reporting
    // the cancel honestly here would throw the user's crashed session away because they stopped a slow
    // load. The document goes blank (half its instruments point at audio the engine does not have) and
    // the FILE STAYS, so the next launch offers it again.
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

    // ⚠️ **DIRTY, on purpose — the one load path in the app that is.** `load_project_done` aligns the
    // two versions because a loaded project IS what is on disk. Recovered work is not: it lives in one
    // file the user cannot see, has never named and did not ask for. Marking it clean would tell them
    // the song is safe at the exact moment its only copy is the crash file. So the version is bumped
    // and the baseline is left behind, the document reads as dirty, and the next NEW or EXIT asks —
    // which is the nudge to save it under a real name. (TrackerController.recoverFromAutosave.)
    //
    // It also means the debounce is NOT armed here, and does not need to be: the file it would write is
    // the file we just read. The next actual edit arms it, and rewrites it with the edit in.
    s_.projectVersion      = 1;
    s_.savedProjectVersion = 0;
    s_.projectPath.clear();   // it came from the autosave, which is not a name the user can save over

    s_.statusMessage = "RECOVERED";
    s_.statusSuccess = true;
    return true;
}

InputDispatcher::BootRecovery InputDispatcher::boot_recovery() {
    if (!autosave_exists(fs_)) return BootRecovery::NONE;   // last session ended cleanly — nothing to say

    if (!s_.settings.autosaveResumeAuto) {
        // ASK. The prompt is raised by nobody's button, which makes it the only dialog in the app the
        // user did not open — so it must be the first thing they see, before a keystroke can land on
        // the screen underneath it. (The confirm is the topmost modal and owns every button but A/B.)
        //
        // ⚠️ Note it does NOT try to parse the file first. A corrupt autosave still raises the prompt,
        // and A on it then fails and drops it (confirm_accept). That is deliberate: reading a ~440 KB
        // document to decide whether to ASK about it would put the cost of the recovery on every launch
        // that has one, and the answer would be the same anyway — the user is told either way.
        s_.confirm.open(ConfirmDialogState::Kind::RECOVER);
        return BootRecovery::ASKED;
    }

    // AUTO. Restore in silence — the right answer on a handheld whose launcher kills the port every
    // time the user opens a menu, where a prompt on every return is noise rather than a safeguard.
    //
    // ⚠️ **A corrupt autosave is DROPPED, not offered again.** Without this, AUTO would try the same
    // unreadable file on every launch forever — Kotlin guards the AUTO path for exactly this reason
    // ("so AUTO can't loop on it"). ⚠️ And S10 found that its ASK path does NOT: a `recoverFromAutosave`
    // that fails there leaves the file, so the prompt returns every single launch and can never
    // succeed. Both arms drop it here, and Android's ASK arm now does too.
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
    // SONG is 256 rows deep and shows 16. A SCREEN-scope selection there means the whole ARRANGEMENT,
    // not the visible window — which is the only reason `maxRow` is a parameter at all.
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

/**
 * The GROOVE screen's state, assembled once. The cursor context and the edit both need all five
 * fields — where the tick cursor is, where the panel cursor is, and the quantize pointer — so they
 * read it from here rather than each building their own and drifting apart.
 */
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
            // ⚠️ The CURSOR needs the project too, not only the renderer: the NOTE cell's scale comes
            // from it, and a null one silently reads as "chromatic" — a note cursor that steps by
            // semitones with no error anywhere. `layout.cpp` sets this on its own PhraseEditorState;
            // the two are separate objects and setting one is not setting the other.
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
            // The PRESET row's range is the SF2's own list length, so the context needs it.
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
            ss.theme        = s_.theme;   // VISUALIZER's value lives on the theme, not in the settings
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
    Project& p = host_.edit_project();  // the SAME Project the Sequencer is reading

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

            // The "last edited" memory + the audition. Note the two guards: the STEP must have a note
            // (editing the velocity of an empty step remembers nothing), and only an edit to the NOTE
            // column auditions — dialling a velocity should not retrigger the voice under your fingers.
            const songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];
            if ((r.hasNote || r.hasVolume || r.hasInstrument) && step.note != Note::EMPTY()) {
                s_.lastEditedNote       = step.note;
                s_.lastEditedVolume     = step.volume;
                s_.lastEditedInstrument = step.instrument;
                if (r.hasNote) preview_held_note();
            }
            // A+B under a held audition: the note it was playing is gone, so is the sound.
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
            // ⚠️ The quantize pointer is NOT part of the song, so it comes back rather than being
            // written through the Groove — and moving it must not report a modification, or setting
            // an editing aid would dirty the project and arm an autosave.
            if (r.newQuantize >= 0) s_.grooveQuantize = r.newQuantize;
            return r.modified;
        }

        case ScreenType::SCALE: {
            // ⚠️ The KEY row is the one cell on this screen that does NOT edit the object the module
            // was handed — it edits the project. The module says so by handing the new key back
            // rather than by reaching for a Project it has no business holding.
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

            // The PRESET row. The module deliberately does not resolve it — the bank+preset behind an
            // index live in the SF2's own list, which only the ENGINE has opened. Keeping that one
            // lookup here is what keeps the module a pure function of the Project, and therefore
            // measurable by tools/ptinput.
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

        // MIXER and EFFECTS take the whole PROJECT, not a cell: their fields are scattered across it
        // (a track's volume, the master strip, two send buses), and which one an action lands on is the
        // cursor's business. Kotlin's modules likewise take the Project.
        case ScreenType::MIXER:
            return mixer_.handle_input(p, s_.mixerMasterRow, s_.mixerCursorColumn, action).modified;

        case ScreenType::EFFECTS:
            return effects_.handle_input(p, s_.effectsCursorRow, action).modified;

        case ScreenType::PROJECT:
            return project_
                .handle_input(p, s_.projectCursorRow, s_.projectCursorColumn, action)
                .modified;

        // ⚠️ SETTINGS edits the SETTINGS, not the project — so it returns `false` and mark_modified()
        // never runs. That is the point, not an oversight: turning the visualizer on does not make a
        // song dirty, and it must not put a "you have unsaved work" question in front of the next NEW
        // or EXIT. (It is also why this arm is the only one that ignores `p`.) The shell persists
        // these to settings.json instead — see the SETTINGS branch of on_a and the shell's own save.
        case ScreenType::SETTINGS: {
            const bool navBefore = s_.settings.navSongRelative;
            settings_.handle_input(s_.settings, s_.theme, s_.caps, s_.settingsCursorRow,
                                   s_.settingsCursorColumn, action);
            // ⚠️ SWITCHING NAV ON MUST LAND THE POINTER SOMEWHERE REAL. The remembered song cell was
            // last meaningful under POOL, where nothing kept it on a filled cell — so without this the
            // first R+RIGHT after flipping the row is refused, on a screen with nothing to say why.
            if (!navBefore && s_.settings.navSongRelative) clamp_song_pointer(s_);
            return false;
        }

        // ⚠️ MIDI IS THE ONE SCREEN THAT EDITS BOTH SUBJECTS, so it is the one arm whose return value
        // is a QUESTION rather than a constant. PROG CHG is a `Project` field that emits into
        // the .ptp, so they dirty the song exactly as TEMPO does; OUTPUT, INPUT and OFFSET are
        // settings.json's and must not, or picking a cable would put "you have unsaved work" in front
        // of the next NEW.
        case ScreenType::MIDI: {
            const MidiInputResult r =
                midi_.handle_input(p, s_.settings, s_.midiCursorRow, s_.midiCursorColumn,
                                   s_.midiDeviceNames, s_.midiInDeviceNames, action);
            // The side effects the module cannot perform itself — it has no port and no OS.
            if (r.deviceChanged)   apply_midi_device();
            if (r.inDeviceChanged) apply_midi_in_device();
            if (r.offsetChanged)   host_.set_midi_offset_ms(
                                       midi_offset_in_force(s_.settings, s_.midiAutoOffsetMs));
            if (r.syncChanged)     host_.set_midi_sync_out(s_.settings.midiSyncOut);
            if (r.controlChannelChanged)
                host_.set_midi_control_channel(s_.settings.midiControlChannel);
            return r.projectModified;
        }

        // ⚠️ THE MAPPINGS ARE THE SONG'S, so every edit here dirties it — unlike the cable rows above,
        // and like PROG CHG. ⚠️ Nothing is pushed to the engine: a mapping says what a knob WILL do,
        // and until one turns, no parameter has moved.
        case ScreenType::MIDI_MAP: {
            const MidiMapInputResult r = midiMap_.handle_input(p, s_.midiMapCursorRow,
                                                               s_.midiMapCursorColumn, action);
            // A delete leaves the cursor one past the end of a list that just got shorter — and the
            // ADD row is where it should land, which is exactly the row that is now under it.
            if (r.rowDeleted) clamp_midi_map_cursor();
            return r.modified;
        }

        case ScreenType::SAMPLE_EDITOR: {
            const SampleEditorInputResult r = sample_.handle_input(s_.sampleEditor, action);
            if (r.rateModeChanged || r.bitDepthChanged) apply_sample_rate_and_bits();

            // ⚠️ `false`, and it is the honest answer rather than a shortcut. This function's question is
            // "did the LIVE DOCUMENT change?", and the sample editor's session state is not the document:
            // its zoom, its selection, its slice index and its pending pitch shift are invisible to the
            // sequencer and to the engine alike, so there is nothing to push and nothing to notify. Saying
            // `true` would send `mark_modified()` → `notify_data_changed()` on every step of a held A+UP
            // on the ZOOM cell, rolling the lookahead back sixty times a second under a playing song, for
            // an edit that did not change a single audible thing.
            //
            // The edits here that DO reach the engine are RATE and BIT, which rebuild the buffer — and
            // they say so themselves, in `apply_sample_rate_and_bits()` above.
            return false;
        }

        default:
            return false;
    }
}

void InputDispatcher::mark_dirty_and_arm_autosave() {
    // The document changed, so there is unsaved work. One counter, bumped in the one place every
    // edit in the app already funnels through — which is what makes "is this project dirty?" a
    // question with a single answer rather than a flag each screen must remember to set.
    // (TrackerController's projectVersion; SAVE / LOAD / NEW align savedProjectVersion to it.)
    //
    // ⚠️ ONE COUNTER, ONE JOB — and on Android it has two, which is a bug S10 found by building the
    // thing that reads it. Kotlin's `projectVersion` is ALSO the Compose recomposition trigger (every
    // write to an `observed` property calls `onStateChanged()` → `stateVersion++`), so its SETTINGS arm
    // bumps it purely to force a redraw — and inherits "the song is dirty" for free. Change the
    // visualizer on Android and three seconds later a crash-recovery autosave is written for a project
    // with no edits in it; the next launch asks RECOVER WORK? about work that does not exist. There is
    // no recomposition here, so the counter only ever had the one job, and S7's arm already refused to
    // bump it for a settings change (see generic_input's SETTINGS case). Android is fixed to match.
    s_.projectVersion++;

    // ── Arm the autosave's debounce (S10) ────────────────────────────────────────────────────────
    //
    // ⚠️ RE-ARMED, not armed-if-idle: the deadline is 3 s after the LAST edit, so a burst coalesces
    // into ONE write. Holding A+UP produces an edit every 100 ms (the key-repeat interval), and an
    // arm-once deadline would fire in the middle of it and then again, and again — ~440 KB of JSON onto
    // an SD card, ten times a second, for a value the user is still moving. Kotlin gets the same
    // behaviour from Compose rather than by saying it: a `LaunchedEffect(projectVersion)` is CANCELLED
    // and restarted every time its key changes, so the `delay(3000)` inside it never completes until
    // the edits stop.
    //
    // ⚠️ These two blocks travel TOGETHER, which is why they are one function: on Android every
    // projectVersion bump re-keys the autosave effect, so "dirty" and "armed" cannot come apart. A
    // bare `projectVersion++` here is a document that reads as dirty with no crash protection behind
    // it — exactly what the EQ path had (parity audit, finding 7; ptdispatch §33).
    autosavePending_ = true;
    autosaveDueAtMs_ = now_ms_ + AUTOSAVE_DEBOUNCE_MS;
}

void InputDispatcher::mark_modified(bool table_touched) {
    mark_dirty_and_arm_autosave();

    // ⚠️ The consumer caches which tables it has already pushed to the engine. push_project
    // invalidates that cache; an IN-PLACE edit cannot, so the table screen must say so itself.
    if (table_touched || s_.currentScreen == ScreenType::TABLE) host_.invalidate_tables();

    // ⚠️ An instrument's params are ENGINE STATE, not something a note carries: the engine holds one
    // slot per instrument for its drive, crush, downsample, filter, sample window and loop, and reads
    // them as a voice runs. Turn the filter while a pad rings and you must hear it turn. Nothing on the
    // event path pushes them (a note re-pushes the mods and sends, but not these), so the edit says so
    // here — the same call Kotlin's InstrumentController.updateDrive makes.
    if (on_instrument_screen()) host_.push_instrument(s_.currentInstrument);

    // The same argument one level up: the MIXER and EFFECTS screens edit state the engine holds ON ITS
    // OWN BEHALF — the mixer, the master bus, both send buses, the master EQ. None of it is a note, so
    // nothing on the event path pushes it, and an unpushed reverb setting is simply not heard.
    //
    // ⚠️ ONE DELIBERATE DIVERGENCE FROM ANDROID, and it is a bug fix. Kotlin pushes these per-field, and
    // three of its arms are guarded — `if (slot >= 0) setMasterEqSlot(slot)`, and the same for the two
    // input EQs. So DELETING an EQ slot (A+B) writes −1 into the project and then declines to tell the
    // engine, which goes on filtering with the slot it last had: the screen says "off", the audio says
    // otherwise, until the project is reloaded. −1 is the engine's own documented bypass value
    // (audio-engine.h) and Kotlin's *load* path pushes it happily (pushGlobalEffectsToBackend), so the
    // guard is simply wrong. Pushing the globals wholesale, as below, cannot express the bug. The Kotlin
    // side is fixed to match (AppInputDispatcher).
    if (on_globals_screen()) host_.push_globals();

    // An edit made WHILE PLAYING has to reach the lookahead already scheduled past it, or it is not
    // heard until the buffer happens to roll over it. Android does this from a
    // `LaunchedEffect(projectVersion)`; there is no Compose here, so it is said out loud.
    if (host_.is_playing()) host_.notify_data_changed();
}

void InputDispatcher::run_mapped_cc_dirty() {
    const uint64_t writes = host_.mapped_cc_writes();
    if (writes == mappedCcSeen_) return;
    mappedCcSeen_ = writes;

    // ⚠️ **ONCE A FRAME, NOT ONCE A MESSAGE, AND THAT IS THE WHOLE REASON THIS IS A POLL.** A swept
    // knob writes ~30 times a second; the document is equally dirty after the first of them and after
    // the thirtieth, and the autosave's debounce only has to be re-armed while the sweep is still
    // going. ⚠️ The dirty FLAG is still immediate in the sense that matters — within a frame of the
    // first message — because a sweep that did not mark the song modified is a sweep the "you have
    // unsaved work" question never asks about.
    mark_dirty_and_arm_autosave();
}

// ─── MIDI learn — hold R, turn a knob ────────────────────────────────────────────────────────────

void InputDispatcher::on_r_held(bool down) { host_.set_midi_learn_armed(down); }

songcore::MapTarget InputDispatcher::map_target() const {
    const Project& p = *s_.project;

    // ⚠️ A modal owns the screen while it is up, and the cursor underneath it is not what the user is
    // aiming at. The same rule every button obeys here — a knob is simply the one "press" that can
    // arrive without going through the mapper.
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
            // ⚠️ WHICH instrument is this layer's to say — the module holds one by reference and has
            // never known its number. "Instrument 3's cutoff" is finished here or nowhere.
            songcore::MapTarget t = instrument_.map_target(is);
            t.scope = static_cast<uint8_t>(s_.currentInstrument);
            return t;
        }

        // Every other screen names no parameter. A phrase step, a chain row, a file name and a
        // settings row are not values a knob sweeps, and the catalogue deliberately does not hold
        // everything the cursor can sit on.
        default:
            return {};
    }
}

void InputDispatcher::run_midi_learn() {
    const uint64_t events = host_.midi_learn_events();
    if (events == learnSeen_) return;
    learnSeen_ = events;

    // ⚠️⚠️ **THE KNOB CAME IN ON A CHANNEL NOTHING IS LISTENING TO, AND SAYING WHICH ONE IS THE
    // WHOLE POINT.** `CTL CH` is OFF by default — it has to be, or a guessed channel would steal CCs
    // the tracks are already routing — so on a fresh install this gesture CANNOT work until that row
    // is set, and the user has no way to know what number to put there. The cable knows. It says so.
    if (!songcore::ctl_ch_covers(host_.midi_control_channel(), host_.midi_learn_channel())) {
        s_.statusMessage = "KNOB ON CH " + dec2(host_.midi_learn_channel() + 1) + " - SET CTL CH";
        s_.statusSuccess = false;
        return;
    }

    const songcore::MapTarget target = map_target();
    if (!target.named()) {
        // ⚠️ SAID OUT LOUD, because the alternative is a gesture with no feedback at all: a knob that
        // learns nothing looks exactly like a knob that is not plugged in.
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

    // ⚠️ The SCOPE is said out loud, in the numbering the mapping list already uses — a track from 1,
    // an instrument in hex from 00. "MAPPED 4A > INS CUT" on a screen showing eight instruments does
    // not tell the user which one the knob now owns.
    const songcore::MapDest* d = songcore::map_dest(target.id);
    std::string where = d ? d->name : "?";
    if (d && d->scope == songcore::MapScope::TRACK)
        where += " " + std::to_string(target.scope + 1);
    else if (d && d->scope == songcore::MapScope::INSTRUMENT)
        where += " " + songcore::hex2(target.scope);
    s_.statusMessage = "MAPPED " + songcore::hex2(cc) + " > " + where;
    s_.statusSuccess = true;

    // ⚠️ Dirty, and NOTHING PUSHED — the same split the mapping screen's own edits make. A mapping
    // says what a knob WILL do; until one turns, no parameter has moved and the engine has nothing to
    // be told.
    mark_dirty_and_arm_autosave();
}

int InputDispatcher::remembered_song_track() const { return s_.songCursorColumn - 1; }

int InputDispatcher::audition_track() const {
    const Project& p = *s_.project;
    const int track = remembered_song_track();
    if (track < 0 || track >= static_cast<int>(p.tracks.size())) return -1;
    const std::vector<int>& refs = p.tracks[static_cast<size_t>(track)].chainRefs;
    // ⚠️ The size guard is load-bearing, not defensive: a track's chainRefs may be SHORTER than the
    // 256-row screen (the model's default is an empty vector).
    if (s_.songCursorRow < 0 || s_.songCursorRow >= static_cast<int>(refs.size())) return -1;
    return refs[static_cast<size_t>(s_.songCursorRow)] >= 0 ? track : -1;
}

void InputDispatcher::preview_held_note() {
    if (!s_.settings.notePreviewEnabled || s_.selection.active) return;
    if (s_.currentScreen != ScreenType::PHRASE || s_.cursorColumn != 1) return;

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
    // ⚠️ THE THEME EDITOR HAS NO CursorContext AT ALL, so it does not merely get checked first — it gets
    // checked and RETURNS. Kotlin's `handleGenericInput` opens with the identical line
    // (`if (themeEditorState.isOpen) return`), and the reason is in the module header: a channel of a
    // colour is not a cell of a document, and nothing in CursorContext's vocabulary can say it. Every
    // edit the editor makes therefore happens in `on_a_up`/`on_a_down`/`on_a_left`/`on_a_right`, not
    // here. Without this arm, A+UP inside the editor would nudge whatever cell of SETTINGS the cursor
    // was parked on when the editor was raised — which is, by construction, always the THEME row.
    if (theme_open()) return;

    // ⚠️ THE EQ EDITOR IS CHECKED FIRST, and it has to be — it is an OVERLAY, so `currentScreen` still
    // names the screen UNDERNEATH it. Ask that screen what is under its cursor and A+UP would nudge a
    // mixer fader while the user is dialling a bell curve. Kotlin opens `handleGenericInput` with the
    // same arm for the same reason.
    if (eq_open()) {
        EqState es{*s_.project};
        es.slotIndex = s_.eq.slotIndex;
        es.cursorRow = s_.eq.cursorRow;
        es.caller    = s_.eq.caller;

        const CursorContext ctx = eq_.cursor_context(es);
        const InputAction   act = fn(ctx);
        const EqInputResult r =
            eq_.handle_input(host_.edit_project(), s_.eq.slotIndex, s_.eq.cursorRow, act);

        if (r.eqBandChanged) {
            // NOT `mark_modified()`. That one re-pushes the whole GLOBALS (the mixer, both send buses,
            // all 128 EQ slots) whenever `currentScreen` is MIXER or EFFECTS — and `currentScreen` is
            // whatever is behind this overlay. Holding A+UP on a GAIN cell fires an edit every 100 ms;
            // the right-sized verb is the two calls the band actually needs. It bumps `projectVersion`
            // itself (via apply_caller_eq_slot_change), which is what marks the song dirty.
            push_eq_band_to_engine();
            if (host_.is_playing()) host_.notify_data_changed();
        }
        return;
    }

    const InputAction action = fn(cursor_context());
    if (action.type == ActionType::NONE) return;
    if (apply_edit(action)) mark_modified();
}

void InputDispatcher::selection_or_single(InputAction (*fn)(const CursorContext&)) {
    if (!s_.selection.active) {
        generic_input(fn);
        return;
    }
    // Every row of the selection, one at a time, through the SAME path — the cursor is walked down
    // the range and put back. Not a special-cased bulk edit: whatever a single cell does, N cells do
    // N times, so a new column can never behave differently under a selection than outside one.
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

        // Drag the CURSOR along with the selection's active edge, so it stays on screen — without
        // this, a SONG selection whose edge runs past row 16 scrolls out from under the anchored
        // cursor and you are editing blind. Only when the edge actually MOVED, so hitting a clamp (or
        // a D-pad in SCREEN scope, where the bounds are fixed) cannot teleport the cursor.
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
// ⚠️ THE MODAL RULE (input_dispatcher.h): keyboard first, then browser, then the screen. The order is
// the specification — the keyboard opens ON TOP of the browser (SELECT+A renames a file), and a D-pad
// press there must move the KEY cursor, not the file cursor.

void InputDispatcher::on_dpad_up() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::THEME | Overlay::EQ | Overlay::BROWSER |
                         Overlay::RENDER)) return;
    if (render_dialog_open()) { render_dialog_move_cursor(-1); return; }
    if (qwerty_open()) { move_key_cursor_up(s_.qwerty); return; }
    if (theme_open())  { theme_move_cursor(-1, 0); return; }
    if (eq_open())     { eq_move_cursor(0, -1); return; }
    if (on_browser())  { browser_move_cursor(-1, /*page=*/false); return; }
    dpad_nav(NavDir::UP);
}

void InputDispatcher::on_dpad_down() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::THEME | Overlay::EQ | Overlay::BROWSER |
                         Overlay::RENDER)) return;
    if (render_dialog_open()) { render_dialog_move_cursor(+1); return; }
    if (qwerty_open()) { move_key_cursor_down(s_.qwerty); return; }
    if (theme_open())  { theme_move_cursor(+1, 0); return; }
    if (eq_open())     { eq_move_cursor(0, +1); return; }
    if (on_browser())  { browser_move_cursor(+1, /*page=*/false); return; }
    dpad_nav(NavDir::DOWN);
}

void InputDispatcher::on_dpad_left() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::THEME | Overlay::EQ | Overlay::BROWSER)) return;
    if (qwerty_open()) { move_key_cursor_left(s_.qwerty); return; }
    // ⚠️ In the THEME editor LEFT/RIGHT change CHANNEL (R→G→B), and they WRAP, where the EQ's clamp.
    // Three channels are a ring you walk, not a range you scan to the end of.
    if (theme_open())  { theme_move_cursor(0, -1); return; }
    // ⚠️ In the EQ editor LEFT/RIGHT change BAND while keeping the PARAM — they do not walk a flat list
    // of twelve. That is what lets you sweep the same parameter across all three bands without moving
    // your thumb off the row, which is how an EQ is actually dialled.
    if (eq_open())     { eq_move_cursor(-1, 0); return; }
    // LEFT/RIGHT PAGE the browser by a screenful — the one list in the app long enough to need it.
    if (on_browser())  { browser_move_cursor(-BROWSER_VISIBLE_ROWS, /*page=*/true); return; }
    dpad_nav(NavDir::LEFT);
}

void InputDispatcher::on_dpad_right() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::THEME | Overlay::EQ | Overlay::BROWSER)) return;
    if (qwerty_open()) { move_key_cursor_right(s_.qwerty); return; }
    if (theme_open())  { theme_move_cursor(0, +1); return; }
    if (eq_open())     { eq_move_cursor(+1, 0); return; }
    if (on_browser())  { browser_move_cursor(+BROWSER_VISIBLE_ROWS, /*page=*/true); return; }
    dpad_nav(NavDir::RIGHT);
}

}  // namespace pt::ui
