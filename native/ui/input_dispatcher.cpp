#include "ui/input_dispatcher.h"

#include "songcore/timing.h"
#include "songcore/traversal.h"
#include "ui/cursor_move.h"
#include "ui/lifecycle.h"        // the crash-recovery autosave — write / clear / load (S10)
#include "ui/navigation.h"
#include "ui/song_pointer.h"     // NAV = SONG — the pointer, the entry gate and the load-time clamp
#include "ui/std_filesystem.h"   // path_name / path_stem / path_extension / to_lower
#include "ui/theme_io.h"         // .ptt — save_theme_file / load_theme_file

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pt::ui {

using songcore::Chain;
using songcore::Instrument;
using songcore::Note;
using songcore::Phrase;
using songcore::Project;

namespace {

/** Chain.isEmpty(row) — the row holds no phrase. */
bool chain_row_empty(const Chain& c, int row) { return c.phraseRefs[static_cast<size_t>(row)] == -1; }

/** A phrase nobody has written a note into. */
bool phrase_is_blank(const Phrase& p) {
    for (const songcore::PhraseStep& s : p.steps)
        if (!songcore::step_is_empty(s)) return false;
    return true;
}

/** A chain that references no phrase. */
bool chain_is_blank(const Chain& c) {
    for (const int ref : c.phraseRefs)
        if (ref != -1) return false;
    return true;
}

/**
 * Kotlin's `((start..255) + (0 until start)).firstOrNull { pred }` — search forward from `start`,
 * wrapping once. Returns −1 when the pool is full.
 *
 * The wrap is the point: inserting the next unused phrase should hand you one NEAR the one you were
 * just editing, not slot 0 every time. Starting at `lastEdited + 1` and wrapping is what does that.
 */
template <typename Pred>
int first_from_wrapping(int start, int count, Pred pred) {
    for (int i = start; i < count; ++i)
        if (pred(i)) return i;
    for (int i = 0; i < start && i < count; ++i)
        if (pred(i)) return i;
    return -1;
}

/** Phrase IDs any chain references — "used" even when blank (a silent spacer inside a pad chain). */
std::set<int> used_phrase_ids(const Project& p) {
    std::set<int> used;
    for (const Chain& c : p.chains)
        for (const int ref : c.phraseRefs)
            if (ref != -1) used.insert(ref);
    return used;
}

/** The 20 characters a slot's name is cut to when it is taken from a file. One writer, two readers. */
std::string adopted_name(const std::string& stem) { return stem.substr(0, 20); }

/**
 * The name slot `id` would carry if it had adopted its CURRENT source file's — "" when it has no
 * source. Compare a slot's actual name against this to tell an auto-adopted name from a typed one.
 *
 * The source is the SoundFont path for a SoundFont slot and the sample path for everything else; an
 * EXTERNAL slot has neither, and answers "".
 */
std::string instrument_auto_name(const Project& p, int id) {
    if (id < 0 || static_cast<size_t>(id) >= p.instruments.size()) return {};
    const Instrument& ins = p.instruments[static_cast<size_t>(id)];
    const std::string source = (ins.instrumentType == songcore::InstrumentType::SOUNDFONT)
                                   ? ins.soundfontPath.value_or("")
                                   : ins.sampleFilePath.value_or("");
    return source.empty() ? std::string{} : adopted_name(path_stem(source));
}

/** Chain IDs any song track references — same "used even if blank" reasoning. */
std::set<int> used_chain_ids(const Project& p) {
    std::set<int> used;
    for (const songcore::Track& t : p.tracks)
        for (const int ref : t.chainRefs)
            if (ref != -1) used.insert(ref);
    return used;
}

}  // namespace

// ─── The frame tick ──────────────────────────────────────────────────────────────────────────────

void InputDispatcher::set_now(long long now_ms) {
    now_ms_ = now_ms;
    run_due_sample_preview_restore();   // the sample editor's 100 ms audition restore (S6b)
    run_due_autosave();                 // the crash-recovery autosave's 3 s debounce  (S10)
    run_due_status_dismiss();           // the status line's 5 s auto-dismiss (parity finding 5)
    run_instrument_entry_push();        // Android's on-entry instrument push (parity finding 8)
    run_selection_recency();            // which rung L+R takes first
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

// ─── The status line's 5 s auto-dismiss (MainActivity.kt:734–747) ────────────────────────────────

namespace {

/**
 * One status field's watcher and deadline, for the 5 s window both status lines run.
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
    if (!autosave_load(host_, fs_, mediaBaseDir_)) {
        s_.statusMessage = "RECOVER FAILED";
        s_.statusSuccess = false;
        return false;
    }

    reset_editing_context();

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
            return phrase_.cursor_context(ps);
        }
        case ScreenType::TABLE: {
            TableState ts{p.tables[static_cast<size_t>(s_.currentTable)]};
            ts.cursorRow       = s_.tableCursorRow;
            ts.cursorColumn    = s_.tableCursorColumn;
            ts.effectTypeCount = visible_effect_type_count();
            return table_.cursor_context(ts);
        }
        case ScreenType::GROOVE: {
            GrooveState gs{p.grooves[static_cast<size_t>(s_.currentGroove)]};
            gs.cursorRow    = s_.grooveCursorRow;
            gs.cursorColumn = 1;
            return groove_.cursor_context(gs);
        }
        case ScreenType::SCALE: {
            ScaleState cs{p.scales[static_cast<size_t>(s_.currentScale)]};
            cs.key       = p.scaleKey;
            cs.cursorRow = s_.scaleCursorRow;
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
            ms.cursorRow      = s_.midiCursorRow;
            ms.cursorColumn   = s_.midiCursorColumn;
            ms.deviceIndex    = s_.midiDeviceIndex;
            ms.inDeviceIndex  = s_.midiInDeviceIndex;
            ms.caps           = s_.caps;
            return midi_.cursor_context(ms);
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

            // The "last edited" memory + the audition, exactly where Kotlin does them. Note the two
            // guards: the STEP must have a note (editing the velocity of an empty step remembers
            // nothing), and only an edit to the NOTE column auditions — dialling a velocity should
            // not retrigger the voice under your fingers.
            if (r.hasNote || r.hasVolume || r.hasInstrument) {
                const songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];
                if (step.note != Note::EMPTY()) {
                    s_.lastEditedNote       = step.note;
                    s_.lastEditedVolume     = step.volume;
                    s_.lastEditedInstrument = step.instrument;
                    if (s_.settings.notePreviewEnabled && r.hasNote) preview_edited_note();
                }
            }
            return true;
        }

        case ScreenType::TABLE:
            return table_
                .handle_input(p.tables[static_cast<size_t>(s_.currentTable)], s_.tableCursorRow,
                              s_.tableCursorColumn, action)
                .modified;

        case ScreenType::GROOVE:
            return groove_
                .handle_input(p.grooves[static_cast<size_t>(s_.currentGroove)], s_.grooveCursorRow,
                              /*cursor_column=*/1, action)
                .modified;

        case ScreenType::SCALE: {
            // ⚠️ The KEY row is the one cell on this screen that does NOT edit the object the module
            // was handed — it edits the project. The module says so by handing the new key back
            // rather than by reaching for a Project it has no business holding.
            const ScaleInputResult r = scale_.handle_input(
                p.scales[static_cast<size_t>(s_.currentScale)], p.scaleKey, s_.scaleCursorRow, action);
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
        // is a QUESTION rather than a constant. PROG CHG and IN CH are `Project` fields that emit into
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
            if (r.offsetChanged)   host_.set_midi_offset_ms(s_.settings.midiOffsetMs);
            if (r.syncChanged)     host_.set_midi_sync_out(s_.settings.midiSyncOut);
            return r.projectModified;
        }

        case ScreenType::SAMPLE_EDITOR: {
            const SampleEditorInputResult r = sample_.handle_input(s_.sampleEditor, action);
            if (r.rateModeChanged) apply_sample_rate_mode();

            // ⚠️ `false`, and it is the honest answer rather than a shortcut. This function's question is
            // "did the LIVE DOCUMENT change?", and the sample editor's session state is not the document:
            // its zoom, its selection, its slice index and its pending pitch shift are invisible to the
            // sequencer and to the engine alike, so there is nothing to push and nothing to notify. Saying
            // `true` would send `mark_modified()` → `notify_data_changed()` on every step of a held A+UP
            // on the ZOOM cell, rolling the lookahead back sixty times a second under a playing song, for
            // an edit that did not change a single audible thing.
            //
            // The one edit here that DOES reach the engine is RATE, which re-decimates the buffer — and it
            // says so itself, in `apply_sample_rate_mode()` above, exactly where Kotlin says it.
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

void InputDispatcher::preview_edited_note() {
    const Project& p = *s_.project;
    const songcore::PhraseStep& step =
        p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];

    const int sr = std::max(44100, host_.sample_rate());
    host_.set_preview_track(audition_track());
    host_.preview_note(std::min(std::max(step.instrument, 0), 127), step.note,
                       songcore::frames_per_step(p.tempo, sr));
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
    if (overlay_swallows(Overlay::QWERTY | Overlay::THEME | Overlay::EQ | Overlay::BROWSER)) return;
    if (qwerty_open()) { move_key_cursor_up(s_.qwerty); return; }
    if (theme_open())  { theme_move_cursor(-1, 0); return; }
    if (eq_open())     { eq_move_cursor(0, -1); return; }
    if (on_browser())  { browser_move_cursor(-1, /*page=*/false); return; }
    dpad_nav(NavDir::UP);
}

void InputDispatcher::on_dpad_down() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::THEME | Overlay::EQ | Overlay::BROWSER)) return;
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

// ─── The THEME EDITOR ─────────────────────────────────────────────────────────────────────────────

void InputDispatcher::theme_move_cursor(int d_row, int d_channel) {
    // ⚠️ BOTH AXES WRAP, and neither clamps — which makes this the only cursor in the app that wraps in
    // BOTH directions. The rows are the THEME row plus one per colour, the channels a ring of three,
    // and the argument is the mixer's row 0 again: a list of colours is a ring you scroll, not a
    // document you reach the end of. The panel SCROLLS to follow the row (only 16 rows fit), so
    // wrapping from the last row to row 0 also scrolls the list back to the top.
    if (d_row != 0) {
        const int row = s_.themeEditor.cursorRow;
        const int max = ThemeEditorModule::max_row();
        s_.themeEditor.cursorRow = (d_row < 0) ? (row > 0 ? row - 1 : max)
                                               : (row < max ? row + 1 : 0);
    }
    if (d_channel != 0) {
        const int ch = s_.themeEditor.cursorChannel;
        s_.themeEditor.cursorChannel = (d_channel < 0) ? (ch > 0 ? ch - 1 : 2)
                                                       : (ch < 2 ? ch + 1 : 0);
    }
}

/**
 * Kotlin's `name.replace(Regex("[^a-zA-Z0-9_]"), "_")` — a theme name, as a FILENAME.
 *
 * ⚠️ It is NOT `.ifEmpty` on its own: the fallback is applied by the callers, and both of them apply it
 * (`"THEME"`). That is S7's dotfile bug in waiting — an all-punctuation name sanitizes to underscores,
 * but an EMPTY one sanitizes to an empty string, and `<Themes>/.ptt` is a dotfile the browser SKIPS.
 * Kotlin already guards it here (unlike `saveProject`, which did not until S7 fixed it), so this is the
 * one save path in the app that was never broken. Kept explicit so it stays that way.
 */
static std::string sanitize_theme_filename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        out += ok ? c : '_';
    }
    return out;
}

void InputDispatcher::theme_row_action() {
    switch (s_.themeEditor.cursorChannel) {
        case 1: {   // SAVE — name it, then write it
            // ⚠️ The keyboard opens WITHOUT closing the editor, which is why every handler tests
            // `qwerty_open()` before `theme_open()`. It seeds with the SANITIZED current name, so what
            // you are shown is what the file will be called.
            const std::string seed = sanitize_theme_filename(s_.theme.name);
            open_qwerty(QwertyContext::THEME_SAVE, seed.empty() ? "THEME" : seed, "SAVE THEME:",
                        fs_.themes_directory(), /*max_length=*/20, /*clear_on_first_b=*/true);
            break;
        }
        case 2: {   // LOAD — browse the Themes folder for a .ptt
            // ⚠️ This one CLOSES the editor first, where SAVE does not. The browser is a SCREEN, not an
            // overlay — it takes over `currentScreen` — so leaving the editor open would leave a modal
            // standing on top of a screen it was never raised from, swallowing the browser's own D-pad.
            // `browser_confirm` re-opens the editor when a theme lands.
            close_theme_editor();
            open_file_browser(AppState::BrowserPurpose::LOAD_THEME, browser_dir(BrowserDir::THEMES),
                              {"ptt"});
            break;
        }
        default:    // column 0 is the NAME, and a bare A on it does nothing. Kotlin's `when` has no arm.
            break;
    }
}

void InputDispatcher::save_theme_as(const std::string& dir, const std::string& typed_text) {
    // ⚠️ TWO DIFFERENT NAMES COME OUT OF ONE TYPED STRING, and mixing them up is the whole trap here:
    //
    //   the FILENAME is SANITIZED  → "My Theme!"  becomes  My_Theme_.ptt
    //   the NAME IN THE FILE is RAW → "My Theme!"  stays    "name": "My Theme!"
    //
    // That is deliberate on Android and it is right: the filename must survive a FAT32 SD card, and the
    // display name must survive the user's taste. An empty field keeps the theme's CURRENT name rather
    // than blanking it, and falls back to "THEME" for the file (never `.ptt`, which the browser hides —
    // S7's dotfile bug, which this path has always been guarded against).
    const std::string safe = sanitize_theme_filename(typed_text);
    const std::string file = (safe.empty() ? std::string("THEME") : safe) + ".ptt";
    const std::string path = dir + "/" + file;

    Theme to_save = s_.theme;
    if (!typed_text.empty()) to_save.name = typed_text;

    // ⚠️ THE LIVE THEME DOES NOT ADOPT THE NAME IT WAS JUST SAVED UNDER, and that is Kotlin's, kept.
    // `themeToSave` is a COPY (`appTheme.copy(name = …)`); `appTheme` itself is never reassigned. So you
    // save your palette as SUNSET and the THEME row still reads CLASSIC. It is cosmetic — the FILE is
    // correct, and loading it back does set the name — and it is left alone rather than "fixed", because
    // the name is what the built-in cycle keys off (`theme_cycle_builtin`) and adopting a custom name
    // would silently change which palette A+RIGHT lands on. A divergence with a behavioural tail is not a
    // tidy-up. Stated here rather than smuggled in; a parity-ledger entry, not a port decision.
    const bool ok = save_theme_file(fs_, path, to_save);

    // ⚠️ …but a save that FAILS must say so, and Kotlin's does not: it discards `writeFile`'s Boolean
    // outright, so a full SD card or a read-only mount closes the keyboard and reports success by
    // silence. That is S7's own headline, one screen later — "a SAVE that reports nothing is
    // indistinguishable from a SAVE that failed" — and it is a dropped error return, not a missing
    // nicety. Zone B, so it is fixed on Android too (AppInputDispatcher, QwertyContext.THEME_SAVE).
    s_.statusMessage = ok ? "THEME SAVED" : "SAVE FAILED";
    s_.statusSuccess = ok;

    open_theme_editor();
}

// ─── The FX-type column, and the helper it opens ──────────────────────────────────────────────────

bool InputDispatcher::on_fx_type_column() const {
    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
            return s_.cursorColumn == 4 || s_.cursorColumn == 6 || s_.cursorColumn == 8;
        case ScreenType::TABLE:
            return s_.tableCursorColumn == 3 || s_.tableCursorColumn == 5 ||
                   s_.tableCursorColumn == 7;
        default:
            return false;
    }
}

int InputDispatcher::current_fx_type_index() const {
    const Project& p = *s_.project;
    int            code = 0;

    if (s_.currentScreen == ScreenType::PHRASE) {
        const songcore::PhraseStep& step =
            p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        switch (s_.cursorColumn) {
            case 4: code = step.fx1Type; break;
            case 6: code = step.fx2Type; break;
            case 8: code = step.fx3Type; break;
            default: break;
        }
    } else if (s_.currentScreen == ScreenType::TABLE) {
        const songcore::TableRow& row =
            p.tables[static_cast<size_t>(s_.currentTable)].rows[static_cast<size_t>(s_.tableCursorRow)];
        switch (s_.tableCursorColumn) {
            case 3: code = row.fx1Type; break;
            case 5: code = row.fx2Type; break;
            case 7: code = row.fx3Type; break;
            default: break;
        }
    }
    return songcore::effect_type_index(code);
}

int InputDispatcher::visible_effect_type_count() const {
    return s_.caps.midi ? songcore::EFFECT_TYPE_COUNT : songcore::EFFECT_TYPE_COUNT_NO_MIDI;
}

void InputDispatcher::apply_fx_type_change(int effect_code) {
    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::PHRASE) {
        songcore::PhraseStep& step =
            p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        switch (s_.cursorColumn) {
            case 4: step.fx1Type = effect_code; break;
            case 6: step.fx2Type = effect_code; break;
            case 8: step.fx3Type = effect_code; break;
            default: return;
        }
        mark_modified();
    } else if (s_.currentScreen == ScreenType::TABLE) {
        songcore::TableRow& row =
            p.tables[static_cast<size_t>(s_.currentTable)].rows[static_cast<size_t>(s_.tableCursorRow)];
        switch (s_.tableCursorColumn) {
            case 3: row.fx1Type = effect_code; break;
            case 5: row.fx2Type = effect_code; break;
            case 7: row.fx3Type = effect_code; break;
            default: return;
        }
        mark_modified(/*table_touched=*/true);
    }
}

// ─── A + D-pad ───────────────────────────────────────────────────────────────────────────────────

// ⚠️ `on_a_b` below shares a NAME with the free cursor.h handler it calls, and inside a member
// function unqualified lookup finds the MEMBER first — `generic_input(on_a_b)` would try to pass the
// method to itself. The `pt::ui::` qualification forces namespace-scope lookup and is load-bearing,
// not decoration. The other four free handlers are named for the STEP (`increment` / `decrement` /
// `increment_fast` / `decrement_fast`), so nothing shadows them — but they are qualified alongside
// `on_a_b` so the four arms of one gesture read the same way.
//
// ⚠️ WHICH CHORD FIRES WHICH STEP IS THE SPECIFICATION, and it follows LGPT: the D-pad's HORIZONTAL
// axis is the small step and the VERTICAL axis the large one. A+RIGHT/A+LEFT is ±1, A+UP/A+DOWN is
// ±`largeStep`. Everything a screen overrides below keeps that split — the sample editor's fine and
// coarse nudges, the theme editor's ±0x01 and ±0x10, the INSTRUMENT TYPE cycle (a ±1, so horizontal).
// The one gesture that is not a step at all, the FX picker, stays on the vertical axis.

/**
 * A+LEFT/RIGHT on the INSTRUMENT screen's TYPE cell. Switching a slot's type FREES whatever source it
 * holds — a sampler has no use for an .sf2 and vice versa — so a loaded slot is asked about through
 * the confirm dialog first, and only an EMPTY slot switches outright.
 *
 * ⚠️ Silently dropping a loaded sample because the user nudged A+RIGHT one cell too far is the failure
 * this shape exists to prevent. The dialog is the guard; do not add a path around it.
 */
void InputDispatcher::request_instrument_type_toggle(int delta) {
    const Instrument& ins =
        host_.project().instruments[static_cast<size_t>(s_.currentInstrument)];

    if (ins.sampleFilePath.has_value() || ins.soundfontPath.has_value()) {
        s_.confirm.open(ConfirmDialogState::Kind::CHANGE_TYPE, delta);
        return;
    }
    toggle_instrument_type(delta);   // an empty slot has nothing to lose — switch it outright
}

/**
 * Step the TYPE cell by `delta`, wrapping through the types this build offers.
 *
 * A+RIGHT and A+LEFT have to disagree about direction or a cell with three stops can only ever be walked
 * forwards. The cycle runs on a COUNT rather than a chain of ternaries, so a fourth type joins it by
 * existing.
 *
 * ⚠️ EXTERNAL is the LAST type, and a build with the MIDI surfaces hidden simply stops one short.
 * That works for the same reason the FX list can be shortened (songcore/effects.h): the hidden entry
 * is a tail, so every reachable value keeps its meaning. The cycle is the only way to REACH the type
 * — an instrument already set to it, from a .ptp or a .pti, keeps it and keeps drawing as EXTERNAL.
 */
void InputDispatcher::toggle_instrument_type(int delta) {
    Project&    p   = host_.edit_project();
    Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];

    const int count = s_.caps.midi ? songcore::INSTRUMENT_TYPE_COUNT
                                   : songcore::INSTRUMENT_TYPE_COUNT - 1;
    const int cur   = static_cast<int>(ins.instrumentType);
    const int step  = delta < 0 ? -1 : +1;
    // An instrument that is ALREADY external in a build that hides the type is outside the cycle:
    // `cur` is `count`, and the modulo would land it back on itself. Step from the last reachable
    // type instead, so the gesture still has somewhere to go.
    const int from  = (cur >= count) ? count - 1 : cur;
    const auto next = static_cast<songcore::InstrumentType>(((from + step) % count + count) % count);
    host_.set_instrument_type(s_.currentInstrument, next);

    // The row map just changed under the cursor — the three layouts have 16, 15 and 11 rows — and the
    // cursor is sitting on row 0, which exists in all three. Its COLUMN may not: row 0 caps at 3 on a
    // sampler, 2 on a SoundFont and 1 on EXTERNAL, and the cursor is on column 1 (the TYPE cell) to
    // have reached this code at all. Nothing to clamp, then; but the SF preset readback must be
    // re-taken, and the feed does that from the path+type on the next frame.
    s_.statusMessage = std::string("TYPE: ") + songcore::instrument_type_name(next);
    s_.statusSuccess = true;
}

/** True when the cursor is on INSTRUMENT's TYPE cell, the one A+LEFT/RIGHT does not merely increment. */
bool InputDispatcher::on_instrument_type_cell() const {
    return s_.currentScreen == ScreenType::INSTRUMENT && s_.instrumentCursorRow == 0 &&
           s_.instrumentCursorColumn == 1;
}

// A+DPAD has no meaning in either modal — the keyboard's D-pad moves its key cursor (a bare press,
// handled above) and the browser's moves its file cursor. Swallowed, not passed through: an A held
// down over a browser must not reach the editor screen underneath it.

// ⚠️ On the SAMPLE EDITOR, A+DPAD on rows 3..8 does not step a cell — it DRAGS the selection's active
// edge (START on col 0, END on col 1). LEFT/RIGHT are the fine step and UP/DOWN the coarse one, both
// scaled by the ZOOM, so a nudge is always about a pixel's worth of the waveform you can actually see:
// zoomed out, UP moves a sixteenth of the sample; zoomed to 16×, it moves a sixteenth of the WINDOW.
//
// This is checked ahead of the FX helper's column test and the instrument TYPE cell, exactly where
// Kotlin checks it, because neither of those exists on this screen.
static int64_t sample_fine_step(const SampleEditorState& se) {
    return std::max<int64_t>(1, static_cast<int64_t>(se.totalFrames) / (256LL << se.zoomLevel));
}
static int64_t sample_coarse_step(const SampleEditorState& se) {
    return std::max<int64_t>(1, static_cast<int64_t>(se.totalFrames) / (16LL << se.zoomLevel));
}

// ⚠️ THE EQ ARM COMES FIRST in all five A-combo handlers, ahead of the FX helper, the sample editor's
// selection rows, the FX-type column and INSTRUMENT's type cell — which is Kotlin's order, and it is not
// merely defensive. Every one of those four questions is asked of `currentScreen`, and `currentScreen`
// is the screen UNDERNEATH the overlay. They all happen to answer "no" today (you cannot open the EQ
// from an FX column, and the editor's own cell is row 16, not the sample editor's 3..8) — which is
// exactly the kind of accident that stops being true the day someone adds an EQ cell somewhere new.
// `generic_input()` carries the real arm; these five make sure nothing gets in front of it.

// ⚠️ THE THEME ARM COMES FIRST IN ALL FOUR, and it is where the editor's whole edit lives — the module
// has no `handle_input` and no CursorContext (see generic_input). The four gestures are not symmetric:
//
//   A+LEFT / A+RIGHT → on the THEME row, step the BUILT-IN palette (prev / next).
//                      on a colour row, nudge the cursor's channel by ∓0x01.
//   A+UP   / A+DOWN  → on the THEME row, NOTHING (`if (cursorRow >= 1)` — there is no coarse step for a
//                      palette, and no fifth thing for a name to do).
//                      on a colour row, nudge the cursor's channel by ±0x10.

void InputDispatcher::on_a_up() {
    if (overlay_swallows(Overlay::THEME | Overlay::EQ | Overlay::FX_HELPER)) return;
    if (theme_open()) {
        // Row 0 falls through to nothing: `theme_adjust_color` rejects it, as the coarse arm must.
        theme_adjust_color(s_.theme, s_.themeEditor.cursorRow, s_.themeEditor.cursorChannel, +0x10);
        return;
    }
    if (eq_open()) { generic_input(pt::ui::increment_fast); return; }
    if (s_.fxHelper.isOpen) { fx_move_up(s_.fxHelper); return; }
    if (on_sample_selection_row()) { nudge_selection_edge(+sample_coarse_step(s_.sampleEditor)); return; }
    if (on_sample_slice_marker_row()) { nudge_slice_marker(+sample_coarse_step(s_.sampleEditor)); return; }
    if (on_fx_type_column()) {
        s_.fxHelper = fx_helper_opened_at(current_fx_type_index(),
                                          FxGrid::of(visible_effect_type_count()));
        return;
    }
    selection_or_single(pt::ui::increment_fast);
}

