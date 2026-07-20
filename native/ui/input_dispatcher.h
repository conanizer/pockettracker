#pragma once

// ─── THE INPUT DISPATCHER ────────────────────────────────────────────────────────────────────────
//
// The C++ twin of input/AppInputDispatcher.kt — every button, every combo, one place. It replaces
// the `handle_button` stand-in the Phase-3 S1/S2 shell carried, and keeps everything that was under
// it: `cursor_context()` still answers what the cursor is on, and the generic `on_a` / `on_b` /
// `on_a_left` / `on_a_right` / `on_a_b` handlers still turn a button into an `InputAction` without
// ever asking which screen is up (ui/cursor.h). What lands here is what those five could not do —
// selection, the clipboard, item cycling, cloning, the FX helper, the note preview.
//
// ── SCOPE: every screen the app has ──────────────────────────────────────────────────────────────
//
// SONG, CHAIN, PHRASE, TABLE, GROOVE, INSTRUMENT, INST.POOL, MODS (S4), MIXER, EFFECTS (S5), the FILE
// BROWSER with the QWERTY KEYBOARD overlay behind it (S6a), the SAMPLE EDITOR (S6b), PROJECT and
// SETTINGS with the confirm dialog (S7) — and, since S8, the EQ EDITOR overlay behind all five of the
// EQ cells. Nothing in the Kotlin dispatcher is now unported except the THEME editor.
//
// ── ⚠️ THE MODAL RULE, which arrives with S6a and grows a third member in S8 ─────────────────────
//
// The QWERTY keyboard is the app's first true modal, the FILE BROWSER is the first full-screen popup,
// the CONFIRM DIALOG (S7) is the topmost, and the EQ EDITOR (S8) is the first PARTIAL one. Each OWNS
// THE BUTTONS while it is up, and every handler below therefore opens with the same questions in the
// same order — confirm, then keyboard, then the EQ overlay, then the browser, then the screen — before
// it does anything else. That order is the specification: the keyboard can be open ON TOP of the
// browser (SELECT+A to rename a file), and a D-pad press there must move the KEY cursor, not the file
// cursor.
//
// ⚠️ **The EQ editor is PARTIAL, and that is a design decision rather than an oversight.** It swallows
// the D-pad, A, B and SELECT, but it lets START through to the screen underneath — because START on
// INSTRUMENT is an AUDITION, and sweeping a band across a note you can hear is the entire reason the
// screen exists. Kotlin is explicit about it (`stopActivePreview` counts the EQ editor as a preview
// screen when it was opened over an instrument, so its band edits "sweep a held preview live").
// Every other modal in the app swallows everything.
//
// Kotlin enforces this the same way (an `if (qwertyKeyboardState.isOpen) { … ; return }` at the top of
// each handler) and its own comment states the rule the hard way: "every new show*Dialog-style modal
// state MUST be added to this predicate". A modal that one handler forgets is a button that does the
// wrong thing exactly once — and that is a bug nobody reports, because it looks like a mis-press.
// ⚠️ S8 found exactly that bug, ON ANDROID: `handleBUp`/`handleBDown` never got the EQ guard, so B+UP
// with the editor open over INST.POOL pages the pool cursor 16 slots underneath it. See on_b_up().
//
// ── THE MAPPER IS SPLIT IN TWO, AND ONLY ONE HALF IS THE SHELL'S ─────────────────────────────────
//
// Android's chain is InputMapper → ButtonHandlers → AppInputDispatcher: the mapper owns the physical
// keys, the held-modifier state and the key repeat, and calls a NAMED handler. The split is kept —
// the methods below are the ButtonHandlers — but Kotlin's one `InputMapper` class answers to TWO
// files here, and the boundary between them is the only platform seam in the input chain:
//
//   • `shell/sdl-input.h`  — a keycode, a game-controller button, an axis, the key repeat. SDL's.
//   • `ui/button_mapper.h` — the COMBO MATRIX: which named handler a press means. Portable.
//
// ⚠️ **This header used to say there is no `handle(ButtonEvent)` here because "pt-ui must not know
// SDL exists (a `ButtonEvent` is an SDL-side type)". The rule is right and the parenthesis was
// wrong** — `ButtonEvent` never named an SDL type, it was merely DECLARED in a header that included
// `<SDL.h>`, and convergence C0.1 moved it to `ui/buttons.h` where that accident cannot be mistaken
// for a design again. The cost of the mistake was real: it kept the combo matrix in `shell/main.cpp`,
// in one copy, where no tool could reach it (`tools/ptmapper` covers it now).
//
// There is still no `handle(ButtonEvent)` on this class, and that part stands on its own feet: the
// matrix is a free function so a headless tool can drive it with a recording stub, and keeping the
// dispatcher's surface at NAMED handlers is what lets ptdispatch drive them one at a time.
//
// ⚠️ **The clock is injected.** `set_now()` once a frame, and `on_l_b()` reads it. The multi-tap
// window is a function of time, and a class that reaches for the clock itself cannot be tested —
// the same reason `SdlInput::handle_event` takes `now_ms` (S1) and `Selection::handle_select_b`
// does (ui/selection.h).