void InputDispatcher::on_a_down() {
    if (overlay_swallows(Overlay::THEME | Overlay::EQ | Overlay::FX_HELPER)) return;
    if (theme_open()) {
        theme_adjust_color(s_.theme, s_.themeEditor.cursorRow, s_.themeEditor.cursorChannel, -0x10);
        return;
    }
    if (eq_open()) { generic_input(pt::ui::decrement_fast); return; }
    if (s_.fxHelper.isOpen) { fx_move_down(s_.fxHelper); return; }
    if (on_sample_selection_row()) { nudge_selection_edge(-sample_coarse_step(s_.sampleEditor)); return; }
    if (on_sample_slice_marker_row()) { nudge_slice_marker(-sample_coarse_step(s_.sampleEditor)); return; }
    if (on_fx_type_column()) {
        s_.fxHelper = fx_helper_opened_at(current_fx_type_index(),
                                          FxGrid::of(visible_effect_type_count()));
        return;
    }
    selection_or_single(pt::ui::decrement_fast);
}

void InputDispatcher::on_a_left() {
    if (overlay_swallows(Overlay::THEME | Overlay::EQ | Overlay::FX_HELPER)) return;
    if (theme_open()) {
        if (s_.themeEditor.cursorRow == 0) theme_cycle_builtin(s_.theme, -1);
        else theme_adjust_color(s_.theme, s_.themeEditor.cursorRow,
                                s_.themeEditor.cursorChannel, -0x01);
        return;
    }
    if (eq_open()) { generic_input(pt::ui::decrement); return; }
    if (s_.fxHelper.isOpen) { fx_move_left(s_.fxHelper); return; }
    if (on_sample_selection_row()) { nudge_selection_edge(-sample_fine_step(s_.sampleEditor)); return; }
    if (on_sample_slice_marker_row()) { nudge_slice_marker(-sample_fine_step(s_.sampleEditor)); return; }
    if (on_instrument_type_cell()) { request_instrument_type_toggle(-1); return; }
    selection_or_single(pt::ui::decrement);
}

void InputDispatcher::on_a_right() {
    if (overlay_swallows(Overlay::THEME | Overlay::EQ | Overlay::FX_HELPER)) return;
    if (theme_open()) {
        if (s_.themeEditor.cursorRow == 0) theme_cycle_builtin(s_.theme, +1);
        else theme_adjust_color(s_.theme, s_.themeEditor.cursorRow,
                                s_.themeEditor.cursorChannel, +0x01);
        return;
    }
    if (eq_open()) { generic_input(pt::ui::increment); return; }
    if (s_.fxHelper.isOpen) { fx_move_right(s_.fxHelper); return; }
    if (on_sample_selection_row()) { nudge_selection_edge(+sample_fine_step(s_.sampleEditor)); return; }
    if (on_sample_slice_marker_row()) { nudge_slice_marker(+sample_fine_step(s_.sampleEditor)); return; }
    if (on_instrument_type_cell()) { request_instrument_type_toggle(+1); return; }
    selection_or_single(pt::ui::increment);
}

void InputDispatcher::on_a_released() {
    // The FX helper commits on RELEASE, not on a press — which is what lets you hold A, read the
    // description of half a dozen effects, and let go on the one you want.
    if (top_overlay() != Overlay::FX_HELPER) return;
    apply_fx_type_change(s_.fxHelper.selected_effect_code());
    s_.fxHelper = FxHelperState{};
}

void InputDispatcher::on_dpad_released() {
    // Both accelerations are keyed off "the last call looked like part of the same gesture", so
    // ending the gesture is all this has to do — the next press then reads as fresh on its own terms.
    // `lastBrowserMoveDelta_ = 0` is that statement for the browser: a delta is never 0, so the next
    // call cannot match it.
    //
    // The gap tests in both functions STAY. They are no longer the discriminator, but they still
    // cover the case a release cannot reach: a repeat train that outlives whatever produced it.
    lastBrowserMoveDelta_    = 0;
    textCursorRepeatStreak_  = 0;
}

void InputDispatcher::on_a_deferred() {
    // The mapper is holding this press. Nothing acts here — the one thing recorded is the number that
    // will have MOVED by the time A comes back up. Every other deferred cell opens something that reads
    // no clock, so they write the "nothing was sounding" value and never look at it again.
    sliceTapPlayhead_ = on_slice_tap_cell() ? s_.sampleEditor.playbackPosition : -1.0f;
}

// ─── A+B: delete / reset ─────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_a_b() {
    if (overlay_swallows(Overlay::EQ)) return;

    // A+B in the EQ editor RESETS the band param under the cursor to its default — FREQ to 0x80
    // (≈450 Hz, the middle of the log sweep), GAIN to 120 (0 dB) and Q to 0x80. TYPE has no default and
    // no delete, so A+B there is inert, exactly as Kotlin's inline context leaves it.
    if (eq_open()) { generic_input(pt::ui::on_a_b); return; }

    if (s_.selection.active) {
        const SelectionBounds b = s_.selection.bounds();
        Project&              p = host_.edit_project();
        switch (s_.currentScreen) {
            case ScreenType::PHRASE:
                clip_.delete_phrase_steps(p, s_.currentPhrase, b.topLeftRow, b.topLeftColumn,
                                          b.bottomRightRow, b.bottomRightColumn);
                break;
            case ScreenType::CHAIN:
                clip_.delete_chain_rows(p, s_.currentChain, b.topLeftRow, b.topLeftColumn,
                                        b.bottomRightRow, b.bottomRightColumn);
                break;
            case ScreenType::SONG:
                clip_.delete_song_cells(p, b.topLeftRow, b.topLeftColumn, b.bottomRightRow,
                                        b.bottomRightColumn);
                break;
            case ScreenType::TABLE:
                clip_.delete_table_rows(p, s_.currentTable, b.topLeftRow, b.topLeftColumn,
                                        b.bottomRightRow, b.bottomRightColumn);
                break;
            default:
                s_.selection.exit();
                return;
        }
        mark_modified();
        s_.selection.exit();
        return;
    }

    // ⚠️ The SAMPLE EDITOR's SELECTION row (8): A+B RESETS the edge under the cursor to the sample's own
    // bound — START to 0, END to the last frame. It is the fast way back out of a selection you have
    // nudged into a corner, and the only meaning "delete" can have on a cell that cannot be empty.
    // (Rows 3..7 are the waveform, and Kotlin's arm is row 8 alone.)
    if (on_sample_editor() && s_.sampleEditor.cursorRow == 8) {
        SampleEditorState& se = s_.sampleEditor;
        if (se.cursorCol == 0)      se.selectionStart = 0;
        else if (se.cursorCol == 1) se.selectionEnd   = se.totalFrames;
        return;
    }

    // ⚠️ The SLICE DETAIL row (11): A+B puts the boundary under the cursor back where its own method
    // would have put it — the detected position under TRANSIENT and the arithmetic cut under DIVIDE.
    // Under MANUAL there is no such position and it DELETES instead, the slices after it renumbering.
    // ONE boundary, matching row 8 above; changing the method or its parameter is what resets them all
    // (engine_feed.h).
    if (on_sample_editor() && s_.sampleEditor.cursorRow == 11) { reset_slice_marker(); return; }

    // The pool's NAME column: A+B CLEARS the slot (M8's EDIT+OPTION). It frees the sample's PCM and,
    // if this was the SoundFont's last user, that .sf2's engine slot too — which is the whole reason it
    // is a host verb and not a field assignment. The instrument TYPE survives, so a SoundFont slot
    // stays a (now empty) SoundFont slot rather than silently becoming a sampler under the cursor.
    if (s_.currentScreen == ScreenType::INST_POOL && s_.poolCursorColumn == 0) {
        host_.clear_instrument(s_.currentInstrument);
        mark_modified();
        return;
    }

    generic_input(pt::ui::on_a_b);
}

// ─── A,A: insert the next UNUSED item ────────────────────────────────────────────────────────────

// ⚠️ A STATED SUPERSET OF KOTLIN, and the one place S8 deliberately adds a guard Kotlin does not have.
//
// Kotlin does NOT check the EQ editor in `handleAA`, `handleLA`, `handleLB`, `handleLBA` or `handleLR`.
// It gets away with it by accident: all five are gated on the screen, to SONG / CHAIN / PHRASE / TABLE /
// FILE_BROWSER — and the EQ editor can only be raised from INSTRUMENT, INST.POOL, MIXER, EFFECTS and the
// SAMPLE EDITOR. The two sets do not intersect, so every one of them is already inert under the overlay.
//
// That is a proof about today's screens, not about the gesture, and it is worth exactly nothing the day
// an EQ cell appears on a screen that has a clipboard. The guard costs a token; the accident costs a
// silent paste into a phrase you cannot see. It changes no observable behaviour on either platform
// (ptdispatch asserts the whole button set is inert under the overlay, which is the claim that actually
// matters and which holds with or without these lines).

void InputDispatcher::on_a_a() {
    if (overlay_swallows(Overlay::NONE)) return;

    // ⚠️ THE SAMPLE EDITOR'S ROW 11 NEEDS NO ARM HERE, though the fast pass it would be for is real —
    // it taps a boundary per hit, and a 16th note at 120 BPM is 125 ms, well inside the 300 ms window.
    // Its A is DEFERRED (`defer_a_to_release`) and the mapper clears `lastAPress` on every defer, so no
    // tap can reach this handler to be dropped by the insert-position gate below.

    // ⚠️ RESAMPLE is checked BEFORE the double-tap-position gate below, and it must be — Kotlin's
    // handleAA opens with the identical arm ahead of its InsertPosition logic. Under a SONG selection
    // the FIRST A press COPIED the selection rather than inserting an item, so `hasInsertPos_` was
    // never armed; the position gate would `return` and this arm would never run. Opening the RESAMPLE
    // keyboard here is the double-tap's whole meaning on a SONG selection.
    if (s_.currentScreen == ScreenType::SONG && s_.selection.active) {
        open_qwerty(QwertyContext::RESAMPLE, resample_base_name(fs_), "SAMPLE NAME:", "",
                    /*max_length=*/20, /*clear_on_first_b=*/true);
        return;
    }

    // A double-tap is only a double-tap if the cursor has not moved between the presses. Anything
    // else is two separate A presses, and each of those already did something (they inserted the
    // LAST-EDITED item — see on_button_a).
    if (!hasInsertPos_ || insertScreen_ != s_.currentScreen || insertRow_ != s_.cursorRow ||
        insertCol_ != s_.cursorColumn) {
        return;
    }
    hasInsertPos_ = false;

    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::SONG) {
        if (s_.cursorColumn < 1 || s_.cursorColumn > 8) return;
        songcore::Track& track = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)];

        const int next = first_from_wrapping(s_.lastEditedChain + 1, 256, [&](int i) {
            return chain_is_blank(p.chains[static_cast<size_t>(i)]);
        });
        if (next < 0) return;

        while (static_cast<int>(track.chainRefs.size()) <= s_.cursorRow) track.chainRefs.push_back(-1);
        track.chainRefs[static_cast<size_t>(s_.cursorRow)] = next;
        s_.lastEditedChain                                 = next;
        mark_modified();

    } else if (s_.currentScreen == ScreenType::CHAIN) {
        Chain& chain = p.chains[static_cast<size_t>(s_.currentChain)];

        const int next = first_from_wrapping(s_.lastEditedPhrase + 1, 256, [&](int i) {
            return phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
        });
        if (next < 0) return;

        chain.phraseRefs[static_cast<size_t>(s_.cursorRow)]      = next;
        chain.transposeValues[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedTranspose;
        s_.lastEditedPhrase                                      = next;
        mark_modified();

    } else if (s_.currentScreen == ScreenType::PHRASE) {
        // D1: advance the NOTE cell's instrument to the next FREE slot, keeping the note the first A
        // laid down. `instrument_is_free` (not a bare sampleFilePath==null) is the resample-safe
        // predicate — it skips configured SoundFonts, which also have a null sampleFilePath.
        if (s_.cursorColumn != 1) return;   // the NOTE column only
        Phrase&               ph   = p.phrases[static_cast<size_t>(s_.currentPhrase)];
        songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];

        const int count = static_cast<int>(p.instruments.size());
        const int next  = first_from_wrapping(s_.lastEditedInstrument + 1, count, [&](int i) {
            return songcore::instrument_is_free(p.instruments[static_cast<size_t>(i)]);
        });
        if (next < 0) return;

        step.instrument         = next;
        s_.lastEditedInstrument = next;
        mark_modified();
    }
}

// ─── B + D-pad: which item am I looking at? ──────────────────────────────────────────────────────

void InputDispatcher::cycle_current_item(int delta) {
    // ⚠️ The THEME editor SWALLOWS B+LEFT/RIGHT rather than doing anything with it (Kotlin's
    // `cycleCurrentItem` opens with the same line). It is not that there is nothing sensible to cycle —
    // B+LEFT/RIGHT could plausibly walk the built-in palettes — it is that A+LEFT/A+RIGHT already does, and
    // a second gesture for one job is a second thing to keep in step. Without this arm the press would
    // fall through to `currentScreen`, which is SETTINGS, whose `default:` arm does nothing — so the bug
    // would be invisible today and would arrive the day an EQ cell or a pool lands on SETTINGS.
    if (theme_open()) return;

    // ⚠️ In the EQ editor B+LEFT/RIGHT changes the SLOT — and it CLAMPS at 0 and 127 where every other
    // B+LEFT/RIGHT in the app wraps. A phrase pool is a ring you scroll through; the EQ bank is an index
    // you are pointing a mixer channel at, and wrapping from slot 127 back to 0 would silently re-point
    // it at a completely different curve.
    if (eq_open()) {
        const int newSlot = std::min(127, std::max(0, s_.eq.slotIndex + delta));
        s_.eq.slotIndex   = newSlot;
        apply_caller_eq_slot_change(newSlot);
        return;
    }

    // ⭐ ON SONG, B+LEFT/RIGHT TOGGLES THE TRANSPORT MODE — LGPT's own gesture, ported literally
    // because it was free here: the NAV=SONG arm below is gated on CHAIN and PHRASE, and the pool
    // switch under it has no SONG case, so this press has always reached `default: break;` and done
    // nothing. LEFT and RIGHT both toggle rather than one each: there are two modes, so a direction
    // that could only ever confirm the mode you are already in would be a button that does nothing.
    //
    // ⚠️ GATED ON SONG ALONE. The handler is shared: an ungated arm would swallow the pool cycle on
    // six screens and the song-relative walk on two.
    if (s_.currentScreen == ScreenType::SONG) {
        host_.set_live_mode(!host_.live_mode());
        return;
    }

    // ⭐ UNDER NAV = SONG, THIS WALKS THE SONG ROW instead of the pool — the whole gesture changes
    // meaning, so it takes the press before the pool arms below ever see it. CHAIN steps to the nearest
    // FILLED cell either side; PHRASE steps to the nearest one whose chain ALSO holds a phrase at the
    // chain row you are on, which is one predicate covering both reasons a track is skipped
    // (songcore/traversal.h). Both CLAMP: nothing to that side is a press that does nothing.
    //
    // ⚠️ `chainRow` is deliberately NOT reset by the move. It may then point at an empty row of the
    // chain you land on — which the CHAIN→PHRASE entry gate already refuses, so a second guard here
    // would only be a second place to get it wrong.
    if (s_.settings.navSongRelative &&
        (s_.currentScreen == ScreenType::CHAIN || s_.currentScreen == ScreenType::PHRASE)) {
        const int requireRow = (s_.currentScreen == ScreenType::PHRASE) ? pointer_chain_row(s_) : -1;
        const int songRow    = pointer_song_row(s_);
        const int track      = songcore::next_song_cell_h(*s_.project, songRow, pointer_track(s_),
                                                          delta, requireRow);
        set_pointer_song_cell(s_, songRow, track);
        refresh_song_relative_refs(s_);
        return;
    }

    // Kotlin's `(value + delta).mod(max + 1)` — a FLOORING modulo, so −1 wraps to the top rather than
    // staying at −1 the way C's % would.
    auto wrap = [delta](int value, int max) {
        const int n = max + 1;
        return ((value + delta) % n + n) % n;
    };

    switch (s_.currentScreen) {
        case ScreenType::CHAIN:
            s_.currentChain    = wrap(s_.currentChain, 255);
            s_.lastEditedChain = s_.currentChain;
            break;
        case ScreenType::PHRASE:
            s_.currentPhrase    = wrap(s_.currentPhrase, 255);
            s_.lastEditedPhrase = s_.currentPhrase;
            break;
        case ScreenType::TABLE:
            s_.currentTable    = wrap(s_.currentTable, 127);
            s_.lastEditedTable = s_.currentTable;
            break;
        case ScreenType::GROOVE:
            s_.currentGroove = wrap(s_.currentGroove, 127);
            break;
        case ScreenType::SCALE:
            s_.currentScale = wrap(s_.currentScale, songcore::POOL_SCALES - 1);
            break;
        // INSTRUMENT and MODS cycle the same thing — the instrument — because MODS *is* a view of one.
        // (INST_POOL is absent on purpose: there, the D-PAD already selects the instrument, so B+LEFT
        // would be a second, redundant way to do it. Kotlin has the same gap for the same reason.)
        case ScreenType::INSTRUMENT:
        case ScreenType::MODS:
            s_.currentInstrument    = wrap(s_.currentInstrument, 127);
            s_.lastEditedInstrument = s_.currentInstrument;
            break;
        default:
            break;
    }
}

// ⚠️ THE THEME AND EQ EDITORS ARE ARMED HERE, and their arms live inside `cycle_current_item` above
// rather than in the two lines below — one of them swallows B+LEFT/RIGHT and the other re-points the
// EQ slot with it.
void InputDispatcher::on_b_left() {
    if (overlay_swallows(Overlay::THEME | Overlay::EQ)) return;
    cycle_current_item(-1);
}

void InputDispatcher::on_b_right() {
    if (overlay_swallows(Overlay::THEME | Overlay::EQ)) return;
    cycle_current_item(+1);
}

// ⚠️ AN ANDROID BUG, FOUND BY PORTING — and it is the modal rule's own warning coming true.
//
// `handleBUp`/`handleBDown` are the ONLY two handlers in the Kotlin dispatcher that never got an
// `eqEditorState.isOpen` guard. Every other one has it. It survived because the guard is only MISSING
// where it is also needed: of the two screens these two handlers act on, SONG cannot raise the EQ
// editor at all — but INST.POOL can, from its column 4.
//
// So on Android: open the EQ from the pool's EQ column, hold B (which does NOT close the editor — the
// deferred-B latch is holding it), press UP. The B+DPAD arm fires, cancels the latch, and pages
// `currentInstrument` sixteen slots. The editor stays open on the instrument you opened it FROM (the
// caller is captured, so the bands are still right), and the pool cursor is now somewhere else
// entirely. Close it and you are looking at a different instrument than the one you were editing.
//
// Not corrupting, and that is exactly why nobody ever reported it: it reads as a mis-press. Zone B, so
// fixed on Android too (`AppInputDispatcher.handleBUp`/`handleBDown`), per §4's rule.

/**
 * B+UP/DOWN under NAV = SONG — the two directions the pool ruleset leaves FREE on both screens.
 *
 * On CHAIN it walks the track COLUMN: the nearest song row above or below whose cell in this track is
 * filled, GAPS SKIPPED. On PHRASE it walks the CHAIN's own filled rows and NEVER LEAVES THE CHAIN —
 * crossing chains vertically from there is deliberately not a gesture.
 *
 * ⚠️ Returns true even when the walk CLAMPS. The press was owned and its answer was "nothing to that
 * side"; falling through to the pool arms would page the song out from under the pointer instead.
 */
bool InputDispatcher::song_relative_b_vertical(int delta) {
    if (!s_.settings.navSongRelative) return false;

    if (s_.currentScreen == ScreenType::CHAIN) {
        const int track = pointer_track(s_);
        const int row   = songcore::next_song_cell_v(*s_.project, pointer_song_row(s_), track, delta);
        set_pointer_song_cell(s_, row, track);
        refresh_song_relative_refs(s_);
        return true;
    }
    if (s_.currentScreen == ScreenType::PHRASE) {
        const int row = songcore::next_chain_row(*s_.project, s_.currentChain, pointer_chain_row(s_),
                                                 delta, /*wrap=*/false);
        set_pointer_chain_row(s_, row);
        refresh_song_relative_refs(s_);
        return true;
    }
    return false;
}

void InputDispatcher::on_b_up() {
    if (overlay_swallows(Overlay::NONE)) return;
    if (song_relative_b_vertical(-1)) return;

    // The pool pages by 16 like the song does — but it CLAMPS at the ends where a single D-pad step
    // wraps 00↔7F. Paging past the end of a 128-slot list should stop at the end, not lap it.
    if (s_.currentScreen == ScreenType::INST_POOL) {
        s_.currentInstrument    = std::max(0, s_.currentInstrument - 16);
        s_.lastEditedInstrument = s_.currentInstrument;
        return;
    }
    if (s_.currentScreen != ScreenType::SONG) return;
    s_.cursorRow = std::max(0, s_.cursorRow - 16);   // TrackerController.moveSongBigUp
    scroll_song_to_row(s_, s_.cursorRow);
}

void InputDispatcher::on_b_down() {
    if (overlay_swallows(Overlay::NONE)) return;
    if (song_relative_b_vertical(+1)) return;

    if (s_.currentScreen == ScreenType::INST_POOL) {
        const int last = static_cast<int>(s_.project->instruments.size()) - 1;
        s_.currentInstrument    = std::min(last, s_.currentInstrument + 16);
        s_.lastEditedInstrument = s_.currentInstrument;
        return;
    }
    if (s_.currentScreen != ScreenType::SONG) return;
    s_.cursorRow = std::min(255, s_.cursorRow + 16);  // moveSongBigDown
    scroll_song_to_row(s_, s_.cursorRow);
}

// ─── R + D-pad: move between screens — except where it does not ──────────────────────────────────
//
// ⚠️ On the modals R+DPAD is NOT navigation, and this is the one place the modal rule pays for itself
// several times over. In the KEYBOARD, R+UP/DOWN switches layout (letters ↔ numbers) and R+LEFT/RIGHT
// moves the TEXT cursor — four bindings that have nowhere else to live on an eight-button device. In
// the BROWSER, R+UP/DOWN cycles the SORT MODE and R+LEFT goes UP A DIRECTORY, which is what its own
// bottom bar advertises ("R+<=UP R+^v=SORT"). In the SAMPLE EDITOR R+UP/DOWN is the waveform ZOOM and
// R+LEFT/RIGHT is swallowed. In the EQ EDITOR all four are simply SWALLOWED: the overlay has no cell in
// the 5×5 grid, so there is nowhere for R+DPAD to go FROM — and letting it navigate would leave the
// editor drawn over a screen it was never opened from, still writing into the caller that raised it.
//
// None of them may fall through to `navigate_*`: a browser is a popup, not a cell in the screen grid,
// and R+RIGHT out of one would land the user on a screen with the browser's cursor state still live.

void InputDispatcher::on_r_up() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::BROWSER)) return;
    if (qwerty_open()) { s_.qwerty.layout = 0; clamp_col(s_.qwerty); return; }
    if (on_browser()) { browser_cycle_sort(+1); return; }
    // Sample-editor ZOOM IN (v0.9.4 C3): R+UP/R+DOWN drive `zoomLevel` (0=1×…4=16×) without hopping to
    // the ZOOM row. The per-frame feed re-bins the waveform off `view_start`/`view_end`, so mutating the
    // level is the whole job. (SAMPLE_EDITOR is a popup, and `navigate_up`/`navigate_down` sit still on
    // it via their default arm — unlike the horizontal pair, which do NOT: see on_r_right.)
    if (on_sample_editor()) {
        if (s_.sampleEditor.showConfirmClose) return;   // the ARE YOU SURE? dialog owns the buttons
        s_.sampleEditor.zoomLevel = std::min(s_.sampleEditor.zoomLevel + 1, 4);
        return;
    }
    const NavState ns = nav_state_of(s_);
    go_to_screen(s_, navigate_up(ns));
    s_.selection.exit();   // a selection belongs to the screen it was made on
}

void InputDispatcher::on_r_down() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::BROWSER)) return;
    if (qwerty_open()) { s_.qwerty.layout = 1; clamp_col(s_.qwerty); return; }
    if (on_browser()) { browser_cycle_sort(-1); return; }
    if (on_sample_editor()) {   // ZOOM OUT — see on_r_up
        if (s_.sampleEditor.showConfirmClose) return;
        s_.sampleEditor.zoomLevel = std::max(s_.sampleEditor.zoomLevel - 1, 0);
        return;
    }
    const NavState ns = nav_state_of(s_);
    go_to_screen(s_, navigate_down(ns));
    s_.selection.exit();
}

void InputDispatcher::browser_cycle_sort(int delta) {
    FileBrowserState& b = s_.fileBrowser;

    // Step through the six modes BY INDEX, which is why the enum's declaration order is behaviour
    // rather than documentation (ui/filesystem.h).
    const int next = (static_cast<int>(b.sortMode) + delta + FILE_SORT_MODE_COUNT) % FILE_SORT_MODE_COUNT;
    b.sortMode = static_cast<FileSortMode>(next);

    // ⚠️ REBUILD, not `sort_items` on what is already there — see rebuild_items. Sorting the on-screen
    // list in place would make the tie-break depend on the sort mode you happened to arrive from.
    rebuild_items(b, fs_);

    // The cursor stays where it is (Android's does — `handleRUp` copies only `sortMode`), so the row
    // under it now holds a different file. That is the point: you are re-ordering the list you are
    // looking at, not jumping somewhere.
    b.statusMessage = file_sort_label(b.sortMode);
    b.statusSuccess = true;
}