#include "songcore/host.h"
#include "ui/app_state.h"
#include "ui/clipboard.h"
#include "ui/cursor.h"
#include "ui/filesystem.h"
#include "ui/modules/chain_editor.h"
#include "ui/modules/effects_editor.h"
#include "ui/modules/eq_editor.h"
#include "ui/modules/file_browser.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/instrument_editor.h"
#include "ui/modules/instrument_pool.h"
#include "ui/modules/mixer.h"
#include "ui/modules/modulation.h"
#include "ui/modules/phrase_editor.h"
#include "ui/modules/project_editor.h"
#include "ui/modules/qwerty_keyboard.h"
#include "ui/modules/sample_editor.h"
#include "ui/modules/settings_editor.h"
#include "ui/modules/song_editor.h"
#include "ui/modules/table_editor.h"
#include "ui/project_actions.h"

#include <functional>

#include <string>
#include <utility>
#include <vector>

namespace pt::ui {

class InputDispatcher {
  public:
    /**
     * `fs` is a REFERENCE, and there is no null-FileSystem path — unlike the engine, which
     * `SongcoreHost` null-checks everywhere so the whole editing layer can be driven with no audio
     * device. A file browser with no filesystem is not a degraded browser, it is an empty box; a tool
     * that wants one without touching the user's disk points it at a temp directory instead, which is
     * what `tools/ptdispatch` does.
     */
    InputDispatcher(AppState& state, songcore::SongcoreHost& host, FileSystem& fs)
        : s_(state), host_(host), fs_(fs) {}

    /**
     * The frame's clock reading — call it once per frame, before the events.
     *
     * It feeds the L+B multi-tap window, and (since S6b) it RUNS ANY DEFERRED WORK THAT IS NOW DUE.
     * There are TWO such jobs now, and both are deadlines because Kotlin arranges them with coroutines
     * and there are none here:
     *
     *   • the sample editor's audition restore (S6b) — the preview writes the SELECTION into the
     *     instrument's sample window, and has to put the real one back once the voice has actually
     *     triggered, which is 100 frames after the note is scheduled rather than when it is scheduled;
     *   • the crash-recovery AUTOSAVE's 3 s debounce (S10) — see `mark_modified`.
     *
     * ⚠️ Which means the clock is INJECTED rather than read, and that is the point: work that fires on
     * a deadline cannot be tested by a tool that cannot move time. `tools/ptdispatch` drives both of
     * these with a fake clock — the same reason `SdlInput::handle_event` takes `now_ms` (S1) and
     * `Selection::handle_select_b` does.
     */
    void set_now(long long now_ms);

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // THE LIFECYCLE (Phase 3 S10) — the three things the SHELL has to say
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    //
    // Everything else about the autosave is internal: `mark_modified` arms it, `set_now` fires it, and
    // SAVE / LOAD / NEW / EXIT clear it. These three are the boundary, because only the shell knows
    // where the media is, when the app started, and when it is being taken away.

    /**
     * Where a project's RELATIVE sample paths resolve. Call once, at start-up.
     *
     * ⚠️ It is the SESSION's media dir, and the autosave is why it has to be remembered rather than
     * guessed. `resolve_media_path` leaves an ABSOLUTE path alone and joins a RELATIVE one onto this —
     * and every sample loaded through the browser is absolute, so in normal use this changes nothing.
     * But a *portable* project stores its media relative (every golden does, and so will anything the
     * Linux build ships), and recovering one of those against the wrong folder loads no samples at all:
     * the song comes back looking perfectly correct and playing silence, which is the worst way for a
     * recovery to fail. The shell hands in what it handed `load_media` at boot.
     */
    void set_media_base_dir(std::string dir) { mediaBaseDir_ = std::move(dir); }

    /** What `boot_recovery()` actually did. Four outcomes, and each one means exactly one thing. */
    enum class BootRecovery {
        NONE,      // no autosave — the last session ended cleanly. The overwhelmingly common case.
        ASKED,     // RESUME=ASK: the RECOVER WORK? dialog is up, and nothing is decided yet.
        RESTORED,  // RESUME=AUTO: the document is back, and DIRTY.
        DROPPED,   // it would not parse. The file is gone, so it cannot be offered again.
    };

    /**
     * START-UP: an autosave that survived to launch means the last session did not end cleanly.
     *
     * SETTINGS → RESUME decides what happens next — ASK raises the RECOVER WORK? dialog, AUTO restores
     * it in silence — so the shell must call this AFTER loading settings.json, and after its own
     * `push_params()`, because a recovery re-pushes everything anyway and doing it twice is only slow.
     *
     * ⚠️ The return value is an ENUM and not a bool, and S10 changed it to one after watching the shell
     * print a lie. `bool found` collapsed RESTORED and DROPPED into the same answer, so a CORRUPT
     * autosave under RESUME=AUTO — dropped, correctly, with nothing recovered — reported itself on the
     * console as *"restored silently"*. A boot diagnostic that misreports the outcome is worse than no
     * diagnostic at all: it is the thing you will trust at 2 a.m. on a handheld with no screen.
     */
    BootRecovery boot_recovery();