// ─── The R+LEFT/R+RIGHT deep-link (AppInputDispatcher.syncLastEditedOnScreenSwitch) ──────────────
//
// What makes SONG-over-chain-04 → R+RIGHT land ON chain 04 rather than on chain 00. TWO halves,
// transcribed from :2760–2787:
//
//   • CAPTURE — the ref under the DEPARTING screen's cursor becomes the lastEdited memory. PHRASE
//     asks the module's own cursor_context whether the cell is empty (the CELL, column and all: an
//     empty FX cell of a noted step captures nothing); CHAIN and SONG guard on `ref >= 0`.
//   • APPLY — the ARRIVING screen deep-links its current* to the matching lastEdited*.
//
// ⚠️ HORIZONTAL MOVES ONLY, and only when the screen actually changes. Kotlin's handleRUp/handleRDown
// do plain cursor save/restore + selection exit (:2695/:2716) — sync them too and the port diverges
// the other way. ptdispatch §31 pins both directions of that trap.
void InputDispatcher::sync_last_edited_on_screen_switch(ScreenType from, ScreenType to) {
    const Project& p = *s_.project;

    switch (from) {
        case ScreenType::PHRASE:
            // `currentScreen` is still the departing PHRASE here, so cursor_context() is the same
            // question Kotlin puts to phraseEditorModule.getCursorContext. No `>= 0` guard on the
            // instrument, exactly as Kotlin has none (:2772) — the CLAMP below is what makes -1 safe.
            if (!cursor_context().capabilities.isEmpty) {
                s_.lastEditedInstrument = p.phrases[static_cast<size_t>(s_.currentPhrase)]
                                              .steps[static_cast<size_t>(s_.cursorRow)]
                                              .instrument;
            }
            break;

        case ScreenType::CHAIN: {
            const int ref = p.chains[static_cast<size_t>(s_.currentChain)]
                                .phraseRefs[static_cast<size_t>(s_.cursorRow)];
            if (ref >= 0) s_.lastEditedPhrase = ref;
            break;
        }

        case ScreenType::SONG: {
            // On SONG the cursor column IS the track, 1-based — and the size guard is load-bearing,
            // not defensive: a track's chainRefs vector may be SHORTER than the 256-row screen
            // (the model's default is empty, as Kotlin's mutableListOf() is).
            const auto& refs = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)].chainRefs;
            if (s_.cursorRow < static_cast<int>(refs.size()) &&
                refs[static_cast<size_t>(s_.cursorRow)] >= 0) {
                s_.lastEditedChain = refs[static_cast<size_t>(s_.cursorRow)];
            }
            break;
        }

        default:
            break;
    }

    switch (to) {
        case ScreenType::PHRASE: s_.currentPhrase = s_.lastEditedPhrase; break;
        case ScreenType::CHAIN:  s_.currentChain  = s_.lastEditedChain;  break;
        case ScreenType::INSTRUMENT: {
            // Kotlin assigns through the currentInstrument SETTER (TrackerController.kt:167–172),
            // which coerces into the pool and mirrors the CLAMPED value back into the memory — a
            // captured -1 must land on 00, not on "slot -1". Plain fields here, so both are said.
            const int last          = static_cast<int>(p.instruments.size()) - 1;
            s_.currentInstrument    = std::min(last, std::max(0, s_.lastEditedInstrument));
            s_.lastEditedInstrument = s_.currentInstrument;
            break;
        }
        default:
            break;
    }
}

int InputDispatcher::text_cursor_repeat_step() {
    const long long dt = now_ms_ - lastTextCursorMoveMs_;
    lastTextCursorMoveMs_ = now_ms_;
    if (dt > 250) { textCursorRepeatStreak_ = 0; return 1; }   // fresh gesture / the 400 ms initial gap
    ++textCursorRepeatStreak_;
    if (textCursorRepeatStreak_ < 4) return 1;                 // settle in at 1 char/repeat
    if (textCursorRepeatStreak_ < 9) return 2;                 // then 2×
    return 4;                                                  // then 4× — ~40 chars/s at the 100 ms cadence
}

void InputDispatcher::on_r_left() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::BROWSER)) return;
    if (qwerty_open()) {
        for (int n = text_cursor_repeat_step(); n > 0; --n) move_text_cursor_left(s_.qwerty);
        return;
    }
    if (on_sample_editor()) return;   // see on_r_right
    if (on_browser())  { navigate_to_parent(s_.fileBrowser, fs_); return; }
    const NavState ns = nav_state_of(s_);
    const NavResult r = navigate_left(ns);
    if (r.screen != s_.currentScreen) sync_last_edited_on_screen_switch(s_.currentScreen, r.screen);
    go_to_screen(s_, r);
    s_.selection.exit();
}

void InputDispatcher::on_r_right() {
    if (overlay_swallows(Overlay::QWERTY | Overlay::BROWSER)) return;
    if (qwerty_open()) {
        for (int n = text_cursor_repeat_step(); n > 0; --n) move_text_cursor_right(s_.qwerty);
        return;
    }
    // ⚠️ SWALLOWED on the sample editor, for the reason the EQ overlay is: it has no cell in the 5×5
    // grid, so `navigate_left`/`navigate_right` fall through their `!is_main_row` arm to
    // `main_screen_for_column(screen_column(SAMPLE_EDITOR))` = `main_screen_for_column(-1)` = PHRASE.
    // That is a way OUT of the editor that bypasses ARE YOU SURE? and discards an unsaved edit
    // silently — B is the only door, and it asks.
    if (on_sample_editor()) return;
    if (on_browser())  return;   // no "down a directory" — that is what A on a folder is for
    const NavState ns = nav_state_of(s_);
    const NavResult r = navigate_right(ns);
    // ⚠️ THE ENTRY GATE, and it sits ABOVE the sync rather than beside `go_to_screen`: under NAV = SONG
    // a refused press must leave NOTHING behind, and `sync_last_edited_on_screen_switch` writes the
    // lastEdited memory before the screen moves. See ui/song_pointer.h — R+RIGHT is the only gated
    // direction, because it is the only one that goes DEEPER into the arrangement.
    if (!song_relative_entry_allowed(s_, r.screen)) return;
    if (r.screen != s_.currentScreen) sync_last_edited_on_screen_switch(s_.currentScreen, r.screen);
    go_to_screen(s_, r);
    s_.selection.exit();
}

// ─── L: selection and the clipboard ──────────────────────────────────────────────────────────────

void InputDispatcher::on_l_b() {
    if (overlay_swallows(Overlay::BROWSER)) return;

    // ⚠️ The browser's selection is a DIFFERENT machine from the grid editors'. Theirs is the multi-tap
    // CELL→ROW→SCREEN widener (ui/selection.h); the browser's is a plain anchor..cursor RANGE over a
    // list, and its second tap inside the window means SELECT ALL rather than "widen the scope". They
    // share a button and a 500 ms window and nothing else, which is why they are two pieces of code.
    if (on_browser()) {
        FileBrowserState& b = s_.fileBrowser;
        if (b.mode != BrowserMode::NORMAL) return;

        if (!b.selectionMode) {
            b.selectionMode   = true;
            b.selectionAnchor = b.cursor;
            b.lastSelectTapMs = now_ms_;
        } else if (now_ms_ - b.lastSelectTapMs <= 500) {
            // Tap again inside the window: select everything, skipping the ".." row.
            const int first = b.first_selectable();
            const int last  = std::max(static_cast<int>(b.items.size()) - 1, first);
            b.selectionAnchor = first;
            b.cursor          = last;
            b.scroll          = std::max(0, last - BROWSER_VISIBLE_ROWS + 1);
            b.lastSelectTapMs = 0;   // …so a third tap re-anchors rather than re-selecting all
        } else {
            b.selectionAnchor = b.cursor;   // the window lapsed — start a fresh range here
            b.lastSelectTapMs = now_ms_;
        }
        return;
    }

    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
        case ScreenType::CHAIN:
        case ScreenType::SONG:
        case ScreenType::TABLE:
            s_.selection.handle_select_b(now_ms_, cursor_row(), cursor_column(),
                                         max_selection_column(), max_selection_row());
            break;
        default:
            break;  // GROOVE has one column and no clipboard type — nothing to select
    }
}

void InputDispatcher::on_l_a() {
    if (overlay_swallows(Overlay::BROWSER)) return;

    // On the browser L+A is the FILE clipboard's cut/paste — the same "inside a selection it cuts,
    // outside one it pastes" shape as the grid editors below, over files instead of cells.
    if (on_browser()) {
        FileBrowserState& b = s_.fileBrowser;
        if (b.mode != BrowserMode::NORMAL) return;

        if (b.selectionMode) {
            std::vector<std::string> files = browser_selected_paths();
            if (files.empty()) return;
            const size_t n = files.size();

            b.fileClipboard      = std::move(files);
            b.fileClipboardIsCut = true;
            b.selectionMode      = false;
            b.selectionAnchor    = -1;
            b.statusMessage = "CUT " + std::to_string(n) + (n == 1 ? " FILE" : " FILES");
            b.statusSuccess = true;
        } else if (!b.fileClipboard.empty()) {
            browser_paste();
        }
        return;
    }

    // Inside a selection L+A CUTS; outside one it PASTES. One button, two verbs, and which one you
    // get is a function of the selection — Kotlin's `handleSelectA()`, inlined because the C++
    // selection has no InputAction to return.
    Project& p = host_.edit_project();

    if (s_.selection.active) {
        const SelectionBounds b = s_.selection.bounds();
        switch (s_.currentScreen) {
            case ScreenType::PHRASE:
                clip_.cut_phrase_steps(p, s_.currentPhrase, b.topLeftRow, b.topLeftColumn,
                                       b.bottomRightRow, b.bottomRightColumn);
                break;
            case ScreenType::CHAIN:
                clip_.cut_chain_rows(p, s_.currentChain, b.topLeftRow, b.topLeftColumn,
                                     b.bottomRightRow, b.bottomRightColumn);
                break;
            case ScreenType::SONG:
                clip_.cut_song_cells(p, b.topLeftRow, b.topLeftColumn, b.bottomRightRow,
                                     b.bottomRightColumn);
                break;
            case ScreenType::TABLE:
                clip_.cut_table_rows(p, s_.currentTable, b.topLeftRow, b.topLeftColumn,
                                     b.bottomRightRow, b.bottomRightColumn);
                break;
            default:
                s_.selection.exit();
                return;
        }
        mark_modified();
        s_.selection.exit();
        return;
    }

    // Paste. The target id is the item being edited; SONG has none (its clip carries its own tracks).
    int targetId = 0;
    switch (s_.currentScreen) {
        case ScreenType::PHRASE: targetId = s_.currentPhrase; break;
        case ScreenType::CHAIN:  targetId = s_.currentChain;  break;
        case ScreenType::TABLE:  targetId = s_.currentTable;  break;
        default:                 targetId = 0;                break;
    }
    const PasteResult r =
        clip_.paste(p, s_.currentScreen, targetId, cursor_row(), cursor_column());
    if (r.kind == PasteResult::Kind::SUCCESS && r.itemsPasted > 0) mark_modified();
}

// ─── R+A / R+B: MUTE and SOLO ────────────────────────────────────────────────────────────────────

void InputDispatcher::mute_solo_targets(int (&out)[8], int& count) const {
    count = 0;
    switch (s_.currentScreen) {
        case ScreenType::SONG: {
            // A selection makes the chord act on every channel it covers — the reason the gesture is
            // worth having on a tracker at all: L+B+B selects the row, R+B drops the whole mix out.
            if (s_.selection.active) {
                const SelectionBounds b = s_.selection.bounds();
                for (int col = b.topLeftColumn; col <= b.bottomRightColumn; ++col)
                    if (col >= 1 && col <= 8) out[count++] = col - 1;
                return;
            }
            if (s_.cursorColumn >= 1 && s_.cursorColumn <= 8) out[count++] = s_.cursorColumn - 1;
            return;
        }
        case ScreenType::MIXER:
            // ⚠️ Column 8 is the MASTER strip and has no mute of its own — the chord is a no-op there
            // rather than muting track 8, which does not exist. The selection is not consulted: it
            // belongs to the grid editors, and a stale one from SONG must not reach across.
            if (s_.mixerCursorColumn >= 0 && s_.mixerCursorColumn <= 7)
                out[count++] = s_.mixerCursorColumn;
            return;
        default:
            return;   // every other screen: the chord is the consumed no-op it has always been
    }
}

void InputDispatcher::toggle_mute_solo(bool solo) {
    // Ordering, not bookkeeping: a selection made earlier in THIS batch of events has to be seen as
    // older than the toggle about to happen. See `run_selection_recency()`.
    run_selection_recency();

    int targets[8];
    int count = 0;
    mute_solo_targets(targets, count);
    if (count == 0) return;

    Project& p = host_.edit_project();

    // ⚠️ ALL EIGHT PAIRS, and only on the FIRST toggle of a chord. A revert has to undo everything the
    // chord did — a selection touches several channels, and a solo changes what the other seven are
    // heard doing — and re-snapshotting per press would leave it able to undo only the last one.
    if (!mixSnapshot_.live) {
        for (int i = 0; i < 8 && i < static_cast<int>(p.tracks.size()); ++i) {
            mixSnapshot_.mute[i] = p.tracks[static_cast<size_t>(i)].mute;
            mixSnapshot_.solo[i] = p.tracks[static_cast<size_t>(i)].solo;
        }
        mixSnapshot_.live = true;
    }

    for (int i = 0; i < count; ++i) {
        if (targets[i] >= static_cast<int>(p.tracks.size())) continue;
        songcore::Track& t = p.tracks[static_cast<size_t>(targets[i])];
        if (solo) t.solo = !t.solo;
        else      t.mute = !t.mute;
    }

    s_.lastClearable = AppState::Clearable::MUTE;

    // ⚠️ NO mark_dirty_and_arm_autosave(). Muting a channel is a PERFORMANCE action, not an edit to
    // the document: it is saved when the project is saved for some other reason, but on its own it
    // must not arm the 3 s autosave, must not make the song read as dirty, and must not put a
    // "RECOVER WORK?" in front of the next launch. `push_globals()` is what makes it audible, and it
    // sweeps all eight tracks because a solo changes the answer for the seven it was not aimed at.
    host_.push_globals();
}

void InputDispatcher::restore_full_playback() {
    Project& p = host_.edit_project();
    for (songcore::Track& t : p.tracks) { t.mute = false; t.solo = false; }
    s_.lastClearable = AppState::Clearable::NONE;
    host_.push_globals();
}

void InputDispatcher::on_r_b() {
    if (!mute_solo_chord_live()) return;
    toggle_mute_solo(/*solo=*/false);
}

void InputDispatcher::on_r_a() {
    if (!mute_solo_chord_live()) return;
    toggle_mute_solo(/*solo=*/true);
}

// The one question both the chord and the deferred B ask: is R+A/R+B a MUTE/SOLO here, right now?
// Derived rather than restated at the three sites, so a screen that grows the chord tomorrow starts
// deferring its B on the same day.
bool InputDispatcher::mute_solo_chord_live() const {
    if (overlay_swallows(Overlay::NONE)) return false;   // a modal owns the buttons while it is up
    return s_.currentScreen == ScreenType::SONG || s_.currentScreen == ScreenType::MIXER;
}

void InputDispatcher::on_r_combo_commit() {
    // R came up first, so what the chord did stands. Dropping the snapshot is the whole of it — the
    // next chord takes a fresh one. (A real handler rather than an absent one: the mapper's release
    // ordering is the specification, and a closer that never ran must be distinguishable from one
    // that ran and had nothing to do.)
    mixSnapshot_.live = false;
}

void InputDispatcher::on_r_combo_revert() {
    if (!mixSnapshot_.live) return;   // the chord armed on a screen that has no channels
    Project& p = host_.edit_project();
    for (int i = 0; i < 8 && i < static_cast<int>(p.tracks.size()); ++i) {
        p.tracks[static_cast<size_t>(i)].mute = mixSnapshot_.mute[i];
        p.tracks[static_cast<size_t>(i)].solo = mixSnapshot_.solo[i];
    }
    mixSnapshot_.live = false;
    host_.push_globals();
}

unsigned InputDispatcher::selection_signature() const {
    unsigned h = clip_.has_data() ? 1u : 0u;
    h = h * 31u + static_cast<unsigned>(clip_.type());
    h = h * 31u + static_cast<unsigned>(clip_.width());
    h = h * 31u + static_cast<unsigned>(clip_.height());
    h = h * 31u + (s_.selection.active ? 1u : 0u);
    if (s_.selection.active) {
        const SelectionBounds b = s_.selection.bounds();
        h = h * 31u + static_cast<unsigned>(b.topLeftRow);
        h = h * 31u + static_cast<unsigned>(b.topLeftColumn);
        h = h * 31u + static_cast<unsigned>(b.bottomRightRow);
        h = h * 31u + static_cast<unsigned>(b.bottomRightColumn);
    }
    return h;
}

void InputDispatcher::run_selection_recency() {
    const unsigned sig = selection_signature();
    if (sig == selectionSig_) return;
    selectionSig_    = sig;
    s_.lastClearable = AppState::Clearable::SELECTION;
}

void InputDispatcher::on_l_r() {
    if (overlay_swallows(Overlay::BROWSER)) return;
    if (on_browser()) {
        s_.fileBrowser.selectionMode   = false;
        s_.fileBrowser.selectionAnchor = -1;
        return;
    }

    // ── ONE PRESS UNDOES ONE THING, MOST RECENT FIRST ────────────────────────────────────────────
    //
    // Two rungs: the mix (any track muted or soloed) and the selection with its buffer.
    // `s_.lastClearable` says which the user touched last, and that one is tried first — clearing
    // both at once would throw away a selection someone built press by press just because they also
    // dropped a channel out of the mix.
    //
    // ⚠️ A rung with nothing to clear FALLS THROUGH to the other. Without that, L+R reads as a dead
    // button whenever the most recent thing is already empty — and the recency flag survives the
    // clear that emptied it, so that is not a rare state.
    //
    // ⚠️ The SAMPLE_EDITOR exclusion covers this rung too, for the reason it covers the clipboard's:
    // L+R is reserved there for the editor's own selection, and one screen with two exclusion lists
    // is a special case someone has to remember.
    const bool mix_touched = [&] {
        if (s_.currentScreen == ScreenType::SAMPLE_EDITOR) return false;
        const Project& p = host_.project();
        for (const songcore::Track& t : p.tracks)
            if (t.mute || t.solo) return true;
        return false;
    }();

    if (s_.lastClearable == AppState::Clearable::MUTE && mix_touched) {
        restore_full_playback();
        return;
    }

    // Two-state rule (v0.9.4 C4), and the gate is `selection.active`, nothing else:
    //   • SELECTING → leave selection mode, but the copy buffer MUST SURVIVE — you might re-enter a
    //     selection by accident and must not lose what you copied. `selection.exit()` does not touch
    //     `clip_`, so the buffer is untouched.
    //   • NOT selecting → CLEAR the buffer. It is the only way to dismiss the top-strip clipboard
    //     readout (`Clipboard::info()`) short of a restart.
    if (s_.selection.active) {
        s_.selection.exit();   // buffer untouched
        return;
    }

    // ⚠️ A DENY-list, not an allow-list, and that is the point: the readout is drawn on the top strip
    // of EVERY screen, so scoping the clear to the four screens that can fill the buffer left it
    // undismissable from the other ten. The two exclusions above this line stand (the overlays own
    // every button; the browser's L+R cancels its own selection, which its hint bar advertises).
    //
    // ⚠️ SAMPLE_EDITOR is excluded: L+R is reserved there for the editor's own selection, and a
    // generic clear would shadow it. GROOVE and the other one-column screens are NOT excluded — a
    // clear there is a no-op that costs nothing, which beats a special case someone has to remember.
    const bool had_buffer = !clip_.info().empty();
    if (s_.currentScreen != ScreenType::SAMPLE_EDITOR) clip_.clear();

    // Nothing on the selection rung to clear, but the mix has something: take it rather than leave
    // the press doing nothing at all.
    if (!had_buffer && mix_touched) restore_full_playback();
}

// ─── L+B+A: clone ────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_l_b_a() {
    if (overlay_swallows(Overlay::NONE)) return;

    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::SONG) {
        if (s_.cursorColumn < 1 || s_.cursorColumn > 8) { s_.selection.exit(); return; }
        songcore::Track& track = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)];
        const int currentChainId =
            (s_.cursorRow < static_cast<int>(track.chainRefs.size()))
                ? track.chainRefs[static_cast<size_t>(s_.cursorRow)]
                : -1;

        if (currentChainId != -1) {
            const Chain&        src         = p.chains[static_cast<size_t>(currentChainId)];
            const std::set<int> usedChains  = used_chain_ids(p);
            const std::set<int> usedPhrases = used_phrase_ids(p);

            // The destination must be a FREE chain: blank AND unreferenced.
            const int dstChainId = first_from_wrapping(currentChainId + 1, 256, [&](int i) {
                return usedChains.count(i) == 0 && chain_is_blank(p.chains[static_cast<size_t>(i)]);
            });

            // A DEEP clone: every phrase the chain references gets its own free slot, so the copy is
            // fully independent. `reserved` stops two source phrases claiming the same destination;
            // duplicate refs inside the chain map to the SAME clone, which is what keeps a chain that
            // plays phrase 5 twice still playing one phrase twice.
            std::vector<int>   srcPhraseIds;
            for (const int ref : src.phraseRefs)
                if (ref != -1 &&
                    std::find(srcPhraseIds.begin(), srcPhraseIds.end(), ref) == srcPhraseIds.end())
                    srcPhraseIds.push_back(ref);

            std::set<int>      reserved;
            std::map<int, int> phraseMap;
            bool               enoughPhrases = true;
            for (const int pid : srcPhraseIds) {
                const int slot = first_from_wrapping(0, 256, [&](int i) {
                    return reserved.count(i) == 0 && usedPhrases.count(i) == 0 &&
                           phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
                });
                if (slot < 0) { enoughPhrases = false; break; }
                reserved.insert(slot);
                phraseMap[pid] = slot;
            }

            // Capacity is checked in FULL before anything is written. Abort, never half-clone: a
            // partial clone leaves a chain pointing at phrases that were never copied.
            if (dstChainId < 0) {
                s_.statusMessage = "NO FREE CHAINS";
                s_.statusSuccess = false;
            } else if (!enoughPhrases) {
                s_.statusMessage = "NO FREE PHRASES";
                s_.statusSuccess = false;
            } else {
                for (const auto& kv : phraseMap)
                    p.phrases[static_cast<size_t>(kv.second)].steps =
                        p.phrases[static_cast<size_t>(kv.first)].steps;

                Chain& dst = p.chains[static_cast<size_t>(dstChainId)];
                for (size_t i = 0; i < src.phraseRefs.size(); ++i) {
                    const int ref     = src.phraseRefs[i];
                    dst.phraseRefs[i] = (ref == -1) ? -1 : phraseMap[ref];
                }
                dst.transposeValues = src.transposeValues;

                track.chainRefs[static_cast<size_t>(s_.cursorRow)] = dstChainId;
                s_.lastEditedChain = dstChainId;
                s_.statusMessage   = "CHAIN CLONED";
                s_.statusSuccess   = true;
                mark_modified();
            }
        }

    } else if (s_.currentScreen == ScreenType::CHAIN) {
        Chain&    chain           = p.chains[static_cast<size_t>(s_.currentChain)];
        const int currentPhraseId = chain.phraseRefs[static_cast<size_t>(s_.cursorRow)];
        if (currentPhraseId != -1) {
            const std::set<int> usedPhrases = used_phrase_ids(p);
            const int next = first_from_wrapping(currentPhraseId + 1, 256, [&](int i) {
                return usedPhrases.count(i) == 0 && phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
            });
            if (next >= 0) {
                p.phrases[static_cast<size_t>(next)].steps =
                    p.phrases[static_cast<size_t>(currentPhraseId)].steps;
                chain.phraseRefs[static_cast<size_t>(s_.cursorRow)] = next;
                s_.lastEditedPhrase                                 = next;
                mark_modified();
            }
        }

    } else if (s_.currentScreen == ScreenType::PHRASE) {
        const int srcPhraseId = s_.currentPhrase;
        const std::set<int> usedPhrases = used_phrase_ids(p);
        const int next = first_from_wrapping(srcPhraseId + 1, 256, [&](int i) {
            return i != srcPhraseId && usedPhrases.count(i) == 0 &&
                   phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
        });
        if (next >= 0) {
            p.phrases[static_cast<size_t>(next)].steps =
                p.phrases[static_cast<size_t>(srcPhraseId)].steps;
            s_.currentPhrase = next;   // …and follow the clone, so you are editing the copy
            mark_modified();
        }
    }

    s_.selection.exit();
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
// PROJECT + SETTINGS (Phase 3 S7)
// ═════════════════════════════════════════════════════════════════════════════════════════════════

void InputDispatcher::confirm_accept() {
    const ConfirmDialogState::Kind kind = s_.confirm.kind;
    // ⚠️ `arg` is read out HERE, beside the kind, because `close()` resets it — and it is the direction
    // the CHANGE_TYPE arm below needs. Reading it after the close would have silently turned every
    // confirmed A+LEFT into an A+RIGHT, which is a wrong answer that still looks like the cell working.
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
            // ⚠️ `clean_inst` also RELOADS the media and re-pushes the params, and both are needed:
            // compacting emptied the unused instrument slots in the DOCUMENT, but their sample and
            // SoundFont buffers are still in the engine — so without the reload the RAM would not drop
            // until the project was saved and opened again. See SongcoreHost::clean_inst.
            host_.clean_inst(fs_.samples_directory());
            mark_modified();
            {
                // The reload can fail the same way a project load can — see load_project_done.
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
            // ⚠️ S4 built the TYPE toggle and then REFUSED to fire it on a slot with a source loaded,
            // because switching a sampler to a SoundFont frees its sample and there was no dialog to
            // ask first. This is that dialog. `toggle_instrument_type` is unchanged; what changed is
            // that it can now be reached destructively, having asked.
            //
            // ⚠️ The DIRECTION comes back out of the dialog (`arg`), not from a default: A+LEFT on a
            // loaded slot must still step backwards after the user says yes, and with three types
            // that is a different answer from A+RIGHT's.
            toggle_instrument_type(argv);
            break;

        case ConfirmDialogState::Kind::EXIT:
            // ⚠️ **A CONFIRMED EXIT IS A CLEAN EXIT, SO IT LEAVES NOTHING TO RECOVER.** The user was
            // shown their unsaved work, and said quit anyway — that is a decision, and the autosave has
            // to honour it. Leave the file behind and the next launch offers to restore precisely the
            // work they just chose to abandon.
            //
            // ⚠️ Which is also why the dialog SURVIVES the autosave rather than being made redundant by
            // it. The obvious "there is an autosave now, so EXIT can stop asking" is wrong twice over:
            // it removes the only way to deliberately discard a session, and it makes quitting silently
            // preserve a document the user thought they were throwing away. So EXIT still asks (S7),
            // and its YES is the app's one *clean* death.
            //
            // Everything that is NOT this — SIGTERM from a launcher's menu, a flat battery, a crash, the
            // F10 escape hatch — is an UNCLEAN death, and those are the ones `flush_autosave()` catches
            // on the way out of the frame loop. The user was never asked; the work is kept.
            autosave_clear(fs_);
            s_.shouldQuit = true;
            break;

        case ConfirmDialogState::Kind::RECOVER:
            // A = recover. The document comes back DIRTY and the file STAYS — see recover_from_autosave
            // for why both of those are deliberate. A failure has already dropped the file (below), so
            // there is nothing to clean up here.
            if (!recover_from_autosave()) autosave_clear(fs_);
            break;

        case ConfirmDialogState::Kind::NONE:
            break;
    }
}

void InputDispatcher::confirm_cancel() {
    const ConfirmDialogState::Kind kind = s_.confirm.kind;
    s_.confirm.close();

    // ⚠️ The ONE question whose NO is an ACTION. For the other five, "no" means the world is exactly as
    // it was and closing the box is the whole of it. Here it means *discard my unsaved work* — and a
    // discard that leaves the file on disk is not a discard: the prompt would return on the next launch,
    // and the next, about work the user has already refused. That is how a safety prompt teaches people
    // to dismiss it without reading. (Kotlin: `showRecoveryDialog = false; fileController.clearAutosave()`.)
    if (kind == ConfirmDialogState::Kind::RECOVER) autosave_clear(fs_);
}

/**
 * The editing context, back to zero — TrackerController.resetEditingContext.
 *
 * Shared by NEW and LOAD, and it is not cosmetic: leaving `currentInstrument` at 0x40 after opening a
 * document whose instruments are all empty would put INSTRUMENT on a slot the user never chose, and
 * leaving `lastEditedChain` at 0x2F would have A,A on SONG insert a chain from the PREVIOUS song.
 */
void InputDispatcher::reset_editing_context() {
    s_.currentPhrase = s_.currentChain = s_.currentInstrument = 0;
    s_.currentTable  = s_.currentGroove = 0;
    s_.lastEditedPhrase = s_.lastEditedChain = s_.lastEditedTable = 0;
    s_.lastEditedInstrument = s_.lastEditedTranspose = 0;
    s_.lastEditedNote   = songcore::Note::C4();
    s_.lastEditedVolume = 0x7F;

    // …and EVERY secondary screen's own cursor, exactly the set Kotlin resets (:286–300). Resetting
    // a subset left INSTRUMENT / MIXER / EFFECTS / TABLE / GROOVE / MODS / PROJECT — and the
    // REMEMBER slots below — pointing into the PREVIOUS song after a LOAD (parity audit, finding 4).
    s_.cursorRow = 0; s_.cursorColumn = 1;
    s_.songScrollPosition = 0;
    s_.instrumentCursorRow = 0; s_.instrumentCursorColumn = 1;
    s_.mixerCursorColumn = 0;
    s_.effectsCursorRow  = 0;
    s_.tableCursorRow = 0; s_.tableCursorColumn = 1;
    s_.grooveCursorRow = 0;
    s_.modCursorRow = 0; s_.modCursorPair = 0; s_.modCursorSide = 0;
    s_.projectCursorRow = 0; s_.projectCursorColumn = 1;

    // The three REMEMBER slots (Kotlin's resetCursorRememberPositions) — or REMEMBER mode restores
    // a cursor that was saved inside the previous song.
    s_.songCursorRow = 0;   s_.songCursorColumn = 1;
    s_.chainCursorRow = 0;  s_.chainCursorColumn = 1;
    s_.phraseCursorRow = 0; s_.phraseCursorColumn = 1;

    // ⚠️ NOT mixerMasterRow, NOT the SETTINGS cursor, NOT poolCursorColumn: Kotlin leaves all three
    // alone (the pool's ROW is currentInstrument, which IS reset above). Match the quirk exactly —
    // ptdispatch §32 pins the negatives too.
    s_.selection = Selection{};

    // ⚠️ …AND UNDER NAV = SONG THE THREE REMEMBER SLOTS ABOVE ARE THE POINTER, so "reset to 0" aims it
    // at song row 0 / track 1 — a cell the document just loaded need not have. The entry gate then
    // refuses CHAIN, correctly and silently, and the user cannot leave SONG until they find a filled
    // cell themselves. So it is DERIVED from the arrangement here, below every reset, rather than
    // remembered. A no-op under NAV = POOL, where these are only a convenience.
    clamp_song_pointer(s_);
}

void InputDispatcher::start_new_project() {
    host_.new_project();

    // Blank document: nothing is unsaved and nothing has a path. Both matter — without the version
    // reset the very next NEW or EXIT would ask "unsaved work?" about a project nobody has touched.
    s_.projectVersion      = 0;
    s_.savedProjectVersion = 0;
    s_.projectPath.clear();

    // …and therefore nothing to recover. A clean transition DELETES the autosave, and the deletions are
    // as load-bearing as the writes: leave the file behind here and the next launch offers to restore a
    // song the user deliberately started over from. (TrackerController.newProject: "fresh project,
    // nothing to recover".) The pending deadline goes with it — it would otherwise fire three seconds
    // from now and write the blank document straight back out.
    autosavePending_ = false;
    autosave_clear(fs_);

    reset_editing_context();

    s_.statusMessage = "NEW PROJECT";
    s_.statusSuccess = true;
}

/** A .ptp just replaced the document. Leave the browser, and forget everything about the last one. */
void InputDispatcher::load_project_done(const std::string& path) {
    // A freshly LOADED project is CLEAN — it is exactly what is on disk, so there is nothing unsaved and
    // nothing to recover. (Kotlin aligns the two versions AND clears the autosave on load, for the two
    // halves of the same reason.) ⚠️ The one load path that deliberately does NEITHER is autosave
    // recovery — see recover_from_autosave, which is the exception this comment used to promise.
    s_.projectVersion      = 0;
    s_.savedProjectVersion = 0;
    s_.projectPath         = path;

    autosavePending_ = false;   // …or it fires 3 s from now and re-creates the file this just deleted
    autosave_clear(fs_);

    reset_editing_context();

    close_file_browser();

    // ⚠️ **A SAMPLE THAT DID NOT LOAD LEAVES ITS INSTRUMENT SILENT AND SAYS NOTHING OTHERWISE.** The
    // launch path prints a warning to stderr; the two platforms this port is aimed at have no console
    // to print it to, so without this the only symptom is a track that does not sound. It is also
    // where an out-of-memory sample lands on a 512 MB device — a path that handles the failure
    // correctly and then had nowhere to report it.
    const int failed = host_.last_media_load().failed;
    s_.statusMessage = failed > 0 ? "LOADED: " + std::to_string(failed) + " MISSING" : "LOADED";
    s_.statusSuccess = (failed == 0);
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

    const ActionResult r = stems ? render_stems(host_, fs_, s_, progress)
                                 : render_mix(host_, fs_, s_, progress);

    s_.isRendering    = false;
    s_.renderProgress = 0.0f;
    s_.statusMessage  = r.message;
    s_.statusSuccess  = r.ok;

    if (render_.suspend_audio) render_.suspend_audio(false);
}

void InputDispatcher::resample_selection(const std::string& customBaseName) {
    if (s_.isRendering) return;         // a render is already running — a second APPLY is a mis-press
    if (!s_.selection.active) return;   // the selection lapsed between opening the keyboard and APPLY

    // The selected TRACKS: SONG columns are 1-indexed (col 1 = track 0), so column − 1 is the track id,
    // clamped to the eight that exist. Kotlin: `(topLeftColumn-1..bottomRightColumn-1).filter { 0..7 }`.
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
            // Kotlin: "RESAMPLED → INST 05". ASCII arrow, because the status line's 5×5 font has no
            // U+2192 — a → would draw as tofu on the very screen this is meant to reassure.
            std::snprintf(msg, sizeof(msg), "RESAMPLED -> INST %02X", instId);
            s_.statusMessage = msg;
            s_.statusSuccess = true;
            mark_modified();   // Kotlin's projectVersion++ — a new instrument is unsaved work
        } else {
            // The WAV rendered but no slot could take it (pool full, or the fresh file will not reload).
            // Kotlin sets its own text inside createResampledInstrument; one line is enough here.
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
            // ⚠️ Nothing, and that is not a gap. A on the NAME row opens the KEYBOARD — but it is one of
            // the six DEFERRED cells, so `open_sub_screen_at_cursor` has already handled it and returned
            // before `on_button_a` ever reached this switch. The arm stays, empty and named, because a
            // silent `default:` here is how the row would quietly acquire a second, divergent opener.
            break;

        case ProjectRow::PROJECT:
            switch (s_.projectCursorColumn) {
                case 1: {   // SAVE
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
                    // ⚠️ Only ASK if there is something to lose. A clean project has nothing to
                    // confirm, and a dialog that always appears is a dialog nobody reads.
                    if (s_.project_dirty()) s_.confirm.open(ConfirmDialogState::Kind::NEW_PROJECT);
                    else                    start_new_project();
                    break;
                default: break;
            }
            break;

        case ProjectRow::EXPORT:
            if (s_.projectCursorColumn == 1)      export_song(/*stems=*/false);
            else if (s_.projectCursorColumn == 2) export_song(/*stems=*/true);
            break;

        case ProjectRow::COMPACT:
            if (s_.projectCursorColumn == 1)
                s_.confirm.open(ConfirmDialogState::Kind::CLEAN_SEQ);
            else if (s_.projectCursorColumn == 2)
                s_.confirm.open(ConfirmDialogState::Kind::CLEAN_INST);
            break;

        case ProjectRow::SYSTEM: {
            // A shortcut INTO a screen the nav grid can also reach (SETTINGS is one of the twelve).
            // It keeps the column it came from — SETTINGS owns none, exactly as PROJECT owns none — so
            // R+UP out of it later returns to the main-row screen you were on, not to a fixed default.
            //
            // …and B's way out is a SECOND, dedicated target, captured here exactly as Kotlin captures it
            // on this same gesture (`settingsReturnScreen = currentScreen`, AppInputDispatcher.kt:1666).
            // See AppState::settingsReturnScreen for why it cannot just be `previousScreen`.
            s_.settingsReturnScreen = s_.currentScreen;
            NavResult nav;
            nav.screen = ScreenType::SETTINGS;
            nav.column = s_.previousColumn;
            go_to_screen(s_, nav);
            break;
        }

        case ProjectRow::MIDI: {
            // ⚠️ The row is not drawn where the build hides the MIDI surfaces, and this guard is what
            // makes that a real gate rather than a cosmetic one: PROJECT is the MIDI screen's ONLY
            // door (it is not one of the twelve R+DPAD cells), so anything that reaches this case with
            // the cap off — a stale cursor, a future caller — must be turned back here.
            if (!s_.caps.midi) break;

            // The same shortcut shape as SYSTEM above, minus the nav grid. B is the only way back.
            //
            // ⚠️ THE ENUMERATION HAPPENS HERE, ON THE WAY IN. See refresh_midi_devices(): a port list is
            // only true at the moment it is read, and this is the moment the user is about to read it.
            // ⚠️ BOTH lists, since E3 — the INPUT row is as hot-pluggable as the OUTPUT one, and a
            // screen that refreshed one of them would show a stale answer on the other for as long as
            // the user stayed on it.
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
            // ⚠️ The shell only — and gated on the same question NEW asks. It still asks, now that S10
            // has built the autosave, and that is deliberate: the dialog is the app's ONE way to
            // deliberately throw a session away, and its YES is the app's one clean death (which is why
            // confirm_accept's EXIT arm deletes the autosave). Everything else — the launcher's kill, a
            // flat battery, F10 — is unclean, and the work is kept.
            if (!s_.caps.appExit) break;
            if (s_.project_dirty()) s_.confirm.open(ConfirmDialogState::Kind::EXIT);
            else                    s_.shouldQuit = true;
            break;

        // TEMPO / TRANSPOSE are A+DPAD cells. Plain A does nothing on them, as it does nothing on any
        // value cell in the app.
        default:
            break;
    }
}

void InputDispatcher::settings_action() {
    switch (static_cast<SettingsRow>(s_.settingsCursorRow)) {
        case SettingsRow::THEME:
            // The arrow S7 drew as a promise. A opens the editor (S9) — the last screen in the Kotlin
            // dispatcher the port had not reached.
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

        // Every other row is a VALUE, and a value changes with A+DPAD. Kotlin says so in a comment at
        // the top of SettingsModule ("Single A is reserved for actions only"), and it is why this
        // switch has exactly two arms.
        default:
            break;
    }
}

// ─── MIDI (phase B4.3) ───────────────────────────────────────────────────────────────────────────

void InputDispatcher::boot_midi_port() {
    // The OFFSET first and unconditionally — it is a number the consumer needs whether or not a port
    // ever opens, and forgetting it is the "a setting that round-trips is not a setting that is
    // applied" bug in its purest form: the value would sit correct in settings.json, be drawn correctly
    // on the screen, and change nothing anybody could hear.
    host_.set_midi_offset_ms(s_.settings.midiOffsetMs);
    // SYNC (phase C) for exactly the same reason, and it is the more dangerous of the two to forget:
    // OFFSET being unapplied is a few milliseconds nobody measures, but SYNC being unapplied means a
    // user who turned it on last session, saw ON when they came back, and got no clock at all.
    host_.set_midi_sync_out(s_.settings.midiSyncOut);

    refresh_midi_devices();
    if (port_open()) return;                       // the env override already opened this same device
    if (s_.midiDeviceIndex != 0) apply_midi_device();
    s_.midiStatusText.clear();                     // boot news is the console's job, not the screen's
}

// Resolve a SAVED NAME against the list that exists right now. Not found → 0 → OFF, and that rule is
// the whole reason the setting is a name and not an index: an index would silently come back pointing
// at whatever port took its place. One definition for all four call sites (both directions of both
// the OUT and IN pairs) so the not-found rule cannot be spelled differently in one of them.
//
// Index 0 is "OFF" and is never a device, so the search starts at 1.
static int resolve_port_index(const std::vector<std::string>& names, const std::string& want) {
    for (size_t i = 1; i < names.size(); ++i)
        if (names[i] == want) return static_cast<int>(i);
    return 0;
}

void InputDispatcher::refresh_midi_devices() {
    s_.midiDeviceNames.assign(1, "OFF");   // index 0, always — the module never handles "no device"

    if (s_.midiOut) {
        const int n = s_.midiOut->device_count();
        for (int i = 0; i < n; ++i) s_.midiDeviceNames.push_back(s_.midiOut->device_name(i));
    }

    s_.midiDeviceIndex = resolve_port_index(s_.midiDeviceNames, s_.settings.midiOutDevice);
}

void InputDispatcher::apply_midi_device() {
    // Re-resolve first: `settings.midiOutDevice` is the choice, `midiDeviceIndex` is where that choice
    // sits in the list the screen is drawing, and the module just changed the former.
    const int wanted = resolve_port_index(s_.midiDeviceNames, s_.settings.midiOutDevice);
    s_.midiDeviceIndex = wanted;

    if (!s_.midiOut) { s_.midiStatusText = "NO MIDI BACKEND"; return; }

    // ⚠️ THE PANIC IS OURS TO SEND — see the header. `set_out` panics on a POINTER change and the
    // pointer is not changing; only the device behind it is. Skip this and every note sounding on the
    // port we are about to close is held by that hardware until someone power-cycles it.
    host_.midi_out().panic();
    s_.midiOut->close();

    if (wanted == 0) {
        s_.midiStatusText = "OUTPUT OFF";
    } else if (s_.midiOut->open(wanted - 1)) {   // −1: index 0 of the list is OFF, not a device
        s_.midiStatusText = "PORT OPENED";
    } else {
        // ⚠️ SAID OUT LOUD, and the setting is left alone. A port that refuses to open is usually one
        // another app already holds exclusively — a transient the user can fix and retry — so throwing
        // their choice away on the first failure would be the wrong repair. The row reads OFF because
        // `is_open()` is false, which is the truth.
        s_.midiStatusText = "PORT BUSY";
    }

    // ⭐ ONE call, below every arm, because the loopback verdict depends on which port is OPEN and each
    // of the three arms above leaves that different — including "OUTPUT OFF", which is the arm that
    // turns thru back ON. A rule repeated at each site is a rule one site will forget.
    update_midi_thru();   // the OUT row's half of the ONE verdict (E4) — see apply_midi_in_device
}

// ─── MIDI IN (phase E2) ──────────────────────────────────────────────────────────────────────────

void InputDispatcher::boot_midi_in_port() {
    refresh_midi_in_devices();
    if (s_.midiInDeviceIndex != 0) apply_midi_in_device();
    // ⚠️ UNCONDITIONALLY, and after the OUT port's own boot (app.cpp calls them in that order): with no
    // input device the verdict is still a verdict — thru ON, nothing to loop — and a boot that skipped
    // it would leave the flag at whatever the default happens to be rather than at something decided.
    update_midi_thru();
    // ⚠️ …and drop what `apply` just wrote, exactly as `boot_midi_port` does: boot news belongs on the
    // CONSOLE, and a status line left over from launch would greet the user on the MIDI screen minutes
    // later as though something had just happened.
    s_.midiStatusText.clear();
}

void InputDispatcher::refresh_midi_in_devices() {
    s_.midiInDeviceNames.assign(1, "OFF");   // index 0, always — "no device" is a choice, not a gap

    if (s_.midiIn) {
        const int n = s_.midiIn->device_count();
        for (int i = 0; i < n; ++i) s_.midiInDeviceNames.push_back(s_.midiIn->device_name(i));
    }

    s_.midiInDeviceIndex = resolve_port_index(s_.midiInDeviceNames, s_.settings.midiInDevice);
}

void InputDispatcher::apply_midi_in_device() {
    const int wanted = resolve_port_index(s_.midiInDeviceNames, s_.settings.midiInDevice);
    s_.midiInDeviceIndex = wanted;

    if (!s_.midiIn) { s_.midiStatusText = "NO MIDI BACKEND"; return; }

    // ⚠️ The teardown order, and every step of it answers a way this can go wrong — see the header.
    s_.midiIn->set_sink(nullptr);
    s_.midiIn->close();
    host_.reset_midi_in();

    if (wanted == 0) {
        s_.midiStatusText = "INPUT OFF";
    } else {
        // The sink BEFORE the open: an open port is already delivering, and bytes that arrive between
        // the two would be dropped by a backend that has nowhere to put them — a silent loss at exactly
        // the moment a user is watching to see whether their keyboard works.
        s_.midiIn->set_sink(&host_.midi_in_sink());
        if (s_.midiIn->open(wanted - 1)) {   // −1: index 0 of the list is OFF, not a device
            s_.midiStatusText = "INPUT OPENED";
        } else {
            // The choice is KEPT, exactly as on the OUTPUT side: a port that refuses is usually one
            // another app holds, which is a transient the user can fix. The sink is unwired again so a
            // backend that half-opened cannot deliver into a port the app believes is closed.
            s_.midiIn->set_sink(nullptr);
            s_.midiStatusText = "INPUT BUSY";
        }
    }

    update_midi_thru();   // …and the IN half of the same one verdict (E4) — see apply_midi_device
}

void InputDispatcher::update_midi_thru() {
    // ⚠️ **BOTH PORTS OPEN, OR THERE IS NOTHING TO LOOP.** `is_open()` and not the saved choice: a
    // device that was picked and then refused to open (PORT BUSY) is a port sending nothing, and
    // suppressing thru for it would silence a feature over a cable that does not exist.
    const bool inOpen  = s_.midiIn != nullptr && s_.midiIn->is_open() && s_.midiInDeviceIndex > 0;
    const bool outOpen = port_open() && s_.midiDeviceIndex > 0;

    // The two names the SCREEN is showing. On every backend this project has met, a loopback's two
    // directions carry the SAME display name (winmm's "loopMIDI Port" both ways) — a hardware keyboard
    // and a hardware synth never do, because they are two different devices.
    // ⚠️ Index 0 is "OFF" on both lists, which is why the `> 0` above is part of the predicate and not
    // an optimisation: without it, two closed ports would compare equal and turn thru off.
    const bool loopback =
        inOpen && outOpen &&
        static_cast<size_t>(s_.midiInDeviceIndex) < s_.midiInDeviceNames.size() &&
        static_cast<size_t>(s_.midiDeviceIndex)   < s_.midiDeviceNames.size() &&
        s_.midiInDeviceNames[static_cast<size_t>(s_.midiInDeviceIndex)] ==
            s_.midiDeviceNames[static_cast<size_t>(s_.midiDeviceIndex)];

    host_.set_midi_in_thru(!loopback);

    // ⚠️ SAID OUT LOUD, on the screen the user just used to cause it. A suppression nobody is told
    // about is indistinguishable from an EXTERNAL instrument that does not respond to a keyboard —
    // and it overwrites "INPUT OPENED" deliberately, because it is the more surprising news of the two.
    if (loopback) s_.midiStatusText = "THRU OFF: LOOP";
}

void InputDispatcher::midi_action() {
    switch (static_cast<MidiRow>(s_.midiCursorRow)) {
        case MidiRow::PANIC:
            // Every note-off we owe, on every channel we have used, right now. It goes through the
            // CONSUMER and not the port, because the consumer is what knows which notes are sounding —
            // and it is the same call `SongcoreHost::stop()` makes, so there is one panic in the app.
            host_.midi_out().panic();
            s_.midiStatusText = port_open() ? "PANIC SENT" : "NO PORT";
            break;

        case MidiRow::TEST: {
            // ⚠️ THE ONE THING ON THIS SCREEN THAT WRITES TO THE PORT DIRECTLY, bypassing the bus — and
            // that is the point of it rather than a shortcut taken. TEST answers "is there a cable, and
            // does this machine's MIDI stack work at all?", and an answer routed through the sequencer,
            // the router and the instrument's EXTERNAL flag would be answering a much larger question:
            // a silent TEST would no longer mean "no cable", it would mean "something, somewhere".
            //
            // A note-on and its note-off in the same breath. Sustaining it would make the screen the one
            // place in the app that can leave a note hanging with no transport to stop it.
            if (!port_open()) { s_.midiStatusText = "NO PORT"; break; }
            const uint8_t on[3]  = {0x90, 60, 100};   // C-4, channel 1, mf
            const uint8_t off[3] = {0x80, 60, 0};
            s_.midiOut->send(on, 3);
            s_.midiOut->send(off, 3);
            s_.midiStatusText = "TEST SENT";
            break;
        }

        // OUTPUT / OFFSET / PROG CHG are A+DPAD cells — the app-wide rule that single A is for actions.
        default:
            break;
    }
}

// ─── The plain buttons ───────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_button_a() {
    // Every layer below is ARMED — A means something on each of them, which is why this call swallows
    // only the FX helper. It is still here so that a layer added tomorrow is INERT on A rather than
    // inserting a chain on the screen hidden behind it.
    if (overlay_swallows(Overlay::CONFIRM | Overlay::QWERTY | Overlay::THEME | Overlay::EQ |
                         Overlay::BROWSER)) return;

    // ⚠️ THE CONFIRM DIALOG IS CHECKED FIRST, ahead of the keyboard and the browser both. It is the
    // topmost modal — drawn last, over everything — and a dialog owns the buttons of whatever it is
    // covering: pressing A to answer "CLEAN INST?" must not also fire whatever the cursor is parked on
    // underneath.
    if (confirm_open()) { confirm_accept(); return; }

    // A on the KEYBOARD types the key under the cursor — unless the cursor is on the action row, where
    // the two buttons ARE the answer: ABORT (col 0) and APPLY (col 1).
    if (qwerty_open()) {
        if (s_.qwerty.is_on_action_row()) {
            if (s_.qwerty.keyCursorCol == 0) qwerty_cancel();
            else                             qwerty_apply();
        } else {
            insert_current_key(s_.qwerty);
        }
        return;
    }

    // A on the BROWSER opens a folder, goes up, or loads the file — see browser_confirm.
    if (on_browser()) { browser_confirm(); return; }

    // ⚠️ A on the SAMPLE EDITOR's confirm dialog is YES: discard the unsaved edits and leave. It is
    // checked FIRST, because a dialog owns the buttons of the screen it is covering — pressing A to
    // answer "ARE YOU SURE?" must not also fire whatever op the cursor happens to be parked on.
    if (on_sample_editor() && s_.sampleEditor.showConfirmClose) {
        s_.sampleEditor.showConfirmClose = false;
        close_sample_editor();
        return;
    }

    // ⚠️ A in the THEME EDITOR is meaningful on exactly ONE row and TWO of its three columns — the THEME
    // row's SAVE and LOAD. Everywhere else (the name itself, and all seventeen colour rows) it does
    // NOTHING, because a colour channel is dialled with A+DPAD and has nothing for a bare A to confirm.
    //
    // Like the EQ arm below it, this must RETURN rather than fall through: `currentScreen` is still
    // SETTINGS underneath, and SETTINGS' own A is `settings_action()` — whose THEME row (9) is the very
    // cell the cursor is parked on. Fall through and A would RE-OPEN the editor that is already open,
    // resetting the cursor to row 0 under the user's thumb.
    if (theme_open()) {
        if (s_.themeEditor.cursorRow == 0) theme_row_action();
        return;
    }

    // ⚠️ A PLAIN A DOES NOTHING IN THE EQ EDITOR, and that is Kotlin (`AppInputDispatcher:1300`), not an
    // omission. The editor's vocabulary is A+DPAD (dial the band value), A+B (reset it), B+DPAD (change
    // slot), and B or SELECT (close). There is no cell here for a bare A to insert into or confirm.
    //
    // It must RETURN rather than fall through, and that is the load-bearing half: `currentScreen` is
    // still the screen underneath, so without this line an A in the editor would fire a sample-editor op
    // or insert a chain on a screen the user cannot even see.
    if (eq_open()) return;

    // A on a cell that OPENS a sub-screen — the two NAME rows and all five EQ cells. Runs BEFORE the
    // per-screen arms below, exactly as Kotlin's `openSubScreenAtCursor(peek = false)` does, because
    // those cells have nothing to insert and the sample editor's EQ cell would otherwise run its FX
    // APPLY instead of opening the editor.
    if (open_sub_screen_at_cursor(/*peek=*/false)) return;

    if (on_sample_editor()) { sample_editor_confirm(); return; }

    // A on INSTRUMENT's LOAD / SAVE / EDIT buttons and the pool's empty NAME slot. NOT deferred to
    // release (they are read-only cells with no A+DPAD to protect), which is why they are not part of
    // `open_sub_screen_at_cursor` — Kotlin splits them the same way (`handleConfirmAInstrument`).
    if (instrument_open_at_cursor()) return;

    // A on an EMPTY cell inserts the item you last edited. That is what makes A,A meaningful: press
    // A once to lay down the last chain again, press it twice to get a fresh one.
    Project& p = host_.edit_project();
    hasInsertPos_ = false;

    switch (s_.currentScreen) {
        case ScreenType::PHRASE: {
            if (s_.cursorColumn != 1 || s_.selection.active) return;
            Phrase& ph = p.phrases[static_cast<size_t>(s_.currentPhrase)];
            PhraseEditorState ps{ph};
            ps.cursorRow    = s_.cursorRow;
            ps.cursorColumn = s_.cursorColumn;
            if (!phrase_.cursor_context(ps).capabilities.isEmpty) return;

            songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];
            step.note       = s_.lastEditedNote;
            step.instrument = s_.lastEditedInstrument;
            step.volume     = s_.lastEditedVolume;
            mark_modified();

            // Arm A,A (D1): a second press on this same note cell keeps the note the first A just laid
            // down and re-points it at the next FREE instrument — mirroring chain/song A,A, which
            // advance the chain/phrase ref. Only the NOTE column (col 1) arms.
            hasInsertPos_ = true;
            insertScreen_ = ScreenType::PHRASE;
            insertRow_    = s_.cursorRow;
            insertCol_    = s_.cursorColumn;

            if (s_.settings.notePreviewEnabled && step.note != Note::EMPTY()) {
                // A WHOLE PHRASE long, not one step: this gesture lays a note down to listen to, and
                // an audition that dies after a 16th note tells you nothing about a pad.
                const int sr = std::max(44100, host_.sample_rate());
                host_.set_preview_track(audition_track());
                host_.preview_note(std::min(std::max(step.instrument, 0), 127), step.note,
                                   songcore::frames_per_step(p.tempo, sr) * 16);
            }
            break;
        }

        case ScreenType::CHAIN: {
            Chain& chain = p.chains[static_cast<size_t>(s_.currentChain)];
            if (chain_row_empty(chain, s_.cursorRow)) {
                chain.phraseRefs[static_cast<size_t>(s_.cursorRow)]      = s_.lastEditedPhrase;
                chain.transposeValues[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedTranspose;
                mark_modified();
                hasInsertPos_ = true;   // arm A,A — a second press here inserts the next UNUSED phrase
                insertScreen_ = ScreenType::CHAIN;
                insertRow_    = s_.cursorRow;
                insertCol_    = s_.cursorColumn;
            }
            break;
        }

        case ScreenType::SONG: {
            if (s_.selection.active) return;
            if (s_.cursorColumn < 1 || s_.cursorColumn > 8) return;
            songcore::Track& track = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)];
            while (static_cast<int>(track.chainRefs.size()) <= s_.cursorRow)
                track.chainRefs.push_back(-1);

            if (track.chainRefs[static_cast<size_t>(s_.cursorRow)] == -1) {
                track.chainRefs[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedChain;
                mark_modified();
                hasInsertPos_ = true;
                insertScreen_ = ScreenType::SONG;
                insertRow_    = s_.cursorRow;
                insertCol_    = s_.cursorColumn;
            }
            break;
        }

        // The two screens whose rows are BUTTONS. Nothing to insert — A *is* the action.
        case ScreenType::PROJECT:  project_action();  break;
        case ScreenType::SETTINGS: settings_action(); break;
        case ScreenType::MIDI:     midi_action();     break;

        default:
            break;
    }
}