    /**
     * THE KILL. Flush the autosave NOW, synchronously, if there is unsaved work — and then return,
     * because the caller is on its way out of the process.
     *
     * ⚠️ **CALL THIS FROM THE FRAME LOOP'S EXIT PATH, NEVER FROM A SIGNAL HANDLER.** It serializes
     * ~440 KB of JSON and writes a file: `malloc`, `<filesystem>`, `ofstream` — not one of them is
     * async-signal-safe, and a SIGTERM arriving while the main thread happens to be inside `malloc`
     * would deadlock the handler on the heap lock. The app would then hang instead of saving, and the
     * launcher's SIGKILL would arrive a second later: the autosave would fail in precisely the case it
     * exists for. The port plan's "handle SIGTERM → autosave" (§5) is therefore a line to read twice.
     *
     * The shell's handler writes a `volatile sig_atomic_t` and returns; its frame loop reads that flag,
     * leaves, and calls this. `pt-ui` never sees a signal — which is also why this is an ordinary public
     * method and not a callback: `tools/ptdispatch` drives it directly, with no signals and no SDL, and
     * proves the thing that actually loses data if it is wrong.
     */
    void flush_autosave();

    // ── D-pad alone: move the cursor (or drag a selection's edge) ────────────────────────────────
    void on_dpad_up();
    void on_dpad_down();
    void on_dpad_left();
    void on_dpad_right();

    // ── A + D-pad: edit the cell under the cursor ────────────────────────────────────────────────
    // A+UP/DOWN step by one, A+LEFT/RIGHT by the large step (16 for a hex byte, an octave for a
    // note). On an FX-TYPE column, A+UP/DOWN instead open the FX helper.
    void on_a_up();
    void on_a_down();
    void on_a_left();
    void on_a_right();

    /** A+B: delete the cell, or reset it to its default. Over a selection: delete the range. */
    void on_a_b();

    /** A,A (a double-tap on the same cell): insert the next unused chain/phrase. */
    void on_a_a();

    /** Release of A: commits the FX helper's highlighted effect. */
    void on_a_released();

    // ── B + D-pad: which item am I looking at? ───────────────────────────────────────────────────
    // B+LEFT/RIGHT cycle the current phrase / chain / table / groove — without this the shell can
    // only ever edit slot 0 of each. B+UP/DOWN page the SONG screen.
    void on_b_left();
    void on_b_right();
    void on_b_up();
    void on_b_down();

    // ── R + D-pad: move between screens ──────────────────────────────────────────────────────────
    void on_r_up();
    void on_r_down();
    void on_r_left();
    void on_r_right();

    // ── L: selection and the clipboard ───────────────────────────────────────────────────────────
    /** L+B: enter selection, then widen it CELL → ROW → SCREEN on each tap inside 500 ms. */
    void on_l_b();
    /** L+A: cut (inside a selection) or paste (outside one). */
    void on_l_a();
    /** L+R: leave selection mode. */
    void on_l_r();
    /** L+B+A: deep-clone the chain/phrase under the cursor into free slots. */
    void on_l_b_a();

    // ── SELECT + … : the file browser's verbs (S6a) ──────────────────────────────────────────────
    // Three chords that exist only on the browser, and each opens something: the keyboard (twice) or
    // a confirm. They are its whole file-management vocabulary — Android has no others.
    /** SELECT+A: rename the file/folder under the cursor (opens the keyboard). */
    void on_select_a();
    /** SELECT+B: delete it — arms the "A=YES B=NO" confirm, never deletes on the press itself. */
    void on_select_b();
    /** SELECT+R: create a folder here (opens the keyboard). */
    void on_select_r();

    // ── The plain buttons ────────────────────────────────────────────────────────────────────────
    /** B inside a selection COPIES it and exits — the tracker's copy gesture. */
    void on_button_b();
    void on_button_a();
    void on_select();
    /** START: play/stop. What it plays depends on the screen you are on. */
    void on_start();

    /**
     * "Press any button to silence the audition." The mapper calls this on every plain press; the
     * dispatcher decides whether the current screen even HAS a preview to stop. Previews live on
     * their own voice, so this never touches song playback.
     */
    void on_stop_preview();

    /**
     * ⚠️ **Asked by the MAPPER on every plain A press**: is the cursor on a cell whose A OPENS
     * something, and whose A+DPAD/A+B means something ELSE? If so the mapper holds the press until A is
     * RELEASED, and cancels it outright if any A-combo fires in between.
     *
     * Without it, holding A on such a cell to reset it with A+B would open the sub-screen first and the
     * combo would land on top of it. Kotlin's `InputMapper` asks the same question (`deferAToRelease()`
     * → `openSubScreenAtCursor(peek = true)`) and keeps the same `aPressedAlone` latch.
     */
    bool defer_a_to_release() const;