void InputDispatcher::on_button_b() {
    // Every layer below is ARMED — B closes or answers on each of them. Here for the same reason as
    // on_button_a's: a layer added tomorrow is inert on B rather than closing the browser behind it.
    if (overlay_swallows(Overlay::CONFIRM | Overlay::QWERTY | Overlay::THEME | Overlay::EQ |
                         Overlay::BROWSER)) return;

    // B is the NO of "A=YES  B=NO", and it is checked first for the same reason A's accept is: the
    // dialog owns the buttons of the screen underneath it.
    if (confirm_open()) { confirm_cancel(); return; }

    if (qwerty_open()) { delete_char(s_.qwerty); return; }

    // ⚠️ B CLOSES THE THEME EDITOR, and there is no "are you sure" — the live theme IS the applied theme
    // (every module already draws from it; there is no apply step), so closing loses nothing that a save
    // was needed to keep. The palette survives the close, the app and — since S9 — the QUIT.
    if (theme_open()) { close_theme_editor(); return; }

    // ⚠️ B CLOSES THE EQ EDITOR — but the MAPPER holds this press until B is RELEASED
    // (`defer_b_to_release`), and cancels it outright if a B+DPAD fires in between. Without that latch
    // the slot cycle would be unreachable: B+LEFT would close the editor on B's own press and the LEFT
    // would land on the mixer behind it.
    if (eq_open()) { close_eq_editor(); return; }

    if (on_browser()) {
        FileBrowserState& fb = s_.fileBrowser;

        // B is the NO of "A=YES B=NO" — it disarms whatever is armed rather than leaving the browser,
        // which is what makes SELECT+B and SELECT+A safe to press by accident. Written against the
        // MODE and not against DELETE, so a third one cannot be added and left with no way out.
        if (fb.mode != BrowserMode::NORMAL) { fb.mode = BrowserMode::NORMAL; return; }

        // Inside a file selection, B COPIES it — the same gesture as B over a grid selection below.
        if (fb.selectionMode) {
            std::vector<std::string> files = browser_selected_paths();
            if (!files.empty()) {
                const size_t n = files.size();
                fb.fileClipboard      = std::move(files);
                fb.fileClipboardIsCut = false;
                fb.statusMessage = "CPY " + std::to_string(n) + (n == 1 ? " FILE" : " FILES");
                fb.statusSuccess = true;
            }
            fb.selectionMode   = false;
            fb.selectionAnchor = -1;
            return;
        }

        close_file_browser();
        return;
    }

    // ⚠️ B LEAVES SETTINGS — the port had no such arm until Phase 4, so the screen could only be left by
    // R+DPAD, which is not a way out any other full-screen destination makes you use.
    //
    // Its POSITION is the specification, and it is Kotlin's (AppInputDispatcher.kt:2057):
    //   • AFTER the modals. The THEME EDITOR is raised FROM this screen (SETTINGS' own A, row 9), and the
    //     EQ editor and the keyboard can be over it too — while one is up it owns B, or closing SETTINGS
    //     would yank the screen out from under it.
    //   • BEFORE the selection arm below. B inside a selection COPIES, and `return`s. Put this after it
    //     and B on SETTINGS with a live selection copies nothing (SETTINGS has no clipboard arm) and never
    //     reaches here — the screen would be stuck exactly as it was, only intermittently. Kotlin's own
    //     `exitSelectionMode()` on this path is the tell that the two CAN overlap.
    if (s_.currentScreen == ScreenType::SETTINGS) {
        s_.selection.exit();   // Kotlin: trackerController.inputController.exitSelectionMode()
        NavResult nav;
        nav.screen = s_.settingsReturnScreen;
        nav.column = s_.previousColumn;   // SETTINGS owns no column — the way out keeps the one it came in with
        go_to_screen(s_, nav);            // …and NOT a bare assignment: the port's cursors are saved/restored here
        return;
    }

    // MIDI leaves the same way and for the same reasons — and B is its ONLY way out, since it is not on
    // the R+DPAD grid. Placed beside SETTINGS so the two stay in the same position relative to the
    // modals above and the selection arm below; the argument in the comment on that block is verbatim
    // this one's.
    if (s_.currentScreen == ScreenType::MIDI) {
        s_.selection.exit();
        NavResult nav;
        nav.screen = s_.midiReturnScreen;
        nav.column = s_.previousColumn;
        go_to_screen(s_, nav);
        return;
    }

    // ⚠️ EFFECTS' TIME row: B toggles DELAY SYNC — free-running milliseconds ↔ note divisions. It has to
    // be a gesture of its OWN, because the cell's VALUE means two different things on either side of it
    // (0x40 is a delay length; 4 is a 1/16 note) and no amount of A+DPAD can express "change which of
    // those you mean".
    //
    // ⚠️ B IS COMPLETELY FREE ON THIS SCREEN, which is what makes it the right home: EFFECTS is in no arm
    // of `cycle_current_item`, `on_b_up`/`on_b_down` act only on SONG and INST_POOL, and `on_l_b` will
    // not start a selection here — so there is no B+DPAD to protect and the press needs no release latch.
    //
    // Re-clamping delayTime into 0..B on the way IN is not optional: a free time of 0xF0 is not a
    // subdivision, and an unclamped one would index past the end of the name list.
    if (s_.currentScreen == ScreenType::EFFECTS &&
        s_.effectsCursorRow == EffectModule::ROW_DLY_TIME) {
        songcore::Project& p = host_.edit_project();
        p.delaySync = !p.delaySync;
        if (p.delaySync) p.delayTime = std::min(std::max(p.delayTime, 0), 11);
        mark_modified();
        return;
    }

    // ⚠️ B on the SAMPLE EDITOR is BACK — but it asks first if there is anything to lose. The editor's
    // edits live in the ENGINE's buffer, not in the project, so leaving without saving is the one
    // gesture in the app that can silently destroy work. Three states, in order: the dialog is up (B is
    // NO — stay), the sample is modified (arm the dialog), or it is clean (just go).
    if (on_sample_editor()) {
        SampleEditorState& se = s_.sampleEditor;
        if (se.showConfirmClose)  { se.showConfirmClose = false; return; }
        if (se.isModified)        { se.showConfirmClose = true;  return; }
        close_sample_editor();
        return;
    }

    // B inside a selection COPIES it and exits — the tracker's copy gesture. Outside one, B on these
    // five screens does nothing (they are the main row; there is nowhere to go back to).
    if (!s_.selection.active) return;

    const SelectionBounds b = s_.selection.bounds();
    const Project&        p = *s_.project;

    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
            clip_.copy_phrase_steps(p, s_.currentPhrase, b.topLeftRow, b.topLeftColumn,
                                    b.bottomRightRow, b.bottomRightColumn);
            break;
        case ScreenType::CHAIN:
            clip_.copy_chain_rows(p, s_.currentChain, b.topLeftRow, b.topLeftColumn,
                                  b.bottomRightRow, b.bottomRightColumn);
            break;
        case ScreenType::SONG:
            clip_.copy_song_cells(p, b.topLeftRow, b.topLeftColumn, b.bottomRightRow,
                                  b.bottomRightColumn);
            break;
        case ScreenType::TABLE:
            clip_.copy_table_rows(p, s_.currentTable, b.topLeftRow, b.topLeftColumn,
                                  b.bottomRightRow, b.bottomRightColumn);
            break;
        default:
            break;
    }
    s_.selection.exit();
}

void InputDispatcher::on_select() {
    // ⚠️ BARE SELECT IS HELP, AND THE KEYBOARD'S ABORT. That is the whole list, and the emptiness
    // everywhere else was kept for years to make this possible: every action a cell could want from
    // SELECT is already on the A or the B sitting on that same cell, so nothing had to be taken back
    // off users when help landed.
    //
    // ⚠️ **IT ARRIVES ON THE RELEASE, NOT THE PRESS** (ui/button_mapper.h), and that is what keeps the
    // three browser chords whole: SELECT is a MODIFIER there, so its press only arms SELECT+A/B/R and
    // any other button going down during it cancels this handler outright.
    //
    // ⚠️ NOT TO BE CONFUSED WITH THE A-DEFERRAL. `defer_a_to_release` holds A on the cells that open a
    // sub-screen so that a held A+DPAD can still dial the value underneath. That mechanism is required,
    // it is what makes those cells editable at all, and it has nothing to do with this handler.
    if (overlay_swallows(Overlay::QWERTY)) return;

    // The keyboard's ABORT — the chord alias for the button on its own action row, and the one bare
    // SELECT that duplicates nothing: B backspaces here, so without it the only way to abandon a rename
    // is to walk the cursor onto ABORT and press A.
    if (qwerty_open()) { qwerty_cancel(); return; }

    // ── HELP ─────────────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ Gated on there BEING a strip to draw it in. The compact panel takes the oscilloscope's 620×70
    // and nothing else, and the two full-screen modules have neither — a toggle there would set a flag
    // no frame reads, and the next screen you walked onto would come up holding help you never asked
    // for. (`full_screen_module` is in ui/app_state.h, shared with the two questions layout.cpp asks.)
    if (full_screen_module(s_)) return;
    s_.helpOpen = !s_.helpOpen;
}

void InputDispatcher::on_help_dismiss() {
    // ⚠️ **THE PRESS IS NOT CONSUMED — it closes help and then does its normal job**, which is the
    // whole reason help is not an `Overlay`. It covers the strip and never the editor, so there is
    // nothing under it to protect from a stray press: closing it and swallowing the press would cost a
    // button on every gesture and buy nothing.
    s_.helpOpen = false;
}

void InputDispatcher::on_stop_preview() {
    // ⚠️ THE ONE HANDLER A CONFIRM DOES NOT OWN, and it is ARMED here to say so: a dialog raised over
    // an INSTRUMENT audition must not leave the note hanging, and silencing a note is not an edit. The
    // FX helper and the browser are armed for the same reason — the screen behind them is the one that
    // started the preview, and `previewScreen` below is what decides whether there is one to stop.
    if (overlay_swallows(Overlay::CONFIRM | Overlay::EQ | Overlay::FX_HELPER |
                         Overlay::BROWSER)) return;

    // Only the screens that can START an audition can stop one — `stopActivePreview()`. On PHRASE it
    // is gated on the setting, because with previews off there is nothing to silence. The three
    // instrument screens always can: their START *is* an audition, and it rings out until stopped, so
    // "press any button to silence it" is the only way to end it.
    //
    // ⚠️ The BROWSER is on this list too, and it has to be: its START auditions the file under the
    // cursor and the sample rings out (no timed kill). Moving the cursor to the next file must silence
    // the last one, or scrolling a folder of kicks stacks them on top of each other.
    //
    // ⚠️ And so is the EQ EDITOR — but ONLY when it was opened over an INSTRUMENT. That is the case
    // where a preview can be ringing underneath it (the editor lets START through precisely so that it
    // can be), and its band edits sweep that held note live. Opened over the MIXER or EFFECTS there is
    // no audition to silence, and claiming otherwise would stop a preview nobody started.
    const bool eqOverInstrument =
        eq_open() && s_.eq.caller.kind == EqCallerContext::Kind::INSTRUMENT;

    const bool previewScreen = (s_.currentScreen == ScreenType::TABLE) || on_browser() ||
                               on_instrument_screen() || eqOverInstrument ||
                               (s_.currentScreen == ScreenType::PHRASE && s_.settings.notePreviewEnabled);
    if (previewScreen) host_.stop_preview();
}

void InputDispatcher::on_start() {
    // ⚠️ THE THEME AND EQ EDITORS ARE ARMED HERE TO LET START THROUGH, not to serve it: both partial
    // overlays keep the transport of the screen underneath, which is the only way to HEAR what an edit
    // is doing while you dial it (ui/app_state.h).
    if (overlay_swallows(Overlay::QWERTY | Overlay::THEME | Overlay::EQ | Overlay::BROWSER)) return;
    // START is the keyboard's APPLY — the chord alias for the button on its action row.
    if (qwerty_open()) { qwerty_apply(); return; }

    // ⚠️ START on the BROWSER is not the transport either: it AUDITIONS the file under the cursor, on
    // the preview lane, decoded straight into slot 255. This is what a sample browser is FOR — hearing
    // a file before committing a slot to it — and it is the one note in the port that does not go
    // through `plan_note_on`, because a file being auditioned has no instrument to derive from
    // (songcore::preview_sample_file).
    //
    // ⚠️ A SECOND DELIBERATE DIVERGENCE FROM ANDROID, stated rather than transcribed (parity audit,
    // finding 8). Kotlin gates ONLY the `.wav` arm on where the browser was opened from
    // (`previousScreen ∈ {INSTRUMENT, INST_POOL}`, AppInputDispatcher.kt:2364) — while the very next
    // arms preview mp3/flac/ogg/opus from ANY browser. So on Android the project-LOAD browser plays
    // an .mp3 and sits silent on the .wav beside it, an asymmetry no user could predict. The port
    // previews every audible extension from every browser context: the coherent superset, kept on
    // purpose rather than ported bug-for-bug.
    if (on_browser()) {
        const BrowserItem* item = s_.fileBrowser.current();
        if (!item || item->kind != BrowserItem::Kind::FILE) return;

        const std::string ext = to_lower(item->extension);
        const bool audible = std::find(sample_extensions().begin(), sample_extensions().end(), ext) !=
                             sample_extensions().end();
        if (!audible) return;   // a .pti or an .sf2 has no waveform to play

        // A file on disk has no song cell behind it — neutral gain, and never the channel an earlier
        // audition left pointed at.
        host_.set_preview_track(-1);
        if (!host_.preview_file(item->path)) {
            s_.fileBrowser.statusMessage = "PREVIEW FAILED";
            s_.fileBrowser.statusSuccess = false;
        }
        return;
    }

    // ⚠️ START on the SAMPLE EDITOR is an audition too — but it is the only one in the app that TOGGLES.
    //
    // Everywhere else a second START retriggers. Here it STOPS, because the editor is the one screen you
    // audition a four-minute loop from, and a preview with no timed kill and no way to stop it is a
    // sample you have to leave the screen to silence. (The "press any button to silence a preview" rule
    // that covers the other screens deliberately exempts this one — you are pressing buttons constantly
    // in here, and every one of them would cut the sample you are trying to listen to.)
    //
    // The toggle only engages while the TRANSPORT IS STOPPED: `playbackPosition` also tracks song voices
    // playing the same sample, so during playback START keeps its retrigger meaning rather than reading a
    // song voice as "the preview is running".
    if (on_sample_editor()) {
        if (s_.sampleEditor.showConfirmClose) return;

        if (s_.sampleEditor.playbackPosition >= 0.0f && !host_.is_playing()) {
            host_.stop_preview();
            s_.sampleEditor.playbackPosition = -1.0f;
            return;
        }

        // ⚠️ The rapid double-START guard. A pending restore from the PREVIOUS preview must land before
        // this one arms anything, or it would land in the middle of this audition and take its EQ,
        // sends and modulation back off — the dry preview undone a fifth of a second after it started.
        run_due_sample_preview_restore(/*force=*/true);

        SampleEditorState& se = s_.sampleEditor;

        previewRestoreInst_    = se.instrumentId;
        previewRestorePending_ = true;
        previewRestoreAtMs_    = now_ms_ + 100;   // Kotlin's `delay(100)`

        // The FX row is auditioned by APPLYING it for real and putting the clean audio back afterwards —
        // there is no dry/wet path through a destructive DSP chain. The backup is what makes that safe.
        // (EQ has no amount, so it always previews; the other three need a nonzero one.)
        const bool hasFxPreview = (se.fxType == SampleEditorModule::FX_EQ) ||
                                  (se.fxType <= SampleEditorModule::FX_DRIVE && se.fxValue > 0);
        host_.restore_fx_preview_backup();
        if (hasFxPreview) {
            host_.save_fx_preview_backup(se.instrumentId);
            host_.apply_sample_fx(se.instrumentId, se.fxType, se.fxValue);
        }

        host_.set_preview_track(-1);   // a waveform being edited is not in the arrangement either
        host_.preview_sample_editor(se.instrumentId, se.sourceMode, se.selectionStart, se.selectionEnd,
                                    se.totalFrames, se.pitchSemitones);
        return;
    }

    // ⚠️ **START IS NOT ALWAYS THE TRANSPORT.** On the four screens that edit a SOUND rather than an
    // arrangement — INSTRUMENT, INST.POOL, MODS and TABLE — it AUDITIONS the instrument at its own
    // root, on the preview lane, and the note rings until the next plain button press silences it. That
    // is the whole point of sitting on one of them: you are dialling a sound in and listening to it.
    //
    // It does not consult `is_playing()`, and that is deliberate: auditioning an instrument OVER a
    // running song is exactly what you want while you fit it into the mix, and the preview lane is a
    // ninth voice — it steals nothing from the eight the song is using.
    //
    // ⚠️ TABLE auditions **through the table it is showing** (`previewInstrumentWithTable(currentTable,
    // currentTable)` — the instrument id and the table id are the same number, because instrument N
    // owns table N). Without the override you would hear the instrument's own table instead of the
    // automation on the screen in front of you, which is the one thing the audition exists to check.
    //
    // ⚠️ …AND IT IS NO LONGER AT UNITY GAIN. The lane borrows the fader of the song cell you came
    // through, so a pad you can only hear through its sends auditions where it actually sits. It is
    // still a ninth voice, and still steals nothing.
    if (on_instrument_screen()) {
        host_.set_preview_track(audition_track());
        host_.preview_instrument(s_.currentInstrument);
        return;
    }
    if (s_.currentScreen == ScreenType::TABLE) {
        host_.set_preview_track(audition_track());
        host_.preview_instrument(s_.currentTable, /*tableIdOverride=*/s_.currentTable);
        return;
    }

    // ⚠️ **IN LIVE MODE, START ON SONG IS NOT THE TRANSPORT AT ALL — IT QUEUES.** It sits ahead of the
    // play/stop toggle rather than inside it because that is the whole difference between the two
    // modes: here the button launches the cell under the cursor, and stopping is R+START for one
    // channel or the STOP button for everything.
    if (live_song_gesture()) {
        const int track = s_.cursorColumn - 1;   // on SONG the cursor column IS the track, 1-based
        if (!host_.is_playing()) {
            // Nothing is running, so there is no boundary to wait for: this channel starts NOW and
            // the transport starts with it. The other seven begin silent, waiting to be launched.
            host_.play_song_live(s_.cursorRow, 1 << track);
            return;
        }
        // ⭐ LGPT's two-press launch, and it needs no second chord and no state of its own: the first
        // press queues for the end of the playing chain, and a second on the SAME cell promotes that
        // queue to the next phrase boundary. The slot already says which press this is.
        //
        // ⚠️ **ONLY WHILE IT IS STILL ARMED.** A launch the sequencer has already committed to a frame
        // goes on showing as queued until it is heard, which is right for the marker and wrong here:
        // promoting it then pulls it earlier and cuts short a chain the player is still listening to.
        const songcore::LiveSlot q = host_.live_queue(track);
        const bool               immediate = q.armed() && !q.stop && q.targetRow == s_.cursorRow;
        host_.queue_live(track, s_.cursorRow, immediate);
        return;
    }

    // Everywhere else START is the transport.
    if (host_.is_playing()) {
        host_.stop();
        return;
    }
    switch (s_.currentScreen) {
        // ⚠️ SONG starts at the CURSOR ROW, not at row 0 — "play from here" is the gesture, and on a
        // 200-row arrangement starting from the top every time makes the screen unusable.
        case ScreenType::SONG:  host_.play_song(s_.cursorRow); break;

        // ⚠️ …AND CHAIN PLAYS ON THE TRACK IT BELONGS TO, not on channel 1. A track is a fader, a
        // mute, a peak meter, a voice slot and every per-track FX the phrase carries — a chain
        // auditioned on track 0 is heard at another channel's level, with a VTR inside it moving
        // another channel's fader. The remembered song cell only breaks a tie between the tracks that
        // already hold this chain; the arrangement is what answers.
        case ScreenType::CHAIN:
            host_.play_chain(s_.currentChain,
                             songcore::track_of_chain(*s_.project, s_.currentChain,
                                                      remembered_song_track()));
            break;

        // ⚠️ …and the four screens with NO song cursor play the SONG, from the top. This is a bug fixed
        // in S5, not a new behaviour: the S3 comment already said "MIXER, EFFECTS, PROJECT, SETTINGS
        // start at 0" — while the code below it dropped them into the `default` and played the current
        // PHRASE. It went unnoticed because all four were placeholder screens (you could stand on one
        // and press START, and something plausible happened). What you want on the mixer is the MIX.
        case ScreenType::MIXER:
        case ScreenType::EFFECTS:
        case ScreenType::PROJECT:
        case ScreenType::SETTINGS:
        // ⭐ MIDI JOINS THE FOUR, AND IT IS THE ONE THAT MOST NEEDS TO. This screen is where the user
        // picks the cable and sets the OFFSET, and both are dialled BY EAR against a song that is
        // playing — a MIDI screen you had to leave to start the transport would make its own OFFSET row
        // untunable.
        case ScreenType::MIDI: host_.play_song(0); break;

        // PHRASE, GROOVE, SCALE… — Kotlin's `togglePlayback()` else-arm. The phrase is asked through
        // the chain on screen first: the same phrase may sit in five chains, and the one you are
        // inside is the only one with a gesture behind it.
        default:
            host_.play_phrase(s_.currentPhrase,
                              songcore::track_of_phrase(*s_.project, s_.currentPhrase, s_.currentChain,
                                                        remembered_song_track()));
            break;
    }
}

// ─── LIVE mode's two chords ──────────────────────────────────────────────────────────────────────
//
// ⚠️ **BOTH ARE RESERVED CHORDS TAKING ON A MEANING, NOT NEW ONES.** `button_mapper.h` has consumed
// L+START and R+START since the matrix was written — *"reserved — START must not toggle playback
// here"* — so on every screen but SONG, and in every mode but LIVE, they still do exactly nothing.
// The gate is what keeps that true: the mapper's two arms are GLOBAL, so without it L+START would
// queue a song row from inside the sample editor.

int InputDispatcher::live_row_mask(int songRow) const {
    int mask = 0;
    for (int t = 0; t < 8; ++t) {
        // chainRefs is a GROWING list, so a row past a track's end is empty rather than out of bounds.
        const std::vector<int>& refs = s_.project->tracks[static_cast<size_t>(t)].chainRefs;
        const int chainId = (songRow >= 0 && songRow < static_cast<int>(refs.size()))
                                ? refs[static_cast<size_t>(songRow)] : -1;
        if (chainId >= 0 && chainId < 256) mask |= 1 << t;
    }
    return mask;
}

bool InputDispatcher::live_row_armed(int songRow) const {
    for (int t = 0; t < 8; ++t) {
        const songcore::LiveSlot q = host_.live_queue(t);
        if (!q.stop && q.targetRow == songRow) return true;
    }
    return false;
}

void InputDispatcher::on_l_start() {
    if (!live_song_gesture()) return;
    if (!host_.is_playing()) {
        // From a standing start the whole row launches together, on one downbeat. A cell with no
        // chain in it starts silent — the row sounds the way it looks.
        host_.play_song_live(s_.cursorRow, live_row_mask(s_.cursorRow));
        return;
    }
    host_.queue_live_row(s_.cursorRow, live_row_armed(s_.cursorRow));
}

void InputDispatcher::on_r_start() {
    if (!live_song_gesture()) return;
    if (!host_.is_playing()) return;   // nothing is sounding, so there is nothing to queue a stop for
    const int track = s_.cursorColumn - 1;
    // The same two-press promotion the launch has: queued for the chain end, pressed again for the
    // next phrase boundary.
    const songcore::LiveSlot sq = host_.live_queue(track);
    host_.queue_live_stop(track, sq.armed() && sq.stop);
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
// THE FILE BROWSER AND THE QWERTY KEYBOARD (Phase 3 S6a)
// ═════════════════════════════════════════════════════════════════════════════════════════════════

// ─── Opening and closing the browser ─────────────────────────────────────────────────────────────

std::string InputDispatcher::browser_dir(BrowserDir cat) {
    // A config.json override per category, else the built-in default. The FileSystem getters
    // create-on-first-use, so `def` is always a real, readable directory — which is what makes it a
    // safe answer whenever the override cannot be honoured.
    //
    // No debug gate: config.json ships on every platform's release from v0.9.4 (`ui/folder_config.h`).
    const std::optional<std::string>* ov = nullptr;
    std::string                       def;
    switch (cat) {
        case BrowserDir::SAMPLES:     ov = &s_.folderConfig.samples;     def = fs_.samples_directory();     break;
        case BrowserDir::SOUNDFONTS:  ov = &s_.folderConfig.soundfonts;  def = fs_.soundfonts_directory();  break;
        case BrowserDir::INSTRUMENTS: ov = &s_.folderConfig.instruments; def = fs_.instruments_directory(); break;
        case BrowserDir::PROJECTS:    ov = &s_.folderConfig.projects;    def = fs_.projects_directory();    break;
        case BrowserDir::THEMES:      ov = &s_.folderConfig.themes;      def = fs_.themes_directory();      break;
    }
    // ⚠️ The whole rule lives in `resolve_browse_dir`, NOT here: the value is root-relative unless it is
    // absolute, an override authored under another install's root is re-rooted onto ours, and one that
    // cannot be read on this platform falls back to `def`. Inlining any part of that here is how the
    // config and a project's sample paths would start disagreeing about where the app's folders are.
    return ov ? resolve_browse_dir(fs_, *ov, def) : def;
}

void InputDispatcher::open_file_browser(AppState::BrowserPurpose purpose, const std::string& directory,
                                        const std::vector<std::string>& extensions) {
    s_.previousScreen = s_.currentScreen;
    s_.browserPurpose = purpose;

    s_.fileBrowser.fileExtensions = extensions;
    s_.fileBrowser.mode           = BrowserMode::NORMAL;

    // D2a: for a SAMPLE load with FOLDER = REMEMBER, start at the folder the last sample came from.
    // Keyed off the requested start being the samples dir — which is EXACTLY the two sample-load
    // purposes (LOAD_SOURCE on a sampler, LOAD_SAMPLE_EDITOR) and nothing else, so soundfonts, presets,
    // projects and themes keep their own directories. Falls back to `directory` if the remembered path
    // is empty or gone (a deleted folder must not strand the browser on an empty listing).
    std::string start = directory;
    if (s_.settings.rememberFolder && directory == browser_dir(BrowserDir::SAMPLES) &&
        !s_.settings.lastSampleFolder.empty() && fs_.is_directory(s_.settings.lastSampleFolder)) {
        start = s_.settings.lastSampleFolder;
    }
    navigate_to_folder(s_.fileBrowser, fs_, start);

    s_.currentScreen = ScreenType::FILE_BROWSER;
}

void InputDispatcher::close_file_browser() {
    // The audition the user was scrolling through is over. Kotlin frees slot 255 here for the same
    // reason: a preview left resident is a megabyte of PCM nothing will ever play again.
    host_.clear_previews();
    s_.fileBrowser.selectionMode   = false;
    s_.fileBrowser.selectionAnchor = -1;
    s_.currentScreen = s_.previousScreen;
}

void InputDispatcher::refresh_browser() {
    FileBrowserState& b = s_.fileBrowser;

    // ⚠️ **A RE-LIST IS NOT A RE-READ UNLESS THE FILESYSTEM IS TOLD TO FORGET.** `SafFileSystem`
    // serves a cached listing and drops that cache only on this app's own writes, so without this
    // line a refresh hands back exactly what is already on screen — and a file that arrived from a
    // PC, a download or a file manager stays invisible until the next launch.
    //
    // Here rather than at the five call sites, so that adding a sixth cannot forget it. A no-op on
    // every implementation that caches nothing, which is all of them but Android's.
    fs_.forget_listing(b.currentDirectory);

    // Re-list in place and KEEP THE CURSOR where it was — a refresh is not a navigation. Deleting the
    // twentieth file in a folder should leave you on the twentieth row, not throw you back to the
    // first. It has to be CLAMPED, though: the list may have got shorter, and a cursor past the end of
    // it draws nothing highlighted — the row the user was on simply vanishes.
    rebuild_items(b, fs_);

    const int last = static_cast<int>(b.items.size()) - 1;
    b.cursor = std::min(std::max(b.cursor, 0), std::max(last, 0));

    if (b.cursor < b.scroll) b.scroll = b.cursor;
    if (b.cursor >= b.scroll + BROWSER_VISIBLE_ROWS) b.scroll = b.cursor - BROWSER_VISIBLE_ROWS + 1;
    b.scroll = std::max(0, std::min(b.scroll, std::max(0, last - BROWSER_VISIBLE_ROWS + 1)));
}

void InputDispatcher::refresh_browser_on_foreground() {
    // ⚠️ Guarded on the SCREEN, not on the browser's state: `fileBrowser` keeps its directory and its
    // cursor after `close_file_browser`, so re-listing unconditionally would re-read a directory
    // nobody is looking at on every Home-and-back — and on Android that listing is a query per entry
    // over a content provider.
    if (s_.currentScreen != ScreenType::FILE_BROWSER) return;

    // ⚠️ NOT while a modal is up. The DELETE confirm names the row under the cursor, and a refresh can
    // move what is under the cursor — so a listing rebuilt behind the prompt would leave "DELETE X?"
    // on screen with the cursor now on Y, and A would delete Y. The browser is re-read on the next
    // gesture instead; a stale listing is a cosmetic problem, a mislabelled confirm is not.
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    refresh_browser();
}

// ─── The browser's cursor ────────────────────────────────────────────────────────────────────────

int InputDispatcher::browser_repeat_factor(int delta) {
    const long long dt = now_ms_ - lastBrowserMoveMs_;
    lastBrowserMoveMs_ = now_ms_;

    // > 250 ms apart is a fresh press or a deliberate tap, not a hold: the shell's own initial-delay
    // gap is 400 ms and its repeat cadence 100 ms, so the threshold sits between them with room on
    // both sides. A different direction is a fresh gesture too, however fast it arrives.
    if (dt > 250 || delta != lastBrowserMoveDelta_) {
        lastBrowserMoveDelta_ = delta;
        browserHoldStartMs_   = now_ms_;
        return 1;
    }
    const long long held = now_ms_ - browserHoldStartMs_;
    if (held >= BROWSER_ACCEL_X4_MS) return 4;
    if (held >= BROWSER_ACCEL_X2_MS) return 2;
    return 1;
}

void InputDispatcher::browser_move_cursor(int delta, bool page) {
    FileBrowserState& b     = s_.fileBrowser;
    const int         total = static_cast<int>(b.items.size());
    if (total == 0) return;

    // Scaled here rather than at the four D-pad call sites, so a row step and a page jump accelerate
    // by the one rule and neither can be forgotten. It is a no-op for anything that is not a held
    // repeat, and `delta` carries the direction the factor keys off.
    delta *= browser_repeat_factor(delta);

    // ⚠️ UP/DOWN WRAP; the LEFT/RIGHT page jump CLAMPS. That asymmetry is Kotlin's and it is the right
    // one: wrapping a single step off the end of a list is a convenience, but a PAGE that wrapped would
    // fling you from the top of a 400-file directory to the bottom on one tap.
    if (page) {
        b.cursor = std::min(std::max(b.cursor + delta, 0), total - 1);
    } else {
        // ⚠️ Modulo TWICE, not `+ total` once: `delta` is no longer bounded to ±1 now that a hold
        // scales it, and one addition of `total` only covers a step smaller than the whole list.
        b.cursor = ((b.cursor + delta) % total + total) % total;
    }

    // Keep the 19-row window around the cursor.
    if (b.cursor < b.scroll) {
        b.scroll = b.cursor;
    } else if (b.cursor >= b.scroll + BROWSER_VISIBLE_ROWS) {
        b.scroll = b.cursor - BROWSER_VISIBLE_ROWS + 1;
    }
}

// ─── A: open a folder, or LOAD the file ──────────────────────────────────────────────────────────

void InputDispatcher::browser_confirm() {
    FileBrowserState& b = s_.fileBrowser;

    // DELETE mode: A is the YES of "A=YES B=NO". This is the ONLY place the browser removes anything,
    // and it is two presses away from any accident — SELECT+B to arm, A to confirm.
    if (b.mode == BrowserMode::DELETE) {
        const BrowserItem* item = b.current();
        b.mode = BrowserMode::NORMAL;
        if (!item || item->is_pseudo()) return;

        const std::string name = item->displayName;
        if (fs_.delete_path(item->path)) {
            refresh_browser();
            b.statusMessage = "DELETED: " + name;
            b.statusSuccess = true;
        } else {
            b.statusMessage = "DELETE FAILED";
            b.statusSuccess = false;
        }
        return;
    }

    // SET_HOME mode: A is the YES of "A=YES B=NO", armed by SELECT+A on a granted tree.
    if (b.mode == BrowserMode::SET_HOME) {
        const BrowserItem* item = b.current();
        b.mode = BrowserMode::NORMAL;
        if (!item || !item->isRoot) return;

        const std::string name = item->displayName;
        if (fs_.set_home_directory(item->path)) {
            // ⚠️ **The two derived roots have to move WITH it, or the app looks in the new tree and
            // resolves media against the old one.** Both were read from the filesystem once, at boot
            // (`AppConfig::mediaBaseDir` and `SongcoreHost::set_app_root`), and neither is re-asked:
            // a project's RELATIVE sample paths join onto the first, and an absolute path authored
            // elsewhere re-roots onto the second. Derived here from an accessor rather than from the
            // row, so pt-ui never has to know what a platform's root string looks like.
            const std::string root = fs_.parent_path(fs_.samples_directory());
            set_media_base_dir(root);
            host_.set_app_root(root);

            refresh_browser();
            b.statusMessage = "HOME FOLDER: " + name;
            b.statusSuccess = true;
        } else {
            b.statusMessage = "COULD NOT SET HOME FOLDER";
            b.statusSuccess = false;
        }
        return;
    }

    // FORGET_ROOT mode: A is the YES of "A=YES B=NO", armed by SELECT+B on a granted tree.
    if (b.mode == BrowserMode::FORGET_ROOT) {
        const BrowserItem* item = b.current();
        b.mode = BrowserMode::NORMAL;
        if (!item || !item->isRoot) return;

        const std::string name = item->displayName;
        if (fs_.revoke_access(item->path)) {
            // ⚠️ The home may have been THIS tree, in which case the filesystem has just chosen another
            // — so the derived roots are re-asked here exactly as they are after a home change, and for
            // the same reason. An accessor answers the truth; a value read at boot does not.
            const std::string root = fs_.parent_path(fs_.samples_directory());
            set_media_base_dir(root);
            host_.set_app_root(root);

            refresh_browser();
            b.statusMessage = "FORGOT: " + name;
            b.statusSuccess = true;
        } else {
            b.statusMessage = "COULD NOT FORGET IT";
            b.statusSuccess = false;
        }
        return;
    }

    const BrowserItem* item = b.current();
    if (!item) return;

    if (item->is_parent()) { navigate_to_parent(b, fs_); return; }

    // An ACTION is not a place, it is a thing to do, and the filesystem owns what it means.
    //
    // ⚠️ **NO refresh on the way out, and that is not an omission.** `activate` starts something it
    // does not wait for — the one action there is opens Android's folder picker — so a listing rebuilt
    // here would be rebuilt from a world that has not changed yet. What catches up is
    // `refresh_browser_on_foreground()`, when the app comes back.
    if (item->kind == BrowserItem::Kind::ACTION) {
        const std::string label = item->displayName;
        if (!fs_.activate(item->path)) {
            b.statusMessage = label + " FAILED";
            b.statusSuccess = false;
        }
        return;
    }

    if (item->kind == BrowserItem::Kind::FOLDER) { navigate_to_folder(b, fs_, item->path); return; }

    // ── It is a FILE, and what happens now is the whole reason the browser was opened ────────────
    const int         id   = s_.currentInstrument;
    const std::string ext  = to_lower(item->extension);
    const std::string path = item->path;
    const std::string stem = item->displayName;

    // Which slot a source load lands on. The SAMPLE EDITOR's own LOAD button targets the slot the
    // EDITOR is on, which is not necessarily the one the INSTRUMENT screen was last left showing.
    const int sourceId = (s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR)
                             ? s_.sampleEditor.instrumentId
                             : id;
    // The name that slot is ABOUT to stop deserving — read before the load overwrites the path it comes
    // from. See the adopt rule below; on anything but a source load nobody looks at it.
    const std::string previousAutoName = instrument_auto_name(host_.project(), sourceId);

    bool ok = false;
    switch (s_.browserPurpose) {
        case AppState::BrowserPurpose::LOAD_PRESET:
            ok = host_.load_instrument_preset(id, path);
            break;

        case AppState::BrowserPurpose::LOAD_SOURCE:
            // The extension decides, not the slot's current type: picking an .sf2 from a sampler slot
            // TURNS it into a SoundFont slot (load_instrument_soundfont sets the type), which is what
            // the user asked for by picking one. The browser's filter usually makes this moot — but the
            // user can navigate anywhere, and a folder full of both is not exotic.
            //
            // `.sf3` is here on the same footing as `.sf2` — same font, same float buffer at the end
            // of it, and the compressed one is the CHEAPER of the two in peak memory (audio-engine.h).
            if (is_soundfont_extension(ext)) {
                ok = host_.load_soundfont(id, path);
            } else {
                ok = host_.load_sample(id, path);
            }
            break;

        case AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR:
            // The editor's own LOAD button. Same load, but it returns to the EDITOR rather than to
            // INSTRUMENT — you came here to pick something to cut up, not to leave.
            ok = host_.load_sample(s_.sampleEditor.instrumentId, path);
            break;

        case AppState::BrowserPurpose::LOAD_PROJECT:
            // ⚠️ The WHOLE DOCUMENT, and it returns early — everything below this switch is about an
            // INSTRUMENT that just gained a source, and none of it applies. `load_project_file` is the
            // guard rail S4 paid 84.4% of a render for: parse → push → load_media → push_params, in one
            // call, so the obligation cannot be forgotten by a new caller. This is that new caller.
            if (!host_.load_project_file(path, fs_.samples_directory())) {
                b.statusMessage = "LOAD FAILED";
                b.statusSuccess = false;
                return;
            }
            load_project_done(path);
            return;

        case AppState::BrowserPurpose::LOAD_THEME:
            // ⚠️ A THEME IS NOT THE PROJECT AND NOT AN INSTRUMENT, so this returns early too — nothing
            // below applies. It touches no engine, no sample, no slot: a palette is pixels.
            //
            // ⚠️ The extension is re-checked even though the browser was opened filtered to "ptt", and
            // Kotlin re-checks it too (`item.file.extension.lowercase() == "ptt"`). The filter is not a
            // guarantee: the user can navigate OUT of the Themes folder into anywhere, and the D-pad
            // does not stop at a directory boundary. A failed parse must not blank the palette.
            if (ext != "ptt" || !load_theme_file(fs_, path, s_.theme)) {
                b.statusMessage = "LOAD FAILED";
                b.statusSuccess = false;
                return;
            }
            // Back to the editor that raised the browser — which `theme_row_action` CLOSED on the way
            // out, because the browser is a screen and would have been standing underneath it.
            close_file_browser();
            open_theme_editor();
            s_.statusMessage = "THEME LOADED";
            s_.statusSuccess = true;
            return;
    }

    if (!ok) {
        // ⚠️ "LOAD FAILED" for a file the DEVICE cannot hold sends the user looking for a corrupt
        // file that is fine. The engine separates the two, and that separation is the whole message:
        // the file is sound, this machine cannot hold it, pick a smaller one. No free figure beside
        // it — the only decision the number could inform has already been made by the refusal, and a
        // megabyte count on a status line is a quantity the user has nothing to compare against.
        b.statusMessage = host_.last_load_ran_out_of_memory() ? "FILE TOO BIG" : "LOAD FAILED";
        b.statusSuccess = false;
        return;
    }

    // The slot adopts the file's name — unless the user TYPED the one it has. Two names are not the
    // user's: the default ("INST07"), and the one the slot took from the source it is replacing right
    // now. Keeping the second is what left a slot reading RHODES C4 after a different sample was
    // loaded onto it, which is a label that lies about what the slot plays.
    //
    // A typed name still survives a source swap — that is the whole reason the default-name test was
    // here — and it is `previousAutoName`, captured before the load, that can tell the two apart.
    //
    // Both source loads, not just the INSTRUMENT screen's: the editor's LOAD replaces the same slot's
    // sample, and a rule that held on one of the two doors would leave a name the other could no
    // longer correct (it would no longer match `previousAutoName`, and would look typed forever).
    if ((s_.browserPurpose == AppState::BrowserPurpose::LOAD_SOURCE ||
         s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR) &&
        sourceId >= 0 && static_cast<size_t>(sourceId) < host_.project().instruments.size()) {
        Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(sourceId)];
        if (songcore::instrument_has_default_name(ins) ||
            (!previousAutoName.empty() && ins.name == previousAutoName)) {
            ins.name = adopted_name(stem);
        }
    }

    mark_modified();

    // D2a: remember the folder a SAMPLE was loaded from — `b.currentDirectory` IS that folder (the file
    // just loaded is an item in it). Only for the two sample-load purposes, and not for a SoundFont
    // picked out of a sampler slot — the row is about SAMPLE folders. Persisted on exit by
    // save_settings_if_changed (no dirty flag), so nothing here has to write the file.
    if ((s_.browserPurpose == AppState::BrowserPurpose::LOAD_SOURCE ||
         s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR) &&
        !is_soundfont_extension(ext)) {
        s_.settings.lastSampleFolder = b.currentDirectory;
    }

    if (s_.browserPurpose == AppState::BrowserPurpose::LOAD_SAMPLE_EDITOR) {
        // Re-enter the EDITOR on the new audio — not `previousScreen`, which is still the INSTRUMENT
        // screen the editor itself will return to. Everything the old sample's session knew is now false,
        // so the state is rebuilt rather than patched.
        host_.clear_previews();
        s_.fileBrowser.selectionMode   = false;
        s_.fileBrowser.selectionAnchor = -1;
        s_.currentScreen = ScreenType::SAMPLE_EDITOR;
        init_sample_editor_state();
        return;
    }

    close_file_browser();
}

// ─── The multi-select and the file clipboard ─────────────────────────────────────────────────────

std::vector<std::string> InputDispatcher::browser_selected_paths() const {
    const FileBrowserState& b = s_.fileBrowser;
    std::vector<std::string> out;
    if (!b.selectionMode || b.selectionAnchor < 0) return out;

    const int lo = std::max(std::min(b.selectionAnchor, b.cursor), b.first_selectable());
    const int hi = std::max(b.selectionAnchor, b.cursor);
    for (int i = lo; i <= hi; ++i) {
        const BrowserItem* item = b.item_at(i);
        if (item && !item->is_pseudo()) out.push_back(item->path);
    }
    return out;
}

void InputDispatcher::browser_paste() {
    FileBrowserState& b = s_.fileBrowser;
    if (b.fileClipboard.empty()) return;

    const std::string& dest = b.currentDirectory;
    int done = 0, failed = 0;

    for (const std::string& src : b.fileClipboard) {
        if (!fs_.file_exists(src)) { ++failed; continue; }

        const std::string name = path_name(src);
        std::string       target = dest + "/" + name;
        if (target == src) { ++done; continue; }   // pasted back into the folder it was copied from

        // De-duplicate: "kick.wav" → "kick_2.wav" → "kick_3.wav". Never overwrite — a paste that
        // silently replaced a file of the same name would be a data-loss bug wearing a convenience hat.
        if (fs_.file_exists(target)) {
            const std::string ext  = path_extension(name);
            const std::string base = path_stem(name);
            for (int n = 2;; ++n) {
                target = dest + "/" + base + "_" + std::to_string(n) + (ext.empty() ? "" : "." + ext);
                if (!fs_.file_exists(target)) break;
            }
        }

        const bool ok = b.fileClipboardIsCut ? fs_.move_file(src, target) : fs_.copy_file(src, target);
        if (ok) ++done; else ++failed;
    }

    const bool  cut  = b.fileClipboardIsCut;
    const char* verb = cut ? "MOVED" : "COPIED";

    // A CUT clipboard is spent once pasted — the sources are gone. A COPY one survives, so the same
    // files can be pasted into several folders.
    if (cut) b.fileClipboard.clear();

    refresh_browser();
    b.statusMessage = failed == 0
                          ? std::string(verb) + " " + std::to_string(done) +
                                (done == 1 ? " FILE" : " FILES")
                          : std::string(verb) + " " + std::to_string(done) + ", FAILED " +
                                std::to_string(failed);
    b.statusSuccess = (failed == 0);
}

// ─── SELECT + A / B / R — the browser's file-management chords ───────────────────────────────────

void InputDispatcher::on_select_a() {
    if (top_overlay() != Overlay::BROWSER) return;   // a browser-only chord
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    const BrowserItem* item = s_.fileBrowser.current();

    // ⭐ **On a granted TREE the three file chords are all meaningless, so SELECT+A means the one thing
    // that row CAN offer: make this the folder the app keeps its own directories in.** Before this
    // there was no way to change it at all — Android's first grant won permanently, and a user who
    // granted the wrong folder had to clear the app's data to get out of it. Armed, never immediate:
    // A confirms, B cancels, exactly as SELECT+B's delete does.
    if (item && item->isRoot) {
        s_.fileBrowser.mode = BrowserMode::SET_HOME;
        s_.fileBrowser.statusMessage.clear();
        s_.fileBrowser.statusSuccess = true;
        return;
    }

    if (!item || item->is_pseudo()) return;   // ".." and ADD FOLDER… are not files and have no name

    const bool  dir   = (item->kind == BrowserItem::Kind::FOLDER);
    const std::string ext = to_lower(item->extension);
    const char* label = dir              ? "FOLDER NAME:"
                        : (ext == "wav") ? "SAMPLE NAME:"
                        : (ext == "ptp") ? "PROJECT NAME:"
                                         : "FILE NAME:";

    // A FOLDER's displayName is "[name]" — the brackets are decoration, and typing them back into the
    // rename box would make them part of the name.
    const std::string base = dir ? path_name(item->path) : item->displayName;
    open_qwerty(QwertyContext::FILE_RENAME, base, label, item->path);
}