    /**
     * ⚠️ **Asked by the MAPPER on every plain B press** (S8), and the exact mirror of the above: is B a
     * CLOSE rather than a modifier? True while the EQ editor is open, and only then.
     *
     * The editor's slot is changed with B+LEFT/RIGHT, and B is also what closes it. Fire the close on the
     * PRESS and the cycle is unreachable — you would be back on the mixer before LEFT ever arrived. So the
     * close is held until B comes back UP, and cancelled outright if a B-combo fires in between. Kotlin
     * carries the same latch (`deferBToRelease` → `bPressedAlone`) for the same one screen.
     */
    bool defer_b_to_release() const;

    /** The clipboard, for the top-strip readout ("PHR:2x3"). */
    const Clipboard& clipboard() const { return clip_; }

    // ── Opening the sub-screens (the shell needs these too, for its start-up state) ──────────────

    /**
     * Show the browser, filtered, in `directory`, and remember what A will do with the pick.
     * `previousScreen` is captured here — it is where B returns to.
     */
    void open_file_browser(AppState::BrowserPurpose purpose, const std::string& directory,
                           const std::vector<std::string>& extensions);

    /**
     * The two things a render needs that only the SHELL can do (S7).
     *
     * ⚠️ THE RENDER IS SYNCHRONOUS. Android hands it to a coroutine because Compose would ANR; a
     * single-threaded frame loop has nothing to hand it to, so it simply stops and renders — which is
     * both simpler and SAFER, because the audio callback is the one thing that must not be reading
     * engine state while an offline render is writing it.
     *
     *   `suspend_audio(true/false)` — the shell pauses its SDL audio device for the duration. Kotlin
     *   only stops PLAYBACK (its Oboe stream stays open and idle); the shell can do better, and a
     *   paused device is a guarantee rather than a hope.
     *
     *   `repaint()` — called as progress moves. It is what draws the "43%" on the EXPORT row, and it
     *   is why the percentage is a real readout rather than a decoration.
     *
     * Both may be empty. `tools/ptdispatch` renders a real WAV with neither an audio device nor a
     * window, which is exactly the proof that neither is load-bearing.
     */
    struct RenderHooks {
        std::function<void(bool)> suspend_audio;
        std::function<void()>     repaint;
    };
    void set_render_hooks(RenderHooks hooks) { render_ = std::move(hooks); }

  private:
    AppState&               s_;
    songcore::SongcoreHost& host_;
    FileSystem&             fs_;
    long long               now_ms_ = 0;
    RenderHooks             render_{};

    /** See set_media_base_dir. Empty means "relative paths stay relative" (resolve_media_path). */
    std::string mediaBaseDir_{};

    // ── The autosave's DEBOUNCE (S10) ────────────────────────────────────────────────────────────
    //
    // Kotlin's is a `LaunchedEffect(projectVersion)` that DELAYS 3 s and is re-keyed — and therefore
    // CANCELLED and restarted — by the next edit, so a burst of typing coalesces into one write. That
    // is a deadline wearing a coroutine's clothes, and without coroutines it is just a deadline.
    //
    // ⚠️ RE-ARMED on every edit, never merely armed once: the write must land 3 s after the LAST
    // keystroke, not 3 s after the first. Arm-if-not-armed would fire mid-burst, on a device where a
    // held A+UP produces an edit every 100 ms — ten writes of ~440 KB a second onto an SD card.
    bool      autosavePending_ = false;
    long long autosaveDueAtMs_ = 0;

    /** 3 s, and it is Kotlin's own constant (MainActivity.AUTOSAVE_DEBOUNCE_MS). */
    static constexpr long long AUTOSAVE_DEBOUNCE_MS = 3000;

    /** The deadline, checked once a frame by set_now(). */
    void run_due_autosave();

    // ── The status line's auto-dismiss (parity audit, finding 5) ─────────────────────────────────
    //
    // MainActivity.kt:734–747 clears the status 5 s after it is SET — a LaunchedEffect keyed on the
    // VALUE, so re-setting an identical message does not restart the delay. The port's "setter" is
    // 22 plain assignments, so the dismissal is derived from the DATA instead: set_now watches the
    // field for CHANGES (the settingsDirty lesson — a stamp 22 call sites must remember is one they
    // will forget once, and the 23rd site gets this for free). Detection therefore lands on the
    // frame AFTER the set — one ~16 ms tick late on the shell, invisible on a 5 s timer, and
    // ptdispatch §34 encodes the tick explicitly.
    std::string statusLastSeen_{};
    long long   statusDismissAtMs_ = 0;

    /** 5 s — Kotlin's own delay (MainActivity's two status LaunchedEffects). */
    static constexpr long long STATUS_DISMISS_MS = 5000;

    /** The watcher and the deadline, both run once a frame by set_now(). */
    void run_due_status_dismiss();

    // ── The INSTRUMENT-entry param push (parity audit, finding 8) ────────────────────────────────
    //
    // Android's currentScreen SETTER calls `instrumentController.syncToLastEdited(project)` on every
    // entry into INSTRUMENT (TrackerController.kt:46–48), which ends in
    // `audioEngine.updateInstrumentPlaybackParams` — a belt-and-braces push of engine state the
    // event path never carries (the S4 family). The port's screen changes have no single setter
    // (go_to_screen from R+DPAD, bare assignments on overlay close), so the push is derived from the
    // DATA: set_now watches `currentScreen` and pushes on the frame after INSTRUMENT is entered, by
    // any route — including routes not written yet.
    ScreenType lastScreenSeen_ = ScreenType::SONG;   // the boot screen — see app_state.h

    /** The watcher, run once a frame by set_now(). */
    void run_instrument_entry_push();

    /**
     * Load the autosave into the live document — and LEAVE IT DIRTY.
     *
     * ⚠️ **The dirty flag is the whole difference between this and a LOAD, and it is deliberate on both
     * platforms** (`TrackerController.recoverFromAutosave`: "leaving it DIRTY … so the user is nudged to
     * Save it under a real name"). Recovered work is not *stored* work — it exists in one file the user
     * cannot see, has never named, and did not ask for. Aligning the versions here would tell them the
     * song is safe when the only copy of it is the crash file.
     *
     * ⚠️ And for the same reason it does NOT clear the autosave. The recovered document is still the
     * only copy; deleting the file that holds it, at the exact moment the user has proved they are
     * capable of losing the session, would be the one deletion in the app that can destroy real work.
     */
    bool recover_from_autosave();

    Clipboard clip_{};

    SongEditorModule       song_{};
    ChainEditorModule      chain_{};
    PhraseEditorModule     phrase_{};
    TableModule            table_{};
    GrooveModule           groove_{};
    InstrumentEditorModule instrument_{};
    InstrumentPoolModule   pool_{};
    ModulationModule       mods_{};
    MixerModule            mixer_{};
    EffectModule           effects_{};
    ProjectModule          project_{};
    SettingsModule         settings_{};
    EqModule               eq_{};   // stateful: it caches its response curve — see eq_editor.h

    /**
     * A,A is a DOUBLE-TAP, and a double-tap is only a double-tap if the cursor has not moved between
     * the presses. Kotlin records where the first A landed and compares; anything else — a press on
     * one cell and a press on the next — is two separate presses.
     */
    bool         hasInsertPos_ = false;
    ScreenType   insertScreen_ = ScreenType::PHRASE;
    int          insertRow_    = 0;
    int          insertCol_    = 0;

    // ── The spine (AppInputDispatcher's own private shape, kept) ─────────────────────────────────

    /** "What is under the cursor?" — the ONE place that asks which screen is up. */
    CursorContext cursor_context() const;

    /** Apply a resolved action to the live document. True if anything changed. */
    bool apply_edit(const InputAction& action);

    /**
     * `handleGenericInput`: context → action → mutate → echo to the engine. `fn` is one of the five
     * generic handlers, and this function never learns which.
     */
    void generic_input(InputAction (*fn)(const CursorContext&));

    /**
     * `handleSelectionOrSingleIncrement`: the same, but applied to EVERY ROW of a selection when one
     * is up. A+UP over a 4-row selection increments four cells.
     */
    void selection_or_single(InputAction (*fn)(const CursorContext&));

    /** `handleDPadNavigation`: move the cursor, or drag the selection's active edge. */
    void dpad_nav(const char* direction);

    /**
     * `AppInputDispatcher.syncLastEditedOnScreenSwitch` (:2760) — the R+LEFT/R+RIGHT deep-link.
     * CAPTURE the ref under the departing screen's cursor into lastEdited*, then APPLY lastEdited*
     * to the arriving screen's current*. ⚠️ The two HORIZONTAL moves only, and only when the screen
     * actually changes: Kotlin's handleRUp/handleRDown do plain save/restore + selection exit, and
     * syncing them too would diverge the other way (parity audit, finding 2's scope trap).
     */
    void sync_last_edited_on_screen_switch(ScreenType from, ScreenType to);

    /** An edit happened: tell the sequencer, so a note already scheduled past the cursor is redone. */
    void mark_modified(bool table_touched = false);

    /**
     * The first HALF of mark_modified — dirty the document and (re-)arm the crash autosave — split
     * out for the one caller that must not take the second half: the EQ editor's band path, whose
     * right-sized engine push is two calls, not mark_modified's wholesale push_globals. On Android
     * the two halves cannot come apart (EVERY projectVersion++ re-keys the autosave LaunchedEffect,
     * MainActivity.kt:754); here a bare `projectVersion++` is a dirty flag with no crash protection
     * behind it — P4d's shape, third body (parity audit, finding 7).
     */
    void mark_dirty_and_arm_autosave();

    /** Play the note an edit just wrote (SETTINGS "NOTE PREVIEW"). */
    void preview_edited_note();

    /** True on the three screens that edit an INSTRUMENT rather than the arrangement. */
    bool on_instrument_screen() const;

    /** True on the two that edit the GLOBALS — the mixer, the master bus, the send buses. */
    bool on_globals_screen() const;

    /**
     * INSTRUMENT row 0: A+UP/DOWN toggles SAMPLER↔SOUNDFONT.
     *
     * ⚠️ Kotlin puts a CONFIRM DIALOG in front of this whenever a source is already loaded, because the
     * toggle DROPS that source (a sampler has no use for an .sf2 and vice versa). There is no dialog
     * system in the port yet, so the guard is enforced the only other way that is honest: the toggle is
     * REFUSED on a slot that has a source, with a status message saying so. Clear the slot (A+B in the
     * pool) and it toggles. That is stricter than Android, never destructive, and it lands its proper
     * confirm dialog with the rest of the modal system.
     */
    /** A+UP/DOWN on the TYPE cell: switch outright if the slot is empty, else ASK (S7's dialog). */
    void request_instrument_type_toggle();
    void toggle_instrument_type();