void InputDispatcher::on_select_b() {
    if (top_overlay() != Overlay::BROWSER) return;   // a browser-only chord
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    const BrowserItem* item = s_.fileBrowser.current();

    // ⚠️ **On a granted TREE this is FORGET, not DELETE, and the difference is everything the row is.**
    // `delete_path("pt://<id>")` resolves to the granted folder's own document — the user's whole
    // PocketTracker directory. What SELECT+B can honestly offer here is handing the PERMISSION back,
    // which removes the row and touches not one file. It is also the only way to clear a folder that
    // has been deleted from under its grant: Android keeps such a grant for the life of the install.
    if (item && item->isRoot) {
        s_.fileBrowser.mode = BrowserMode::FORGET_ROOT;
        s_.fileBrowser.statusMessage.clear();
        s_.fileBrowser.statusSuccess = true;
        return;
    }

    if (!item || item->is_pseudo()) return;

    // ARM the confirm; never delete on this press. The top bar becomes "DELETE <name>? A=YES B=NO".
    s_.fileBrowser.mode          = BrowserMode::DELETE;
    s_.fileBrowser.statusMessage.clear();
    s_.fileBrowser.statusSuccess = true;
}

void InputDispatcher::on_select_r() {
    if (top_overlay() != Overlay::BROWSER) return;   // a browser-only chord
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;
    open_qwerty(QwertyContext::FOLDER_CREATE, "NEW FOLDER", "FOLDER NAME:",
                s_.fileBrowser.currentDirectory);
}

// ─── The QWERTY keyboard ─────────────────────────────────────────────────────────────────────────

void InputDispatcher::open_qwerty(QwertyContext context, const std::string& initial_text,
                                  const std::string& field_label, const std::string& context_extra,
                                  int max_length, bool clear_on_first_b) {
    QwertyKeyboardState k{};
    k.isOpen        = true;
    k.text          = initial_text.substr(0, static_cast<size_t>(max_length));
    k.maxLength     = max_length;
    k.textCursor    = static_cast<int>(k.text.size());
    k.fieldLabel    = field_label;
    k.contextExtra  = context_extra;
    k.context       = context;
    k.clearOnFirstB = clear_on_first_b;
    k.insertBefore  = s_.settings.insertBefore;   // read at OPEN, so flipping the setting cannot change what
    s_.qwerty       = k;                 // the buttons mean under the user's thumb mid-word
}