    /** True when the cursor is on INSTRUMENT's TYPE cell — where A+UP/DOWN toggles rather than steps. */
    bool on_instrument_type_cell() const;

    // ── The cursor's live row/column for the screen we are on ────────────────────────────────────
    int  cursor_row() const;
    int  cursor_column() const;
    void set_cursor_row(int row);

    /** The rightmost selectable column, per screen — the selection's ROW scope needs it. */
    int  max_selection_column() const;
    /** 255 on SONG (a selection spans the document, not the viewport); 15 everywhere else. */
    int  max_selection_row() const;

    // ── FX helper ───────────────────────────────────────────────────────────────────────────────
    /** True when the cursor is on an FX-TYPE column (PHRASE 4/6/8, TABLE 3/5/7). */
    bool on_fx_type_column() const;
    /** The index into EFFECT_TYPES the cursor's FX column currently holds. */
    int  current_fx_type_index() const;
    /** Write an effect CODE into the FX column under the cursor. */
    void apply_fx_type_change(int effect_code);

    // ── A,A / L+B+A helpers ─────────────────────────────────────────────────────────────────────
    void cycle_current_item(int delta);

    // ── The modal guards (see the ⚠️ THE MODAL RULE note at the top of this file) ────────────────
    bool qwerty_open() const { return s_.qwerty.isOpen; }
    bool on_browser() const { return s_.currentScreen == ScreenType::FILE_BROWSER; }

    /**
     * The confirm dialog owns EVERY button but A and B. It is the topmost modal — drawn last, over
     * even the keyboard — so it is checked FIRST, ahead of the keyboard and the browser both, and it
     * is the one guard that simply RETURNS rather than redirecting.
     *
     * ⚠️ The check appears at the top of all 28 handlers, and the exceptions are exactly three:
     * `on_button_a` and `on_button_b`, which ARE the answer, and `on_stop_preview`, which silences a
     * ringing audition (a dialog raised over an INSTRUMENT audition must not leave the note hanging —
     * and silencing a note is not an edit). ptdispatch asserts the rest: with a confirm up, every
     * other button is inert. That assertion is the real guarantee here, not the code shape — Kotlin's
     * own comment on this predicate warns that "every new show*Dialog-style modal state MUST be added"
     * to it, and a rule that has to be remembered 28 times is a rule that will eventually be forgotten
     * once.
     */
    bool confirm_open() const { return s_.confirm.is_open(); }

    /** A on the dialog: do the thing it asked about. */
    void confirm_accept();

    /**
     * B on the dialog: don't.
     *
     * ⚠️ **NOT A PURE CLOSE ANY MORE — S10 is where that stopped being true.** For five of the six
     * questions NO means "the world is exactly as it was", and closing the box is the whole of it. For
     * RECOVER it means *discard my unsaved work*, and that has to DELETE the autosave: leave the file
     * on disk and the same prompt comes back on the next launch, and the next, asking about work the
     * user has already refused once — which is how a safety prompt teaches people to dismiss it.
     *
     * ptdispatch pins both halves: the other five leave the filesystem untouched, and this one does not.
     */
    void confirm_cancel();

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // THE EQ EDITOR (Phase 3 S8)
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    //
    // The port's third modal, and the first PARTIAL one: it owns the D-pad, A, A+DPAD, A+B, B, B+DPAD and
    // SELECT, and lets START through on purpose (the screen underneath keeps its audition, which is the
    // only way to HEAR what a band is doing while you dial it).
    //
    // ⚠️ It is an OVERLAY, so `currentScreen` still names the screen underneath — which means every
    // handler that reaches for the cursor MUST ask `eq_open()` first, or it will edit a mixer fader while
    // the user is dialling a bell curve. That is why `generic_input()` opens with the EQ arm rather than
    // each caller carrying one.

    bool eq_open() const { return s_.eq.isOpen; }

    /** Raise the editor on `slot`, remembering WHICH cell asked (the slot cycle has to write back). */
    void open_eq_editor(int slot, EqCallerContext caller);
    void close_eq_editor() { s_.eq = EqEditorState{}; }

    /** The D-pad: LEFT/RIGHT change band, UP/DOWN change param. Both CLAMP; neither wraps. */
    void eq_move_cursor(int d_band, int d_param);

    /**
     * B+LEFT/RIGHT: step the slot 0..127 (CLAMPED — it is a bank index, not a ring) and re-point the
     * cell that opened the editor at the new slot. Five different project fields, one gesture; the
     * caller tag is the only thing that knows which.
     */
    void apply_caller_eq_slot_change(int new_slot);

    /**
     * ⚠️ After EVERY band nudge, and it is two engine calls, not one. See SongcoreHost::set_eq_band:
     * writing the bank changes nothing anyone is listening to until the consumer is re-handed the slot.
     */
    void push_eq_band_to_engine();

    /**
     * Kotlin's `openSubScreenAtCursor(peek)` — the ONE list of cells whose plain A opens something, and
     * the single source of truth for both halves of that claim: what A DOES (peek = false) and what the
     * mapper must DEFER (peek = true).
     *
     * ⚠️ One function rather than two lists, deliberately. Split them and a cell can drift into being
     * openable-but-not-deferred (its A+DPAD gets pre-empted by the open) or deferred-but-not-openable
     * (its A does nothing at all, and the deferral silently eats the press). Both are bugs that look
     * like a mis-press, and neither would show up in any golden — which is precisely the class of bug
     * S7's one-state confirm dialog was about.
     */
    bool open_sub_screen_at_cursor(bool peek);

    /** Is ANY modal already up? Then no cell "opens a sub-screen" — the modal owns the button. */
    bool any_modal_open() const {
        return confirm_open() || qwerty_open() || eq_open() || theme_open();
    }

    // ── The THEME EDITOR (Phase 3 S9) ───────────────────────────────────────────────────────────
    //
    // The port's fourth modal and its second PARTIAL one — it owns the D-pad, A, A+DPAD, B, B+DPAD and
    // SELECT, and lets START through to the transport (ui/app_state.h says why: half the colours it
    // edits only exist while the song plays).
    //
    // ⚠️ IT IS RAISED FROM SETTINGS AND `currentScreen` STAYS `SETTINGS` — the same overlay hazard the
    // EQ editor has, and the same rule: ask `theme_open()` before reaching for the screen's cursor, or
    // A+UP will cycle the VISUALIZER row underneath while the user is dialling a colour.
    //
    // ⚠️ AND THE KEYBOARD OPENS ON TOP OF IT. SAVE raises the QWERTY overlay WITHOUT closing the editor
    // (LOAD, by contrast, closes it and re-opens it when the file lands), so `qwerty_open()` must be
    // tested BEFORE `theme_open()` in every handler — THE MODAL RULE from S6a, and this is the second
    // place in the app where two modals are genuinely stacked.

    bool theme_open() const { return s_.themeEditor.isOpen; }

    void open_theme_editor()  { s_.themeEditor = ThemeEditorState{}; s_.themeEditor.isOpen = true; }
    void close_theme_editor() { s_.themeEditor = ThemeEditorState{}; }

    /** The D-pad: UP/DOWN walk the 18 rows (WRAPPING), LEFT/RIGHT the 3 channels (WRAPPING). */
    void theme_move_cursor(int d_row, int d_channel);

    /** A on the THEME row: column 1 = SAVE (raises the keyboard), column 2 = LOAD (raises the browser). */
    void theme_row_action();

    /**
     * Apply the typed name and write `<dir>/<name>.ptt`. The QWERTY's THEME_SAVE arm.
     *
     * ⚠️ `dir` is a PARAMETER and not read from `s_.qwerty.contextExtra`, which would be the obvious
     * thing and is wrong: `qwerty_apply()` copies the keyboard state by value and CLEARS `s_.qwerty`
     * before it dispatches, so by the time any arm runs, the live keyboard is already gone. Reading it
     * here wrote the theme to the filesystem ROOT instead of the Themes folder — silently, because
     * `write_file` creates its parent directories and cheerfully succeeded. Every other arm takes
     * `k.contextExtra`; this one does too now. (Caught by ptdispatch §27 on its first run, which is
     * precisely the join a golden cannot see.)
     */
    void save_theme_as(const std::string& dir, const std::string& typed_text);

    // ── PROJECT + SETTINGS: the buttons (Phase 3 S7) ────────────────────────────────────────────
    /** A on PROJECT: SAVE / LOAD / NEW / MIX / STEMS / SEQ / INST / SETTINGS> / EXIT. */
    void project_action();
    /** A on SETTINGS: only THEME (row 9) and TEMPLATE (row 10) do anything — the rest are A+DPAD. */
    void settings_action();

    /** NEW, and the engine sync a fresh project needs (SongcoreHost::new_project). */
    void start_new_project();

    /** Slot 0 across the board, and no selection. Shared by NEW and LOAD. */
    void reset_editing_context();

    /** A .ptp replaced the document: clean, no selection, browser closed. */
    void load_project_done(const std::string& path);

    /** EXPORT. Renders SYNCHRONOUSLY; `on_render_progress_` repaints the frame from inside it. */
    void export_song(bool stems);

    // ── The FILE BROWSER ────────────────────────────────────────────────────────────────────────
    /** Leave the browser for the screen it was opened from, dropping the audition on the way out. */
    void close_file_browser();
    /** Re-list the current directory in place — after a rename, a create, a delete or a paste. */
    void refresh_browser();
    /** R+UP / R+DOWN: step through the six sort modes, rebuilding the listing under the cursor. */
    void browser_cycle_sort(int delta);
    /** Move the cursor, keeping the 19-row window around it. `page` = the D-pad's LEFT/RIGHT jump. */
    void browser_move_cursor(int delta, bool page);
    /** A: open a folder, go up, or LOAD the file — which depends on `browserPurpose`. */
    void browser_confirm();
    /** The paths inside the live selection, minus the ".." entry, which is not a file. */
    std::vector<std::string> browser_selected_paths() const;
    /** Copy or move the clipboard into the directory on screen, de-duplicating names. */
    void browser_paste();