void InputDispatcher::qwerty_apply() {
    const QwertyKeyboardState k    = s_.qwerty;   // by value: every arm below closes the keyboard
    const std::string         text = trimmed_text(k);
    s_.qwerty = QwertyKeyboardState{};

    switch (k.context) {
        case QwertyContext::FILE_RENAME: {
            // An empty field means "leave it alone", not "name it nothing" — Kotlin's `.ifEmpty { … }`.
            const std::string name = text.empty() ? path_stem(k.contextExtra) : text;
            if (fs_.rename_file(k.contextExtra, name)) {
                refresh_browser();
                s_.fileBrowser.statusMessage = "RENAMED";
                s_.fileBrowser.statusSuccess = true;
            } else {
                s_.fileBrowser.statusMessage = "RENAME FAILED";
                s_.fileBrowser.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::FOLDER_CREATE: {
            const std::string name = text.empty() ? "NewFolder" : text;
            if (!fs_.create_folder(k.contextExtra, name).empty()) {
                refresh_browser();
                s_.fileBrowser.statusMessage = "CREATED";
                s_.fileBrowser.statusSuccess = true;
            } else {
                s_.fileBrowser.statusMessage = "CREATE FAILED";
                s_.fileBrowser.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::INSTRUMENT_NAME: {
            Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(s_.currentInstrument)];
            // A cleared name reverts to the default "INSTxx" rather than becoming blank — an unnamed
            // instrument still has to be identifiable in the pool.
            ins.name = text.empty() ? songcore::default_instrument_name(ins.id) : text;
            mark_modified();
            break;
        }

        case QwertyContext::PROJECT_NAME:
            // ⚠️ No empty-name fallback, unlike every other arm here. Kotlin's is a bare
            // `trackerController.project.name = typedText`, and it is ported as-is — but note what it
            // leads to: an empty name sanitizes to an empty filename, so SAVE writes `<Projects>/.ptp`
            // (hidden on a POSIX box, and not listed by the browser's own filter). The same is true on
            // Android today. Left bug-for-bug rather than quietly diverging; it wants a fix on BOTH
            // platforms, which makes it a finding, not a port decision.
            host_.edit_project().name = text;
            mark_modified();
            break;

        case QwertyContext::INSTRUMENT_SAVE: {
            const std::string name = text.empty() ? "PRESET" : text;
            const std::string path = k.contextExtra + "/" + name + ".pti";
            if (save_instrument_preset(host_, fs_, s_.currentInstrument, path)) {
                s_.statusMessage = "SAVED: " + name;
                s_.statusSuccess = true;
            } else {
                s_.statusMessage = "SAVE FAILED";
                s_.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::THEME_SAVE:
            // The whole arm is `save_theme_as` — see it for the two names one typed string turns into,
            // and for the error return Kotlin drops on the floor. ⚠️ `k.contextExtra`, not
            // `s_.qwerty.contextExtra`: the live keyboard was cleared at the top of this function.
            save_theme_as(k.contextExtra, text);
            break;

        case QwertyContext::SAMPLE_NAME: {
            // It renames BOTH the editor's sample and the INSTRUMENT holding it — they are the same
            // thing to the user, and the pool showing "INST05" for a slot you have just named "SNARE"
            // would be the app disagreeing with itself. An empty field keeps the current name.
            SampleEditorState& se = s_.sampleEditor;
            const std::string  name = text.empty() ? se.sampleName : text;
            se.sampleName = name;
            host_.edit_project().instruments[static_cast<size_t>(se.instrumentId)].name = name;
            mark_modified();
            break;
        }

        case QwertyContext::SAMPLE_SAVE: {
            // SAVE-AS. The name is DE-DUPLICATED rather than overwritten: `SNARE.wav`, `SNARE_0001.wav`,
            // … The editor has an OVERWRITE button and this is not it — a save that silently replaced a
            // file you did not name would be the one destructive act with no confirm in front of it.
            const std::string base = text.empty() ? "SAMPLE" : text;
            std::string       path = k.contextExtra + "/" + base + ".wav";
            for (int n = 1; fs_.file_exists(path); ++n) {
                char suffix[16];   // 16, not 8: "_%04d" of an unbounded int is up to 12 bytes, and gcc says so (-Wformat-truncation). The counter never gets near it; the buffer now cannot be the reason.
                std::snprintf(suffix, sizeof(suffix), "_%04d", n);
                path = k.contextExtra + "/" + base + suffix + ".wav";
            }
            save_sample_to(path, /*adopt_name=*/true);
            break;
        }

        case QwertyContext::RESAMPLE:
            // An empty field (the user cleared the pre-filled suggestion) auto-names Resample_NNNN;
            // anything typed is used as the base name. The selection is still live — opening the
            // keyboard never touched it — so resample_selection reads s_.selection itself.
            resample_selection(text);
            break;
    }
}

// ─── INSTRUMENT's buttons — the three cells S4 drew and could not press ──────────────────────────

bool InputDispatcher::instrument_open_at_cursor() {
    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::INST_POOL) {
        // A on the pool's NAME column of an EMPTY slot loads a source straight into it (M8's "tap EDIT
        // on an empty slot"). A slot that already HAS a source is managed on the INSTRUMENT screen —
        // loading over it from the pool, where you cannot see what is in it, would be too easy to do by
        // accident.
        if (s_.poolCursorColumn != 0) return false;
        const Instrument& ins  = p.instruments[static_cast<size_t>(s_.currentInstrument)];
        const bool        isSF = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;
        // ⚠️ An EXTERNAL slot has NO source of any kind, so there is nothing to browse for. Without
        // this it would fall to the sampler arm and open the SAMPLES browser — and a file loaded there
        // would flip the slot's type back out from under the user. The pool draws no EDIT for it.
        if (!instrument_has_source_row(ins.instrumentType)) return false;
        if (isSF ? ins.soundfontPath.has_value() : !songcore::instrument_is_free(ins)) return false;

        open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                          isSF ? browser_dir(BrowserDir::SOUNDFONTS) : browser_dir(BrowserDir::SAMPLES),
                          isSF ? soundfont_extensions() : sample_extensions());
        return true;
    }

    if (s_.currentScreen != ScreenType::INSTRUMENT) return false;

    const Instrument& ins  = p.instruments[static_cast<size_t>(s_.currentInstrument)];
    const bool        isSF = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;
    const int         row  = s_.instrumentCursorRow;
    const int         col  = s_.instrumentCursorColumn;

    // Row 0 — TYPE (col 1) + the SOURCE buttons. LOAD (col 2) browses for a sample / SoundFont; EDIT
    // (col 3) opens the sample editor. (The instrument PRESET save/load lives on row 5 now.)
    //
    // ⚠️ Neither button exists on EXTERNAL — it draws neither and the cursor caps at column 1 — so
    // this is the same belt-and-braces consume the SF's EDIT arm below is: unreachable by the D-pad,
    // and refusing here rather than opening a browser for a source that does not exist.
    if (row == 0 && col >= 2 && !instrument_has_source_row(ins.instrumentType)) return true;

    if (row == 0 && col == 2) {
        open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                          isSF ? browser_dir(BrowserDir::SOUNDFONTS) : browser_dir(BrowserDir::SAMPLES),
                          isSF ? soundfont_extensions() : sample_extensions());
        return true;
    }
    // ⚠️ Samplers only, and the refusal is silent: a SoundFont has no single waveform to cut — it is a
    // bank of them, each mapped to a key range — so there is nothing for the editor to draw. The EDIT
    // button is not drawn on SF (the cursor caps at LOAD), so this arm is reached on samplers only; the
    // isSF guard stays as a belt-and-braces consume.
    if (row == 0 && col == 3) {
        if (isSF) return true;   // handled: the press is CONSUMED, it just opens nothing
        open_sample_editor();
        return true;
    }

    // Row 5 — the INSTRUMENT PRESET row. A .pti carries the whole instrument: its params, its mod slots,
    // its table, and the path to its source. SAVE (col 2) names and writes it; LOAD (col 3) browses.
    if (row == 5 && col == 2) {
        const std::string dir  = fs_.instruments_directory();
        const std::string name = ins.name.empty() ? songcore::default_instrument_name(ins.id) : ins.name;
        open_qwerty(QwertyContext::INSTRUMENT_SAVE, name, "SAVE PRESET:", dir, /*max_length=*/20,
                    /*clear_on_first_b=*/true);
        return true;
    }
    if (row == 5 && col == 3) {
        open_file_browser(AppState::BrowserPurpose::LOAD_PRESET, browser_dir(BrowserDir::INSTRUMENTS),
                          {"pti"});
        return true;
    }

    // ⚠️ Row 1 (NAME) and row 12/14 col 1 (the EQ cell) are NOT here — they are the two DEFERRED cells,
    // and they live in `open_sub_screen_at_cursor` with the other four. That split is Kotlin's, and it
    // is the difference between a cell whose A fires on the PRESS (these — read-only buttons with no
    // A+DPAD to protect) and one whose A must wait for the RELEASE.
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
// THE SAMPLE EDITOR (Phase 3 S6b)
// ═════════════════════════════════════════════════════════════════════════════════════════════════

namespace {

/** The waveform panel is 620px wide, so it asks the engine for 620 (min, max) pairs. */
constexpr int WAVEFORM_BINS = SampleEditorModule::WAVEFORM_W;

/** SOURCE mode → the channel the waveform is drawn from: 0 = left, 1 = right, 2 = averaged. */
int waveform_channel(int source_mode) {
    return (source_mode == 0) ? 0 : (source_mode == 1) ? 1 : 2;
}

}  // namespace

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

    // The NAME is the FILE's, not the instrument's — you are editing a sample, and its file is what you
    // will save it back over.
    se.sampleFilePath = ins.sampleFilePath.value_or("");
    se.sampleName     = se.sampleFilePath.empty() ? "" : path_stem(se.sampleFilePath);

    se.totalFrames   = host_.sample_length(se.instrumentId);
    se.sampleRate    = host_.sample_rate_of(se.instrumentId);
    se.hasStereoData = host_.has_stereo_data(se.instrumentId);

    // ⚠️ SOURCE OPENS ON STEREO FOR A STEREO SAMPLE, and this is a SAVE decision rather than a display
    // one. The mode is what `resolve_save_channels` reads: every value but STEREO writes a ONE-CHANNEL
    // file, so a session that never visited the row would silently throw away the right channel of a
    // stereo sample the user only meant to trim. The mode a save cannot lose is the one to open on.
    // (Mono has nothing to choose — the row draws "MONO" and is read-only there whatever this says.)
    se.sourceMode = se.hasStereoData ? 2 /*STEREO*/ : 0;

    se.waveformData  = host_.sample_waveform(se.instrumentId, WAVEFORM_BINS, 0, 0,
                                             waveform_channel(se.sourceMode));

    // The selection opens on the instrument's OWN sample window. The instrument stores it as 0..255 (it
    // is a playback parameter, not a frame count) and the editor works in frames, so it is scaled in
    // here and scaled back out on the way to a preview.
    if (se.totalFrames > 0) {
        se.selectionStart = (static_cast<int64_t>(ins.sampleStart) * se.totalFrames) / 255;
        se.selectionEnd   = (static_cast<int64_t>(ins.sampleEnd) * se.totalFrames) / 255;
        // ⚠️ START and END are two independent free 0-255 cells, so an INVERTED window is typeable
        // and arrives here as `start > end`. It opens on the WHOLE sample, which is not a repair
        // chosen here — it is what `Voice::trigger` already does with the same pair, so the editor
        // shows the region the engine plays. Drawn as-is it would show no selection at all (the
        // waveform lights `>= start && < end`), which reads as "nothing selected".
        if (se.selectionStart >= se.selectionEnd) {
            se.selectionStart = 0;
            se.selectionEnd   = se.totalFrames;
        }
    } else {
        se.selectionStart = 0;
        se.selectionEnd   = 0;
    }

    // The FILE's markers come from the PROJECT, which got them from the file's `cue ` chunk when the
    // sample was loaded (engine_setup.h / wav_writer.h). No file I/O here, and no race: the editor and
    // the loader are looking at the same list. (`sliceMarkers` is int64 — Kotlin's `List<Long>` — and a
    // frame index is an int everywhere else, so the narrowing is said out loud rather than left to the
    // compiler.)
    //
    // ⚠️ **SORTED, UNIQUE and strictly INSIDE the sample**, because a `cue ` chunk is written by whatever
    // made the file and nothing upstream promises any of the three. Every marker reader rests on all
    // three — `slice_bounds` reads marker `k − 1` and marker `k` as one slice's two edges, so an
    // out-of-order pair is a negative-length slice and a duplicate a zero-length one, which is what CHOP
    // would hand to the file writer. MANUAL seeds itself from this list and `place_slice_marker` then
    // maintains the invariant, so this is the only door it can come in through.
    se.fileMarkers.clear();
    se.fileMarkers.reserve(ins.sliceMarkers.size());
    for (const int64_t m : ins.sliceMarkers)
        if (m >= 1 && m < static_cast<int64_t>(se.totalFrames)) se.fileMarkers.push_back(static_cast<int>(m));
    std::sort(se.fileMarkers.begin(), se.fileMarkers.end());
    se.fileMarkers.erase(std::unique(se.fileMarkers.begin(), se.fileMarkers.end()), se.fileMarkers.end());

    // ⚠️ The DETECTOR's list opens EMPTY, and that is behaviour rather than tidiness: `sliceMethod`
    // survives a re-entry, so an editor re-opened while TRANSIENT is still selected must re-detect on
    // the new audio. Empty is the only thing the feed reads as "detect".
    se.transientMarkers.clear();
    // ⚠️ And so do the HAND-PLACED ones. The METHOD survives a re-entry; the markers cannot — they are
    // frame indices into audio that has just been replaced, and a position that meant a drum hit in one
    // sample means the middle of a chord in the next.
    se.manualMarkers.clear();
    se.manualKeyMethod = -1;
    se.manualKeyParam  = -1;
    se.sliceIndex = 0;
    // ⚠️ sliceMethod is deliberately NOT reset — it opens at OFF on a fresh session (the struct's
    // default) and SURVIVES a re-entry, so loading a second sample to compare does not silently drop you
    // back out of TRANSIENT mode.

    se.isModified       = false;
    se.showConfirmClose = false;
    se.playbackPosition = -1.0f;
}

void InputDispatcher::close_sample_editor() {
    // Anything the audition still owes the instrument, it pays now — leaving with a restore pending would
    // put the preview's sample window back onto a slot that is no longer on screen.
    run_due_sample_preview_restore(/*force=*/true);

    host_.restore_fx_preview_backup();          // drop an un-applied FX preview
    host_.free_sample_undo(s_.sampleEditor.instrumentId);   // unreachable once the editor is gone
    host_.clear_previews();                     // the 254/255 scratch slots

    s_.currentScreen = s_.previousScreen;
}

// ─── The selection: A+DPAD on rows 3..8 ──────────────────────────────────────────────────────────

void InputDispatcher::nudge_selection_edge(int64_t delta) {
    SampleEditorState& se = s_.sampleEditor;

    // ⚠️ **AN ANDROID CRASH, FOUND BY PORTING.** With NO sample loaded, `totalFrames` and `selectionEnd`
    // are both 0 — and Kotlin's arms are `coerceIn(0, selectionEnd - 1)` and `coerceIn(selectionStart + 1,
    // maxFrame)`, i.e. `coerceIn(0, -1)` and `coerceIn(1, 0)`. Both have min > max, and `coerceIn` REQUIRES
    // min <= max: it throws IllegalArgumentException, and the app dies. It is reachable in four presses —
    // EDIT on a fresh sampler slot, DOWN, DOWN, A+RIGHT — because nothing on the way in checks that the
    // slot has any audio in it. (In C++ it would be worse than a crash: `std::clamp` with lo > hi is UB.)
    //
    // A selection inside a sample with no frames is meaningless, so there is nothing to nudge. Zone B, so
    // it is fixed on Android too (AppInputDispatcher.nudgeSelectionEdge), per §4's rule.
    if (se.totalFrames <= 0) return;

    const int64_t maxFrame = se.totalFrames;
    const int     dir      = (delta >= 0) ? 1 : -1;

    // SNAP moves the edge on to the nearest zero crossing IN THE DIRECTION OF TRAVEL, which is what keeps
    // a trimmed sample from clicking at its own boundary. Searching in the direction of the nudge (rather
    // than the nearest in either) is what stops the edge sticking: a crossing you have just left is
    // always the nearest one.
    // ⚠️ THROUGH THE SOURCE MODE, so the crossing is looked for in the signal the SAVE will write. On a
    // stereo sample the left channel alone is one of two: an edge that sits exactly on a left crossing
    // leaves the right one wherever it happened to be, and the seam clicks in one ear.
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
 * Put boundary `k` at `want` — or MAKE one there when `k` is not in the list — and answer with the
 * index it ended up at, which is not necessarily the one it started from. −1 when there is nowhere for
 * it to go.
 *
 * ⭐ **A boundary MAY be dragged past its neighbours; its NUMBER follows its POSITION.** The list stays
 * sorted because it is re-sorted here, in the one place a boundary can move, rather than because a
 * clamp forbids the crossing — and that is what lets a boundary made at the end be walked back to
 * anywhere in the sample instead of having to be deleted and remade.
 *
 * ⚠️ **Two boundaries may never share a frame.** `slice_bounds` reads marker `k − 1` as the left edge
 * of slice `k` and marker `k` as its right, so a duplicate is a zero-length slice — one CHOP would hand
 * to the file writer. A landing that is taken is stepped PAST in the direction of travel rather than
 * refused, so a drag through a crowd keeps moving.
 *
 * ⚠️ Frame 0 and the last frame are the sample's own bounds and not boundaries within it — the same
 * rule `compute_slice_cue_points` applies when it writes the `cue ` chunk.
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

    // ⚠️ MOVE the boundary, never rebuild it: `originFrame` is its identity and has to travel with it
    // through every crossing, or A+B loses the position it is meant to put the boundary back on. A
    // boundary that is MADE here rather than moved has no computed home, which is what −1 says.
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
 * Take a copy of whatever the method currently answers with, so the user's nudges have something to
 * write into, and stamp it with the (method, parameter) it describes.
 *
 * Every method starts from the set it already shows, so a drag adjusts what is on screen rather than
 * throwing it away: the detected cuts under TRANSIENT, the arithmetic ones under DIVIDE, and under
 * MANUAL the boundaries the SAMPLE ITSELF came with — its `cue ` chunk, as the project loaded it. A
 * sample carrying no cue points starts MANUAL empty, and its first boundary is born on frame 0, which
 * is the sample's own start and not a boundary until something is dragged off it.
 */
void InputDispatcher::materialise_manual_markers() {
    SampleEditorState& se = s_.sampleEditor;
    if (se.manual_markers_live()) return;

    // ⚠️ Each copy remembers the frame it was made at, and that is the whole of "put slice 01 back where
    // DIVIDE had it" — the boundary can be dragged past its neighbours afterwards, so by the time A+B
    // arrives its index says nothing about which cut it is.
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
        // ⚠️ ORIGIN −1, unlike the two above: the file's cue points are not a position any method can
        // RECOMPUTE, so A+B on one REMOVES it. That is deliberate and it is the point of the seed — the
        // gesture that deletes a boundary the sample came with is the same A+B that deletes one placed by
        // hand, and there is no second meaning of A+B on this row to learn.
        for (const int f : se.fileMarkers) se.manualMarkers.push_back(SliceMarker{f, -1});
    }
    se.manualKeyMethod = se.sliceMethod;
    se.manualKeyParam  = se.manual_key_param();
}

/**
 * The selection follows the slice the row-11 cursor is on. ⚠️ Every gesture that moves a boundary or
 * changes which one is under the cursor ends here — a slice whose edge has just moved is one START must
 * play the new shape of, and the reset leaving the old shape behind is the defect this closes.
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

    // The same guard `nudge_selection_edge` carries, for the same reason: this screen is reachable on an
    // empty slot in four presses, and `std::clamp` with lo > hi is UB rather than a wrong answer.
    if (se.totalFrames <= 0 || delta == 0) return;

    const bool manual = se.sliceMethod == SampleEditorModule::SLICE_MANUAL;
    const int  k      = se.slice_marker_index();

    // Slice 00's left edge is the sample's own start: under TRANSIENT and DIVIDE there is no boundary
    // there and nothing to drag. ⭐ Under MANUAL a rightward drag MAKES one instead of moving that edge
    // — the sample does not start later, it gains a cut — and the cursor follows the new boundary onto
    // whichever slice it opens. Leftward there is nowhere to go: frame 0 is the sample's own start.
    if (k < 0 && (!manual || delta < 0)) return;

    materialise_manual_markers();
    std::vector<SliceMarker>& m = se.manualMarkers;

    // A boundary past the end of the list is MANUAL's free slot — the next one, not yet made. It is
    // born on the boundary to its left, which is where the cell already reads, and this drag is what
    // carries it off there.
    if (k >= static_cast<int>(m.size()) && !manual) return;
    const int64_t from = (k >= 0 && k < static_cast<int>(m.size()))
                             ? static_cast<int64_t>(m[static_cast<size_t>(k)].frame)
                             : (k < 0 || m.empty() ? 0 : static_cast<int64_t>(m.back().frame));

    const int dir  = (delta > 0) ? 1 : -1;
    int64_t   want = from + delta;
    // SNAP is row 2's toggle and it already works on the selection edges; a boundary is the same kind of
    // cut through the same audio, so it reads the same switch and searches in the direction of travel.
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

    // ⚠️ **MATERIALISE FIRST, like the drag and the tap do** — all three gestures on this row go through
    // the same door. The boundary under the cursor may be one the METHOD is still answering with and
    // nothing has copied yet, and under MANUAL that is a cue point the sample arrived with: A+B is how it
    // is deleted, so a "nothing has been placed yet" bail made the FIRST A+B of a session do nothing.
    materialise_manual_markers();
    std::vector<SliceMarker>& m = se.manualMarkers;
    if (k >= static_cast<int>(m.size())) return;      // MANUAL's free slot holds no boundary yet

    // ⭐ THE BOUNDARY ITSELF SAYS WHERE IT GOES, so there is no arm per method here and — more to the
    // point — nothing reads the boundary's INDEX to decide. The index is where it currently sits on
    // screen, which after a crossing is somebody else's cut.
    const int origin = m[static_cast<size_t>(k)].originFrame;

    if (origin < 0) {
        // Placed by hand, or seeded from the file's own cue points: there is no computed position to go
        // back to, so A+B REMOVES it. The boundaries after it renumber, and the cursor keeps its number
        // and therefore names the slice that has just grown into the gap.
        //
        // ⚠️ Emptying the list does NOT retire the stamp. An empty live list is "this sample has no
        // boundaries", and it is the answer every reader must get; drop the stamp and the read falls back
        // to `method_markers()`, which under MANUAL is the file's own cue points — every delete undone at
        // once by the one that finished the job.
        m.erase(m.begin() + k);
    } else {
        // ⚠️ Through `place_slice_marker` like every other move, because a NEIGHBOUR may have been
        // dragged across that position since — the boundary then takes the number its own place gives
        // it, rather than breaking the sort order.
        const int landed = place_slice_marker(m, k, origin, +1, se.totalFrames);
        if (landed < 0) return;
        se.sliceIndex = landed + 1;
    }

    select_current_slice();
}

/**
 * Cut a boundary at the playhead — the "slice it by ear" gesture. MANUAL only, and only while the
 * sample is actually sounding: with nothing playing there is no playhead to cut at.
 *
 * ⭐ **The frame comes from `playbackPosition`, the same field the waveform draws its playhead line
 * from, and that is the point rather than a shortcut.** A fresher number straight off the voice would
 * land the boundary somewhere the user never saw — they are tapping to a line on the screen and to
 * audio that left the device a buffer ago, so the line they were looking at IS the frame they meant.
 *
 * ⚠️ **And it is the line as it stood when A went DOWN** (`on_a_deferred`), not when it came up. The
 * press is the tap; the release is only where the mapper can tell a tap from an A+DPAD, and by then the
 * playhead has run on for as long as the user held the button.
 *
 * SNAP searches BACKWARD, unlike a drag's "in the direction of travel". A tap has no direction, and it
 * is always LATE — reaction time plus the audio buffer — so the zero crossing that matters is the one
 * before the hit rather than the one inside it.
 */
void InputDispatcher::tap_slice_marker() {
    SampleEditorState& se = s_.sampleEditor;

    // Consumed, not merely read: a press that ended in an A+DPAD leaves its snapshot behind, and the
    // next tap must not be able to cut at a playhead position from a gesture that was not a tap.
    const float at    = sliceTapPlayhead_;
    sliceTapPlayhead_ = -1.0f;

    if (se.sliceMethod != SampleEditorModule::SLICE_MANUAL) return;
    if (se.totalFrames <= 0 || at < 0.0f) return;

    const int64_t last = static_cast<int64_t>(se.totalFrames) - 1;
    int64_t       want = std::clamp<int64_t>(
        static_cast<int64_t>(at * static_cast<float>(se.totalFrames)), 0, last);
    if (se.snapEnabled)
        want = host_.find_zero_crossing(se.instrumentId, static_cast<int>(want), -1, se.sourceMode);

    materialise_manual_markers();
    std::vector<SliceMarker>& m = se.manualMarkers;

    // ⚠️ Past the end of the list on purpose: a tap always MAKES a boundary, wherever the cursor
    // happens to be sitting. `place_slice_marker` stamps a made one with origin −1 — "by hand, with
    // nowhere to go back to" — which is what makes A+B remove it rather than move it.
    const int landed = place_slice_marker(m, static_cast<int>(m.size()), want, +1, se.totalFrames);
    if (landed < 0) return;

    se.sliceIndex = landed + 1;   // the cursor follows the cut onto the slice it opens
    select_current_slice();
}

// ─── RATE: the destructive one on row 1 ──────────────────────────────────────────────────────────

void InputDispatcher::apply_sample_rate_mode() {
    SampleEditorState& se     = s_.sampleEditor;
    const int          factor = (se.rateMode == 1) ? 2 : (se.rateMode == 2) ? 4 : 1;
    const int          oldLen = se.totalFrames;

    host_.apply_rate_mode(se.instrumentId, factor);

    // ⚠️ The 2-phrase lookahead has ALREADY scheduled notes against the OLD base frequency — they would
    // play the re-decimated buffer at double or half pitch. Rolling the schedule back is what makes the
    // upcoming phrases re-derive it. (A no-op when stopped.)
    if (host_.is_playing()) host_.notify_data_changed();

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

    // ⚠️ RESET, not clamp. An op that SHORTENS the sample (crop, cut, a SYNC that compressed it) leaves a
    // selection that describes frames which no longer exist; clamping it would leave you with a partial
    // selection of the new audio that you never made. Selecting the whole result is the one answer that
    // is always true. Kotlin's `afterResize()` does the same.
    if (reset_selection) {
        se.selectionStart = 0;
        se.selectionEnd   = newLen;

        // ⚠️ AND SO DOES THE INSTRUMENT'S OWN WINDOW, for the same reason and a sharper one: those two
        // 0-255 cells are a FRACTION of the buffer, so a buffer that changed length silently re-aims
        // them at audio the user never pointed at. CROP to a loop with START/END at 40/C0 and the note
        // plays the middle 50 % of the crop — the file on disk holds the whole thing, so it sounds
        // wrong in the tracker and right everywhere else.
        //
        // `cropSample`, `deleteSampleRegion` and `pasteRegion` already reset the ENGINE's copy of the
        // pair; this is the PROJECT's, and without it the next ordinary push — the audition's own
        // restore, 100 ms later — puts the stale fraction straight back. The LOOP pair is the same two
        // cells one row down and gets the same treatment: after a resize it points into audio that is
        // not there any more, and a loop is the thing this editor is most used to cut.
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

        // ⚠️⚠️ **AND EVERY MARKER LIST, for the third time the same reason: a boundary is a FRAME INDEX,
        // and the frames have just been replaced.** A cut that meant a drum hit before a CROP means the
        // middle of the next hit after it, and nothing on screen says the number is stale — it is a line
        // over a waveform, and a wrong line looks exactly like a right one.
        //
        // ⭐ ALL THREE, not the file's alone. `manualMarkers` OVERRIDES the other two while its stamp is
        // live, so clearing only the source underneath a live override changes nothing the user can see;
        // and `transientMarkers` empty is what makes the feed re-detect, so the detector comes back with
        // the cuts in the NEW audio instead of holding the old ones for good.
        //
        // ⚠️ This is what makes a save after a resize honest. `compute_slice_cue_points` drops a marker
        // past the end, so a cropped sample used to write back the SURVIVING half of its old cue points —
        // fewer slices than before, in the wrong places, with no gesture that said so.
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
    SampleEditorState& se     = s_.sampleEditor;
    const int          instId = se.instrumentId;
    const int          startF = static_cast<int>(se.selectionStart);
    const int          endF   = static_cast<int>(se.selectionEnd);

    // Every destructive op opens the same way: drop any un-applied FX preview (so the op acts on the
    // CLEAN audio, not on the effect you were auditioning) and take an undo backup.
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
        // The whole row, both columns, exactly as A+B on it is: which column the cursor sits in says
        // which NUMBER you are reading, and neither of them is what a tap is aimed at.
        //
        // ⚠️ It arrives on A's RELEASE, not its press (`defer_a_to_release`), and cuts at the playhead
        // as it stood on the PRESS (`on_a_deferred`). The row's other gestures all start with the same
        // A held down, so a tap that fired immediately would precede every one of them.
        //
        // ⚠️ This is the one A on this screen that does nothing destructive and takes no undo backup —
        // a boundary is editor state, not audio. It must sit ABOVE the `begin_destructive` rows for
        // that to stay obvious, not because the order matters to the switch.
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
                    // ⚠️ `isModified` is deliberately NOT cleared: one undo does not mean the sample is
                    // back to what the FILE holds — it means it is back one step. Kotlin leaves it too, and
                    // the flag's only job is to put the "ARE YOU SURE?" in front of an unsaved exit.
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
                // OTT / DUST / DRIVE need an AMOUNT to do anything; EQ has none (its value is a slot),
                // so it always applies.
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
            // Two ways to make a sample last a bar, and they are not the same tool. RPITCH RESAMPLES it —
            // faster is higher, which is what you want for a breakbeat. TSTRETCH holds the pitch and moves
            // the time (SOLA), which is what you want for anything with a tune in it.
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
                refresh_sample_view(/*reset_selection=*/true);
                se.isModified = true;
            };

            // ⚠️ "Already on the grid" is a question about FRAMES, and both branches must ask it the
            // same way. A ratio window instead — a thousandth either side of 1.0 — is 8 ms on a 4-bar
            // loop at 120 BPM, which is most of a tick, so a loop inside the window was declared
            // finished while still audibly off the beat.
            const double  ratio      = targetSecs / rawSecs;
            const int64_t wantFrames = std::llround(static_cast<double>(se.totalFrames) * ratio);
            if (ratio <= 0.001 || wantFrames == se.totalFrames) break;

            if (se.syncType == 0) {   // RPITCH
                // ⚠️ FRACTIONAL, and that is the point. This is a fit-to-grid, not the musical
                // transpose row 2 dials in: one semitone is a 5.9 % step in length, so rounding to the
                // nearest one misses the target by up to half of that — 230 ms on a 4-bar loop, twenty
                // ticks. Nothing below here is integer: `pitch_shift_sample` takes a float and the
                // engine resamples by pow(2, semitones/12).
                const double exact     = 12.0 * std::log(rawSecs / targetSecs) / std::log(2.0);
                const float  semitones = static_cast<float>(std::clamp(exact, -24.0, 24.0));
                begin_destructive();
                host_.pitch_shift_sample(instId, semitones);
                // The shift is BAKED, so the pending one on row 2 is spent — leaving it would apply it
                // twice at the next save.
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
                    // ⚠️ `previousScreen` is the EDITOR's return target (INSTRUMENT), and the browser must
                    // not take it: it would leave B on the editor going back to the browser. Kotlin never
                    // assigns it here for the same reason.
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
                    // Taken. Suggest the next free `<base>_0001` and let the user confirm or change it —
                    // SAVE is not OVERWRITE, and the button next to it is.
                    std::string suggested = base;
                    for (int n = 1; fs_.file_exists(dir + "/" + suggested + ".wav"); ++n) {
                        char suffix[16];   // 16, not 8: "_%04d" of an unbounded int is up to 12 bytes, and gcc says so (-Wformat-truncation). The counter never gets near it; the buffer now cannot be the reason.
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

    // Every frame-measured thing moves with the audio. The SELECTION is scaled rather than reset here
    // (unlike an op) because the user has not asked for anything to change — they asked to SAVE, and the
    // shift is a thing they dialled in earlier that is only now being made real.
    se.selectionStart = scale(se.selectionStart);
    se.selectionEnd   = scale(se.selectionEnd);
    se.slicePosition  = scale(se.slicePosition);

    se.totalFrames    = newLen;
    se.pitchSemitones = 0;   // spent
    se.rateMode       = 0;   // the shifted buffer IS the new original — see sample_edit.h
    se.waveformData   = host_.sample_waveform(se.instrumentId, WAVEFORM_BINS, 0, 0,
                                              waveform_channel(se.sourceMode));

    // The instrument's playback params were derived from the old buffer's length.
    host_.push_instrument(se.instrumentId);
    mark_modified();
}

// ─── The slices ──────────────────────────────────────────────────────────────────────────────────

std::vector<int> InputDispatcher::compute_slice_cue_points() const {
    const SampleEditorState& se = s_.sampleEditor;

    // DIVIDE only while it is still arithmetic — a boundary the user has dragged makes it a list like
    // any other, and the cue points must be the ones on the screen.
    if (se.sliceMethod == SampleEditorModule::SLICE_DIVIDE && se.marker_count() == 0) {
        const int div = std::max(se.sliceDivisions, 1);
        std::vector<int> cues;
        cues.reserve(static_cast<size_t>(std::max(div - 1, 0)));
        for (int i = 1; i < div; ++i)
            cues.push_back(static_cast<int>((static_cast<int64_t>(i) * se.totalFrames) / div));
        return cues;
    }

    // Whichever list the method is answering with — the detector's under TRANSIENT, the hand-placed one
    // under MANUAL (which starts as the file's, so ⭐ **a MANUAL save keeps the boundaries the sample
    // came with unless the user moved or deleted them**), and under OFF the file's own, so ⚠️ **a save
    // with slicing OFF can neither add a slice nor drop one.** A detour
    // through TRANSIENT leaves markers behind on purpose (they are what a return to it re-uses); writing
    // those into the `cue ` chunk would put slices into a file the user turned slicing off for, and
    // nothing on screen would say so.
    //
    // Frame 0 and the end frame are dropped: they are the sample's own bounds, not boundaries WITHIN
    // it, and a cue point at 0 gives every reader a zero-length first slice.
    std::vector<int> cues;
    for (int i = 0; i < se.marker_count(); ++i) {
        const int m = static_cast<int>(se.marker_position(i));
        if (m > 0 && m < se.totalFrames) cues.push_back(m);
    }
    return cues;
}

std::vector<std::pair<int64_t, int64_t>> InputDispatcher::current_slices() const {
    const SampleEditorState& se = s_.sampleEditor;

    // N markers → N+1 slices, whichever method the list came from — which is also what DIVIDE's div−1
    // computed cuts mean, so a dragged DIVIDE keeps its own count. OFF has nothing to chop; TRANSIENT
    // before the detector has run, and MANUAL with no boundaries at all, are one slice — the whole sample.
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
    SampleEditorState& se   = s_.sampleEditor;
    const std::vector<int> cues = compute_slice_cue_points();

    if (!host_.save_sample_wav(se.instrumentId, path, cues, se.sourceMode, se.hasStereoData)) {
        s_.statusMessage = "SAVE FAILED";
        s_.statusSuccess = false;
        return;
    }

    // ⚠️ A MONO save is re-loaded from the file it just wrote, and that is not belt-and-braces. The
    // editor's buffer may still be STEREO (SOURCE=LEFT writes one channel of a two-channel sample), and
    // the slot would otherwise go on holding audio that no longer matches the file its instrument points
    // at. A true stereo save (SOURCE=STEREO) already matches, so it is left alone — re-decoding a
    // multi-megabyte file for nothing is exactly the cost the native load path exists to avoid.
    const bool wrote_mono = !(se.hasStereoData && se.sourceMode == 2);
    if (wrote_mono) host_.load_sample(se.instrumentId, path);

    Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(se.instrumentId)];
    ins.sampleFilePath = path;
    // The markers go into the PROJECT as well as into the file — the .ptp is what a reload reads first,
    // and the two must agree.
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

    // Samples/Chops/<base>/ — its own folder, because a 32-slice break would otherwise bury the sample
    // directory it came from.
    const std::string samples = fs_.samples_directory();
    fs_.create_folder(samples, "Chops");                     // "" if it already exists — either is fine
    const std::string chops = samples + "/Chops";
    fs_.create_folder(chops, base);
    const std::string dir = chops + "/" + base;

    const int written = host_.chop_sample(se.instrumentId, dir, base, slices);
    s_.statusMessage   = written > 0 ? ("CHOPPED " + std::to_string(written)) : "CHOP FAILED";
    s_.statusSuccess   = written > 0;
}

// ─── The audition's deferred restore ─────────────────────────────────────────────────────────────

void InputDispatcher::run_due_sample_preview_restore(bool force) {
    if (!previewRestorePending_) return;
    if (!force && now_ms_ < previewRestoreAtMs_) return;

    previewRestorePending_ = false;
    host_.finish_sample_preview(previewRestoreInst_);
}

bool InputDispatcher::defer_a_to_release() const {
    // ⚠️ NO CELL OPENS A SUB-SCREEN WHILE A MODAL IS ALREADY UP, and this guard is Kotlin's
    // (`currentCellOpensSubScreen` opens with it). It stopped being merely tidy the moment the EQ editor
    // landed: the cursor UNDERNEATH the open editor is, by construction, always parked on the very EQ
    // cell that raised it. Without this line every plain A inside the editor would be deferred to
    // release, and the A,A window cleared, for a press that has nothing to open.
    if (any_modal_open()) return false;

    // ⚠️ The one deferred cell that OPENS NOTHING, and it is deferred for the other half of the reason
    // the rest are: on row 11 under MANUAL a plain A cuts a boundary at the playhead, and the same A is
    // held down for the slice-number step, the boundary drag and the delete. Acting on the press left a
    // stray boundary in front of every one of them.
    if (on_slice_tap_cell()) return true;

    return const_cast<InputDispatcher*>(this)->open_sub_screen_at_cursor(/*peek=*/true);
}

bool InputDispatcher::defer_b_to_release() const {
    // The EQ editor. See the header: B is both the CLOSE and the modifier of the slot cycle, so it
    // cannot act until it is known which one the user meant — and only the release says that.
    if (eq_open()) return true;

    // ⚠️ AND WHEREVER R+B IS A MUTE, for the same reason one layer out. TWO BUTTONS ARE NEVER PRESSED
    // ON THE SAME FRAME: a shoulder and a face button aimed at each other still arrive as two events,
    // in whichever order the pad reported them. Acting on B's own press means a B that lands a frame
    // ahead of its R COPIES the selection and closes it — and the R+B that was aimed at that selection
    // then mutes one channel under the cursor instead of the eight that were highlighted.
    //
    // Holding B until it comes up makes the two gestures the same either way round: R can still claim
    // the press (button_mapper's R_SHIFT arm), and a B that is let go unclaimed is the plain copy it
    // always was. It costs nothing visible — B's copy on the release of B is the same instant to a
    // hand — and it is also what stops B+UP/B+DOWN paging a selection away on the way past.
    return mute_solo_chord_live();
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
// THE EQ EDITOR (Phase 3 S8)
// ═════════════════════════════════════════════════════════════════════════════════════════════════

bool InputDispatcher::open_sub_screen_at_cursor(bool peek) {
    const Project& p = *s_.project;

    switch (s_.currentScreen) {
        case ScreenType::PROJECT:
            // The NAME row, any column but the label. Its characters are cursor COLUMNS, each an
            // in-place CHARACTER cell — so on ONE cell A opens the keyboard, A+RIGHT walks that character
            // through the alphabet, and A+B blanks it. The sharpest case the defer latch exists for.
            if (s_.projectCursorRow == static_cast<int>(ProjectRow::NAME) &&
                s_.projectCursorColumn >= 1) {
                if (!peek) open_qwerty(QwertyContext::PROJECT_NAME, host_.project().name,
                                       "PROJECT NAME:", "", PROJECT_NAME_MAX_CHARS);
                return true;
            }
            break;

        case ScreenType::INSTRUMENT: {
            const Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];

            if (s_.instrumentCursorRow == 1) {
                if (!peek) {
                    // A default-named slot opens the box EMPTY rather than with "INST07" in it: you are
                    // naming the instrument, and deleting a placeholder first is six presses of nothing.
                    const std::string cur =
                        songcore::instrument_has_default_name(ins) ? "" : ins.name;
                    open_qwerty(QwertyContext::INSTRUMENT_NAME, cur, "INSTRUMENT NAME:", "");
                }
                return true;
            }
            // EXTERNAL has no EQ row, and `instrument_eq_row` answers −1 there — a row number no cursor
            // can hold — so this test goes false without a type check of its own to forget here.
            if (s_.instrumentCursorRow == instrument_eq_row(ins.instrumentType) &&
                s_.instrumentCursorColumn == 1) {
                // ⚠️ `max(0, …)`: an UNASSIGNED EQ is −1 in the project, and −1 is the engine's bypass
                // value — but it is not a SLOT, and the editor has to open ON one. Kotlin's
                // `coerceAtLeast(0)` says the same thing: opening an unassigned EQ starts you on slot 0.
                if (!peek) open_eq_editor(std::max(0, ins.eqSlot),
                                          EqCallerContext::instrument(s_.currentInstrument));
                return true;
            }
            break;
        }

        case ScreenType::INST_POOL:
            if (s_.poolCursorColumn == 4) {
                const Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];
                if (!peek) open_eq_editor(std::max(0, ins.eqSlot),
                                          EqCallerContext::instrument(s_.currentInstrument));
                return true;
            }
            break;

        case ScreenType::MIXER:
            if (s_.mixerMasterRow == 1 && s_.mixerCursorColumn == 8) {
                if (!peek) open_eq_editor(std::max(0, p.masterEqSlot), EqCallerContext::master());
                return true;
            }
            break;

        case ScreenType::EFFECTS:
            if (s_.effectsCursorRow == EffectModule::ROW_REV_EQ) {
                if (!peek) open_eq_editor(std::max(0, p.reverbInputEq), EqCallerContext::reverb_in());
                return true;
            }
            if (s_.effectsCursorRow == EffectModule::ROW_DLY_EQ) {
                if (!peek) open_eq_editor(std::max(0, p.delayInputEq), EqCallerContext::delay_in());
                return true;
            }
            break;

        case ScreenType::SAMPLE_EDITOR:
            // Only the EQ SLOT cell (row 16, col 1, with the EQ effect selected). Column 2 is APPLY and
            // keeps its own A; A+DPAD on column 1 still dials the slot.
            if (s_.sampleEditor.cursorRow == 16 && s_.sampleEditor.cursorCol == 1 &&
                s_.sampleEditor.fxType == 3) {
                if (!peek) open_eq_editor(std::min(127, std::max(0, s_.sampleEditor.fxValue)),
                                          EqCallerContext::sample_editor_fx());
                return true;
            }
            break;

        default:
            break;
    }
    return false;
}

void InputDispatcher::open_eq_editor(int slot, EqCallerContext caller) {
    s_.eq           = EqEditorState{};
    s_.eq.isOpen    = true;
    s_.eq.slotIndex = std::min(127, std::max(0, slot));
    s_.eq.cursorRow = 0;   // BAND 1, TYPE — the top-left cell, every time
    s_.eq.caller    = caller;
}

void InputDispatcher::eq_move_cursor(int d_band, int d_param) {
    const int band  = std::min(2, std::max(0, s_.eq.cursor_band() + d_band));
    const int param = std::min(3, std::max(0, s_.eq.cursor_param() + d_param));
    s_.eq.cursorRow = band * 4 + param;
}

void InputDispatcher::apply_caller_eq_slot_change(int new_slot) {
    Project& p = host_.edit_project();

    // ⚠️ FIVE DIFFERENT PROJECT FIELDS, ONE GESTURE — and this is the whole reason `EqCallerContext`
    // exists. Cycling the slot from inside the editor has to write back to the cell that RAISED it, and
    // the editor's own state has no idea which that was unless it was told at open time.
    switch (s_.eq.caller.kind) {
        case EqCallerContext::Kind::MASTER:
            p.masterEqSlot = new_slot;
            host_.set_master_eq_slot(new_slot);
            break;
        case EqCallerContext::Kind::REVERB_IN:
            p.reverbInputEq = new_slot;
            host_.set_reverb_input_eq(new_slot);
            break;
        case EqCallerContext::Kind::DELAY_IN:
            p.delayInputEq = new_slot;
            host_.set_delay_input_eq(new_slot);
            break;
        case EqCallerContext::Kind::INSTRUMENT: {
            const int id = s_.eq.caller.instrId;
            if (id >= 0 && id < static_cast<int>(p.instruments.size())) {
                p.instruments[static_cast<size_t>(id)].eqSlot = new_slot;
                host_.set_instrument_eq_slot(id, new_slot);
            }
            break;
        }
        case EqCallerContext::Kind::SAMPLE_EDITOR_FX:
            // No engine call, and none is missing: the sample editor's EQ is applied DESTRUCTIVELY to
            // the buffer when its APPLY button is pressed, reading the bank at that moment. Nothing is
            // filtering live, so there is nothing to re-point.
            s_.sampleEditor.fxValue = new_slot;
            break;
    }

    // Dirty AND armed — not a bare projectVersion++. This path bypasses mark_modified on purpose
    // (its wholesale push_globals is oversized for a 100 ms-repeat band dial; the right-sized push
    // is push_eq_band_to_engine's two calls), but the bypass must not also skip the crash autosave:
    // a session whose only edits are EQ bands deserves the same protection as any other edit.
    mark_dirty_and_arm_autosave();
}

void InputDispatcher::push_eq_band_to_engine() {
    const Project& p       = *s_.project;
    const int      slot    = s_.eq.slotIndex;
    const int      bandIdx = s_.eq.cursor_band();

    if (slot < 0 || slot >= static_cast<int>(p.eqPresets.size())) return;
    const songcore::EqPreset& preset = p.eqPresets[static_cast<size_t>(slot)];
    if (bandIdx < 0 || bandIdx >= static_cast<int>(preset.bands.size())) return;
    const songcore::EqBand& band = preset.bands[static_cast<size_t>(bandIdx)];

    // The BAND, into the engine's 128-slot bank.
    host_.set_eq_band(slot, bandIdx, band.type, band.freq, band.gain, band.q);

    // ⚠️ AND THEN THE CALLER IS RE-HANDED THE SLOT, and the second call is not redundant: `set_eq_band`
    // writes the BANK, and nothing that is filtering right now reads the bank. Re-assigning the slot is
    // what makes the consumer recompile its coefficients. Drop it and every band you dial does nothing
    // you can hear (SongcoreHost::set_eq_band has the engine's side of it).
    //
    // ⚠️ AN ANDROID BUG, FOUND HERE, AND IT IS WHY THIS GOES THROUGH `apply_caller_eq_slot_change`
    // RATHER THAN AN INLINE `when` OVER THE ENGINE SETTERS — which is what Kotlin had.
    //
    // Opening the editor on an UNASSIGNED EQ shows slot 0 (−1 is the bypass value, not a slot, so it is
    // clamped up for display). But nothing WRITES 0 into the project. So Kotlin's band edit told the
    // ENGINE "use slot 0" while `masterEqSlot` stayed at −1: you could HEAR the EQ, the mixer cell still
    // read "--", and the next save-and-reload threw it away, because the load path faithfully re-pushes
    // the −1 the project still held. The project and the engine disagreed about which slot was live —
    // which is the SAME failure S5 found from the other end (deleting a slot wrote −1 to the project and
    // never told the engine, so the EQ went on filtering).
    //
    // Editing a band therefore ADOPTS the slot: one call writes the field AND makes the engine call, so
    // the two cannot come apart. For an already-assigned slot it is the same engine call plus a no-op
    // write. Zone B, so fixed on Android too (`AppInputDispatcher.handleGenericInput`), per §4's rule.
    apply_caller_eq_slot_change(slot);
}

}  // namespace pt::ui