    // ── The QWERTY keyboard ─────────────────────────────────────────────────────────────────────
    void open_qwerty(QwertyContext context, const std::string& initial_text,
                     const std::string& field_label, const std::string& context_extra,
                     int max_length = 20, bool clear_on_first_b = false);
    /** APPLY — what START, and A on the APPLY button, do. Acts on the context it was opened with. */
    void qwerty_apply();
    /** ABORT — SELECT, and A on the ABORT button. Discards the text. */
    void qwerty_cancel() { s_.qwerty = QwertyKeyboardState{}; }

    // ── INSTRUMENT's four buttons ────────────────────────────────────────────────────────────────
    /**
     * A on one of the four cells that open something: the preset LOAD/SAVE on row 0, and the source
     * LOAD and EDIT on the SOURCE row. Returns true if it handled the press.
     */
    bool instrument_open_at_cursor();

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // THE SAMPLE EDITOR (Phase 3 S6b)
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    //
    // Kotlin scatters this across ten regions of `AppInputDispatcher` — a `SAMPLE_EDITOR ->` arm inside
    // handleButtonA, handleButtonB, handleSelect, handleStart, all four D-pad handlers, all four A+D-pad
    // handlers, handleAB, and the qwerty commit. Gathered here, because they are one screen.

    bool on_sample_editor() const { return s_.currentScreen == ScreenType::SAMPLE_EDITOR; }

    /** Rows 3..8: the D-pad DRAGS the selection instead of moving a cursor. */
    bool on_sample_selection_row() const {
        return on_sample_editor() && s_.sampleEditor.cursorRow >= 3 && s_.sampleEditor.cursorRow <= 8;
    }

    /** INSTRUMENT's EDIT button (row 5, col 3). Samplers only — an SF2 has no waveform to cut. */
    void open_sample_editor();
    /** B on an unmodified sample: free the undo, drop the scratch slots, go back. */
    void close_sample_editor();

    /**
     * Build the editor's session from the sample the engine is holding: its length, its rate, its
     * waveform, the selection its instrument's start/end points describe, and the slice markers its file
     * brought with it. Separate from `open_sample_editor` because the editor's own LOAD button re-enters
     * on DIFFERENT audio — everything the previous session knew is then false, and rebuilding is safer
     * than patching. (It must not touch `previousScreen`: that is the editor's return target, and the
     * browser it just came back from is not it.)
     */
    void init_sample_editor_state();

    /** A+DPAD on rows 3..8, and the whole reason those rows have no CursorContext. */
    void nudge_selection_edge(int64_t delta);

    /** RATE (row 1, col 2) re-decimates the buffer — the one row-1 edit that changes the AUDIO. */
    void apply_sample_rate_mode();

    /** A on rows 13/14/16/18/19 — the twelve ops, the FX apply, the name, and the save buttons. */
    void sample_editor_confirm();

    /**
     * Bake the PENDING pitch shift destructively into the buffer and rescale everything measured in
     * frames. Shared by all three save paths — a sample must be saved as it SOUNDS, and until this runs
     * the shift is only a number on the screen.
     */
    void bake_pending_pitch();

    /** The slice boundaries as WAV cue points: the markers, or DIVIDE's N−1 computed cuts. */
    std::vector<int> compute_slice_cue_points() const;

    /** Re-read the length and the waveform after an op. `reset_selection` re-selects the whole sample. */
    void refresh_sample_view(bool reset_selection);

    /** SAVE / SAVE-AS / OVERWRITE all end here: write the WAV, adopt it, and leave the editor. */
    void save_sample_to(const std::string& path, bool adopt_name);

    /** CHOP: every slice out to `Samples/Chops/<name>/`, as its own WAV. */
    void sample_editor_chop();

    /** The slice (start, end) pairs the current method defines — CHOP's work list. */
    std::vector<std::pair<int64_t, int64_t>> current_slices() const;

    /**
     * ⚠️ The deferred half of the audition (see set_now). The preview writes the SELECTION into the
     * instrument's sample window because that is the only channel the engine has for a voice's window —
     * and the voice reads it when it TRIGGERS, 100 frames later. So the real window cannot go back
     * until then, and these three fields are what remembers to do it.
     *
     * A second START inside the window runs it IMMEDIATELY, before capturing anything: otherwise the
     * second preview would save the FIRST preview's window as the instrument's real one, and the user's
     * start/end points would be silently replaced by whatever they last auditioned.
     */
    void run_due_sample_preview_restore(bool force = false);

    bool      previewRestorePending_ = false;
    long long previewRestoreAtMs_    = 0;
    int       previewRestoreInst_    = 0;
    int       previewSavedStart_     = 0;
    int       previewSavedEnd_       = 255;

    SampleEditorModule sample_{};
};

}  // namespace pt::ui
