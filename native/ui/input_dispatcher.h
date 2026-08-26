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
// SONG, CHAIN, PHRASE, TABLE, GROOVE, INSTRUMENT, INST.POOL, MODS, MIXER, EFFECTS, the FILE BROWSER
// with the QWERTY KEYBOARD overlay behind it, the SAMPLE EDITOR, PROJECT and SETTINGS with the confirm
// dialog, the EQ EDITOR overlay behind all five EQ cells, the MIDI screen, and the THEME editor.
// Every screen the app has is dispatched from here; there is no second input path.
//
// ── ⚠️ THE MODAL RULE ────────────────────────────────────────────────────────────────────────────
//
// The CONFIRM DIALOG is the topmost layer, the QWERTY keyboard is a true modal, the THEME and EQ
// editors are PARTIAL ones, the FX HELPER is a picker held up by A, and the FILE BROWSER is a
// full-screen popup. Each OWNS THE BUTTONS while it is up, and the order in which a press is offered
// to them is the specification, not an implementation detail: the keyboard can be open ON TOP of the
// browser (SELECT+A to rename a file) or ON TOP of the theme editor (its SAVE), and a D-pad press
// there must move the KEY cursor, not the one underneath.
//
// ⭐ **That order is written exactly once, in `top_overlay()`**, and every handler below asks
// `overlay_swallows()` with the set of layers it answers for rather than re-deriving the stack. See
// the block comment on those two, which is where the rule's whole mechanism now lives.
//
// ⚠️ **The EQ editor is PARTIAL, and that is a design decision rather than an oversight.** It swallows
// the D-pad, A, B and SELECT, but it lets START through to the screen underneath — because START on
// INSTRUMENT is an AUDITION, and sweeping a band across a note you can hear is the entire reason the
// screen exists. Kotlin is explicit about it (`stopActivePreview` counts the EQ editor as a preview
// screen when it was opened over an instrument, so its band edits "sweep a held preview live").
// Every other modal in the app swallows everything.
//
// ⚠️ **A modal that one handler forgets is a button that does the wrong thing exactly once** — and
// that is a bug nobody reports, because it looks like a mis-press. Android carries a live one, found
// by porting: `handleBUp`/`handleBDown` never got the EQ guard, so B+UP with the editor open over
// INST.POOL pages the pool cursor 16 slots underneath it. See on_b_up(). That is the failure a
// per-handler subset invites and the reason the stack is derived here rather than remembered.
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
#include "ui/song_pointer.h"     // NAV = SONG — the ctor clamps the pointer onto a real cell
#include "ui/filesystem.h"
#include "ui/modules/chain_editor.h"
#include "ui/modules/effects_editor.h"
#include "ui/modules/eq_editor.h"
#include "ui/modules/file_browser.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/instrument_editor.h"
#include "ui/modules/instrument_pool.h"
#include "ui/modules/midi_settings.h"
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
        : s_(state), host_(host), fs_(fs) {
        // The layout draws the clipboard's "PHR:2x3" readout beside the selection label and reaches it
        // through AppState — so point AppState at our clipboard here, once, the same way the shell points
        // it at the host's document. `clip_` is a member and outlives every frame this dispatcher drives.
        s_.clipboard = &clip_;

        // …and the same thing for the song-relative pointer, for the launch-time project the shell
        // pushed before this object existed: it is the LOAD path `reset_editing_context` covers, minus
        // the reset. A no-op under NAV = POOL and with no project pointed at yet (ui/song_pointer.h).
        clamp_song_pointer(s_);
    }

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

    /**
     * Is there time-triggered work outstanding that will change what is on SCREEN by itself?
     *
     * ⚠️ **C7 NEEDS THIS AND THE PIXEL COMPARISON CANNOT ANSWER IT.** The shell skips drawing on an
     * idle frame, and the safety net under that decision is a byte-compare of the drawn frame — but a
     * frame that is never drawn is never compared. So anything that changes the SCREEN on a TIMER,
     * with no input at all, is invisible to that net: BOTH status lines auto-dismiss 5 s after they
     * are set (`run_due_status_dismiss`), and without this query a "PROJECT SAVED" — or a "FILE TOO
     * BIG" on the browser's own bar — would sit on a quiescent screen forever, cleared in the state
     * and stale in the pixels.
     *
     * Derived from the DEADLINES themselves rather than from a flag any caller has to set — the same
     * rule that made the dismissal watch the message field instead of trusting its 22 assignment
     * sites. ⚠️ A future timer that alters the screen belongs in this expression, and that is one
     * place, colocated with the deadlines it reads.
     *
     * `autosavePending_` is included though it draws nothing itself: it ends in a status message.
     */
    bool has_pending_timed_work() const {
        return statusDismissAtMs_ != 0 || browserStatusDismissAtMs_ != 0 || autosavePending_;
    }

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
    // A+LEFT/RIGHT step by one, A+UP/DOWN by the large step (16 for a hex byte, an octave for a
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

    /**
     * A went down on a cell the mapper is DEFERRING (`defer_a_to_release`), so nothing has fired yet
     * and nothing fires here. It exists for the one thing a release cannot recover: the instant.
     *
     * ⚠️ The lazy-chop tap on the sample editor's row 11 is aimed by EAR at the playhead line, and the
     * release comes a reaction time — and possibly an A+DPAD — later. The frame is snapshotted here and
     * the boundary is cut from the snapshot.
     */
    void on_a_deferred();

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

    // ── R + A/B: MUTE and SOLO (SONG and MIXER) ──────────────────────────────────────────────────
    // The chord acts the instant it is pressed and is undone if its own button comes up before R —
    // the mapper owns that half (`MapperState::rComboArmed`) and calls exactly one of the two
    // closers below. The snapshot the revert restores is taken here, on the first toggle of a chord.
    /** R+B: toggle MUTE on the cursor's channel, or on every channel in the selection. */
    void on_r_b();
    /** R+A: toggle SOLO, same targeting. */
    void on_r_a();
    /** R came up first: the chord's changes stand. Drops the snapshot. */
    void on_r_combo_commit();
    /** A/B came up first: put every track's mute and solo back the way the chord found them. */
    void on_r_combo_revert();

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
     * LIVE mode's two chords, both of them SONG's alone: L+START queues the whole cursor ROW as one
     * scene, R+START queues the channel under the cursor to fall silent. Off SONG, or out of LIVE
     * mode, they stay what they have always been — reserved, and doing nothing.
     */
    void on_l_start();
    void on_r_start();

  private:
    /**
     * The one gate every LIVE gesture asks, written once below its four sites: LIVE mode, on SONG,
     * with nothing on top of it. ⚠️ The SONG half is not a nicety — the mapper's L+START and R+START
     * arms are global, so an ungated handler would queue a song row from inside the sample editor.
     */
    bool live_song_gesture() const {
        return host_.live_mode() && s_.currentScreen == ScreenType::SONG &&
               !overlay_swallows(Overlay::NONE);
    }

    /** Bit N set where track N has a chain on `songRow` — the channels a row launch starts sounding. */
    int  live_row_mask(int songRow) const;

    /**
     * Has this row already been queued? It is what promotes a second L+START to the next phrase
     * boundary, and it is read back off the slots rather than remembered: a row queue writes all
     * eight, so any one of them still pointing here says the press before this one aimed at it.
     */
    bool live_row_armed(int songRow) const;

  public:

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

    /** The load-browse categories a config.json override can redirect (D2b). */
    enum class BrowserDir { SAMPLES, SOUNDFONTS, INSTRUMENTS, PROJECTS, THEMES };

    /**
     * The directory a LOAD browser should START in for `cat` (D2b): the user's config.json override when
     * that category is set and the override RESOLVES to a real directory — otherwise the built-in
     * FileSystem default. Public so `ptdispatch` can drive it directly.
     *
     * ⚠️ An override is root-relative unless it is absolute (`ui/folder_config.h`
     * `resolve_folder_override`), so what this returns is not necessarily what config.json says.
     *
     * ⚠️ The single resolution point, so D2a (remember-last-folder) and D2b compose: open_file_browser
     * compares the requested dir against `browser_dir(SAMPLES)`, so a D2b override still reads as "a
     * sample load" for the remember-folder seed.
     */
    std::string browser_dir(BrowserDir cat);

    /**
     * How many of songcore::EFFECT_TYPES this build lets an FX cell be stepped to — the whole list,
     * or the list minus its trailing MIDI commands (ui/platform_caps.h `midi`).
     *
     * ⚠️ WRITTEN ONCE HERE BECAUSE BOTH WAYS INTO THE FX TYPE COLUMN HAVE TO AGREE: the bound on the
     * cell's CursorContext (cc::effect_type) and the size of the picker A+UP opens (ui/fx_helper.h).
     * Two sites deriving it separately is one of them being wrong, and the disagreement would show up
     * as an effect the picker cannot name. Public so `ptdispatch` can assert both against it.
     */
    int visible_effect_type_count() const;

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

    /**
     * Open the MIDI port the settings name, once, at boot — call it after `AppState::midiOut` is set
     * and `settings.json` has been read.
     *
     * ⚠️ IT IS THE SAME TWO CALLS THE MIDI SCREEN MAKES, ON PURPOSE. The shell could resolve a name to
     * an index and call `open()` itself in six lines, and then there would be two answers to "which
     * port is open and why" — a boot one and a UI one — free to drift the moment either grows a case.
     * This is the guardrail's derive-it-from-the-data rule applied to a seam with exactly two callers,
     * which is when it is cheapest to obey.
     *
     * A port that is ALREADY open is left alone: that is the `POCKETTRACKER_MIDI_OUT` dev override,
     * which the shell has already resolved into `settings.midiOutDevice` before calling this — so the
     * device the screen names and the device that is open are the same one by construction.
     */
    void boot_midi_port();

    /**
     * The same, for the INPUT port (phase E2) — resolve `settings.midiInDevice` against the live list,
     * wire the host's queue as the sink, open it.
     *
     * ⚠️ **A SEPARATE CALL AND NOT A SECOND HALF OF `boot_midi_port`**, because the two are separately
     * absent: a build can have an output backend and no input one (Linux between B2b and E5, and any
     * platform where only one of the two libraries is present), and folding them would make "there is
     * no MIDI in on this build" print as though the cable had failed.
     *
     * ⚠️ **THE SINK IS WIRED HERE, WITH THE OPEN, AND THAT PAIRING IS THE POINT.** An input port that is
     * open with no sink drops every byte silently, and one whose sink outlives its `set_sink(nullptr)`
     * is a backend thread writing into a dead object. Both are invisible until they are not, so the two
     * calls live in the one function that can never do one without the other.
     */
    void boot_midi_in_port();

    /**
     * The app has come back to the front — re-list the file browser if that is what is on screen.
     * A no-op on every other screen, and on a listing nothing has changed under.
     *
     * ⚠️ **It exists because the app is not the only writer.** The directory on screen belongs to the
     * user, and while PocketTracker was in the background a file manager, a download or a sync client
     * may have changed it — but the case it was written for is the app's own: Android's folder picker
     * is another activity, so `ADD FOLDER…` necessarily grants while this one is backgrounded, and the
     * roots directory the user granted from is stale the moment they return to it.
     *
     * ⚠️ Inert on desktop by construction rather than by `#ifdef`, the same way the background-flush
     * watcher is: SDL sends `SDL_APP_DIDENTERFOREGROUND` on Android and iOS only, so nothing calls this
     * anywhere else. The cursor is kept and clamped — a refresh is not a navigation.
     */
    void refresh_browser_on_foreground();

  private:
    AppState&               s_;
    songcore::SongcoreHost& host_;
    FileSystem&             fs_;
    long long               now_ms_ = 0;
    RenderHooks             render_{};

    /** See set_media_base_dir. Empty means "relative paths stay relative" (resolve_media_path). */
    std::string mediaBaseDir_{};

    // ── The qwerty text cursor's HOLD ACCELERATION (v0.9.4 C2) ───────────────────────────────────
    //
    // The shell repeats a held R+LEFT/RIGHT at a flat 100 ms, but R+dpad is context-blind there — it
    // is ALSO screen navigation and (C3) sample-editor zoom — so the acceleration cannot live in the
    // shell without speeding those up too. It lives HERE, in the qwerty branch of on_r_left/on_r_right,
    // where the meaning is known. It is derived from the CADENCE of the calls: a sustained hold arrives
    // ~100 ms apart and builds a streak; a fresh press or a pause (the 400 ms initial-delay gap, or a
    // deliberate tap) is > 250 ms apart and resets it. No shell change, no ButtonEvent change, and it
    // touches nothing but the text cursor.
    long long lastTextCursorMoveMs_ = 0;
    int       textCursorRepeatStreak_ = 0;

    /** 1, then 2, then 4 characters per repeat, by how long R+LEFT/RIGHT has been held. */
    int text_cursor_repeat_step();

    // ── The FILE BROWSER's hold acceleration ─────────────────────────────────────────────────────
    //
    // Same reasoning as the text cursor above, same place, same reason it cannot live in the shell:
    // the shell's repeat is context-blind and a plain D-pad is also the tracker's cell cursor. This one
    // measures the ELAPSED HOLD rather than counting repeats, so it says "after 1.5 s" and "after 3 s"
    // directly instead of restating the shell's 400/100 ms cadence as repeat counts that would rot if
    // either constant moved.

    // Both are wall-clock time SINCE THE BUTTON WENT DOWN, minus the shell's 400 ms initial repeat
    // delay: that first gap is wider than the 250 ms same-gesture threshold, so the sustained train —
    // and this clock — starts on the SECOND press, 400 ms in. Written as the subtractions they are,
    // because the 1500 and the 3000 are the request and the 400 is the shell's. ⚠️ Restated from
    // `shell/sdl-input.h`, not included from it: pt-ui does not link the shell, and must not.
    static constexpr long long BROWSER_ACCEL_X2_MS = 1500 - 400;
    static constexpr long long BROWSER_ACCEL_X4_MS = 3000 - 400;

    long long browserHoldStartMs_   = 0;
    long long lastBrowserMoveMs_    = 0;
    int       lastBrowserMoveDelta_ = 0;

    /**
     * 1 unit per repeat, then 2 at 1.5 s of hold, then 4 at 3 s.
     *
     * The unit is the caller's — a row for UP/DOWN, a screenful for LEFT/RIGHT — so both pairs
     * accelerate in their own terms. `delta` is also the gesture's IDENTITY: a change of direction
     * starts a fresh hold even inside the repeat cadence, or three seconds of DOWN would make the first
     * tap of RIGHT jump four whole pages.
     */
    int browser_repeat_factor(int delta);

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

    // The FILE BROWSER's line, on the same 5 s window. ⚠️ A SEPARATE PAIR, and not for tidiness:
    // feeding both message fields through ONE pair stops the dismissal working at all — `lastSeen`
    // then thrashes between two different strings and re-arms the deadline on every frame, so
    // neither line ever expires. (Measured; §34's own checks are what go red.)
    std::string browserStatusLastSeen_{};
    long long   browserStatusDismissAtMs_ = 0;

    /** 5 s — Kotlin's own delay (MainActivity's two status LaunchedEffects). */
    static constexpr long long STATUS_DISMISS_MS = 5000;

    /** The watchers and the deadlines, all run once a frame by set_now(). */
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

    // ── The MUTE/SOLO chord's undo ───────────────────────────────────────────────────────────────
    // What the mix looked like when the chord started, so `on_r_combo_revert()` can put it back. All
    // EIGHT tracks, not the one under the cursor: a chord over a selection touches several, and a
    // SOLO changes what every other track can be heard doing.
    //
    // ⚠️ Taken on the FIRST toggle of a chord and not on every press, or holding R and muting three
    // channels in turn would leave the revert able to undo only the last of them.
    struct MixSnapshot {
        bool live = false;
        bool mute[8] = {false, false, false, false, false, false, false, false};
        bool solo[8] = {false, false, false, false, false, false, false, false};
    };
    MixSnapshot mixSnapshot_{};

    /** The channels a MUTE/SOLO chord applies to: the selection's columns, else the cursor's. */
    void mute_solo_targets(int (&out)[8], int& count) const;
    /** Toggle `mute` (or `solo`) on those channels, arm the snapshot, and push the result. */
    void toggle_mute_solo(bool solo);
    /** Every track audible again — L+R's mute rung, and the whole of "restore full playback". */
    void restore_full_playback();
    /** Whether R+A/R+B mean MUTE/SOLO on the screen as it stands — and so whether B must be held. */
    bool mute_solo_chord_live() const;
    /**
     * L+R's recency flag, DERIVED rather than stamped by each of the paths that could move it.
     *
     * ⚠️ There is no one place a selection changes: L+B opens it, the D-pad drags its edge, B copies
     * and exits, L+A cuts, a paste fills the buffer, and the next screen to grow a selection will add
     * another. A flag every one of those had to remember to set is the failure this codebase has hit
     * seven times — so the watcher folds the observable state into one number instead and notices
     * when it moves. `run_selection_recency()` runs once a frame from `set_now()`, AND at the top of
     * `toggle_mute_solo()`, which is what keeps the ordering right to the PRESS rather than to the
     * frame when both happen inside one 16 ms batch of events.
     */
    unsigned selection_signature() const;
    void     run_selection_recency();
    unsigned selectionSig_ = 0;

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
    MidiModule             midi_{};
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
     * is up. A+RIGHT over a 4-row selection increments four cells.
     */
    void selection_or_single(InputAction (*fn)(const CursorContext&));

    /** `handleDPadNavigation`: move the cursor, or drag the selection's active edge. */
    void dpad_nav(NavDir direction);

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

    /**
     * The song cell the cursor last left SONG on, as a 0-based track — the TIE-BREAK a chain or
     * phrase played from its own screen offers `songcore::track_of_*`, never the answer on its own.
     *
     * ⭐ It needs no new field: `go_to_screen` saves songCursorRow/Column every time you leave SONG,
     * and on SONG the column IS the track, 1-based. Because it only ever breaks a tie between tracks
     * that already hold the chain, a stale one cannot route a chain somewhere it does not live.
     */
    int remembered_song_track() const;

    /**
     * B+UP/DOWN under NAV = SONG: the CHAIN screen walks the track column of the song grid, the PHRASE
     * screen walks the current chain's filled rows. Returns true when it owned the press — including
     * when the walk clamped and nothing moved (ui/song_pointer.h).
     */
    bool song_relative_b_vertical(int delta);

    /**
     * The mixer channel an audition on the preview lane borrows, or -1 for the neutral unity gain the
     * lane has always had.
     *
     * ⚠️ An INSTRUMENT IS NOT IN THE SONG — only its uses are. So this is the song cell you came
     * through, and only while that cell still holds a chain; "the first track that plays it" is
     * available and tempting, but an instrument used on five tracks would audition through whichever
     * sorts first, which is a lie with no gesture behind it.
     */
    int audition_track() const;

    /** True on the three screens that edit an INSTRUMENT rather than the arrangement. */
    bool on_instrument_screen() const;

    /** True on the two that edit the GLOBALS — the mixer, the master bus, the send buses. */
    bool on_globals_screen() const;

    /**
     * INSTRUMENT row 0, A+LEFT/RIGHT on the TYPE cell: switch outright if the slot is EMPTY, else put the
     * CONFIRM DIALOG in front of it — the toggle drops the loaded source, since a sampler has no use
     * for an .sf2 and vice versa.
     *
     * `delta` is the direction — +1 for A+RIGHT, −1 for A+LEFT — and it rides through the dialog in
     * `ConfirmDialogState::arg`, because with three types the two buttons no longer mean the same.
     */
    void request_instrument_type_toggle(int delta);
    void toggle_instrument_type(int delta);

    /** True when the cursor is on INSTRUMENT's TYPE cell — where A+LEFT/RIGHT toggles rather than steps. */
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

    // ═════════════════════════════════════════════════════════════════════════════════════════════
    // THE OVERLAY STACK — `top_overlay()` below is the ONE place its order is written
    // ═════════════════════════════════════════════════════════════════════════════════════════════
    //
    // The order is the specification (see THE MODAL RULE at the top of this file). Every handler has
    // to know it, and none of them may state it: a stack re-typed at two dozen call sites is a stack
    // that drifts, and each site's subset is correct only about TODAY's overlays.
    //
    // A handler names the layers it answers for ITSELF and `overlay_swallows()` answers for the rest.
    // ⭐ **The default for a layer a handler does not name is SWALLOW**, and that is what makes a new
    // overlay ONE registration: add the enumerator, add its line to `top_overlay()`, and every handler
    // that has not been taught about it is INERT under it. The failure mode of forgetting one is a
    // button that does nothing — not a button that edits the screen hidden behind the new overlay.
    //
    // ⚠️ FX_HELPER and BROWSER are not modals. The helper is a picker held up by A, and the browser is
    // a SCREEN — but every handler has to ask about them in the same breath and in the same place as
    // the three real modals, so they are layers here. `modal_backdrop_active` (ui/app_state.h) asks
    // the narrower question of which layers paint a full-canvas scrim, and stays separate.

    /**
     * ⚠️ The enumerators are BITS, so that a handler's `arms` set is a plain OR of them and reads as
     * one line. `top_overlay()` returns exactly one of them; `overlay_swallows()` takes any number.
     */
    enum class Overlay : unsigned {
        NONE      = 0,
        CONFIRM   = 1u << 0,
        QWERTY    = 1u << 1,
        THEME     = 1u << 2,
        EQ        = 1u << 3,
        FX_HELPER = 1u << 4,
        BROWSER   = 1u << 5,
    };

    friend constexpr Overlay operator|(Overlay a, Overlay b) {
        return static_cast<Overlay>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
    }

    /**
     * The topmost open layer — the one whose vocabulary a press belongs to.
     *
     * ⚠️ QWERTY sits ABOVE THEME because it genuinely stacks on it: the editor's SAVE raises the
     * keyboard without closing the editor, and a D-pad press there must move the KEY cursor. That is
     * the only pair in the app that can be open at once; every other ordering below is disjoint by
     * construction, which is exactly why it was free to drift before it lived in one function.
     */
    Overlay top_overlay() const {
        if (confirm_open())     return Overlay::CONFIRM;
        if (qwerty_open())      return Overlay::QWERTY;
        if (theme_open())       return Overlay::THEME;
        if (eq_open())          return Overlay::EQ;
        if (s_.fxHelper.isOpen) return Overlay::FX_HELPER;
        if (on_browser())       return Overlay::BROWSER;
        return Overlay::NONE;
    }

    /**
     * THE MODAL RULE, DERIVED: true when a layer this handler does not answer for is up, and the
     * caller must return without touching the screen behind it.
     *
     * `arms` is the set of layers the handler takes responsibility for — either by SERVING them with
     * the arms that follow this call (the D-pad moves the key cursor, the theme cursor, the EQ cursor
     * or the file cursor) or by deliberately LETTING THEM THROUGH to the screen underneath, which is
     * what START does for the two partial overlays: they name it, and it reaches the transport.
     *
     * `Overlay::NONE` is the common `arms` value and reads as "any layer at all owns this button".
     */
    bool overlay_swallows(Overlay arms) const {
        const Overlay top = top_overlay();
        return top != Overlay::NONE &&
               (static_cast<unsigned>(arms) & static_cast<unsigned>(top)) == 0;
    }

    /**
     * The confirm dialog owns EVERY button but A and B. It is the topmost modal — drawn last, over
     * even the keyboard — so it is checked FIRST, ahead of the keyboard and the browser both, and it
     * is the one guard that simply RETURNS rather than redirecting.
     *
     * ⚠️ The check appears at the top of every handler but three, and the exceptions are exactly
     * `on_button_a` and `on_button_b`, which ARE the answer, and `on_stop_preview`, which silences a
     * ringing audition (a dialog raised over an INSTRUMENT audition must not leave the note hanging —
     * and silencing a note is not an edit). ptdispatch asserts the rest: with a confirm up, every
     * other button is inert. **That assertion is the real guarantee here, not the code shape** —
     * Kotlin's own comment on this predicate warns that "every new show*Dialog-style modal state MUST
     * be added" to it, and a rule that has to be remembered at every new handler is a rule that will
     * eventually be forgotten once. (No count in prose: a number maintained by hand goes stale
     * silently — this one had said 28 for a while when it was 30 — and the check already holds the
     * claim that matters.)
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
     * ⚠️ It is not the WHOLE of what is deferred: `defer_a_to_release` has one arm above this one, for
     * a cell whose A opens nothing (the sample editor's lazy-chop tap). The rule below applies to that
     * arm as much as to this list — nothing joins it without an A the RELEASE can still perform.
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

    /**
     * Is there an OPEN port right now — the question PANIC and TEST both have to ask before claiming
     * to have done anything.
     *
     * ⚠️ TWO conditions, and the second is the one that matters: a backend can exist and hold no port
     * (nothing picked, or an open that failed). Answering on `midiOut != nullptr` alone would make the
     * screen report "TEST SENT" into a closed handle — a vacuous pass on the one control whose entire
     * purpose is telling the user whether the cable is real.
     */
    bool port_open() const { return s_.midiOut != nullptr && s_.midiOut->is_open(); }

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

    // ── MIDI: the screen, the port and the two buttons (phase B4.3) ─────────────────────────────
    /** A on MIDI: only PANIC and TEST do anything — OUTPUT / OFFSET / PROG CHG are A+DPAD. */
    void midi_action();

    /**
     * Re-enumerate the ports and re-resolve the saved device NAME against the fresh list.
     *
     * ⚠️ CALLED ON EVERY ENTRY TO THE SCREEN, not once at boot. MIDI is hot-pluggable: a list built at
     * launch is a list of the cables that were in at launch, and the screen whose whole job is picking
     * one is exactly where that is a bug the user cannot diagnose. Cheap — `device_count()` is an OS
     * call on a handful of ports, once per screen entry, not per frame.
     *
     * A saved name that is not in the list resolves to index 0 = OFF, and that is the honest answer:
     * the row shows what is OPEN, never what was once wanted (see midi_settings.h).
     */
    void refresh_midi_devices();

    /**
     * Make the port match `settings.midiOutDevice` — close whatever is open, open the named one.
     *
     * ⚠️ PANICS FIRST, AND ITS OWN PANIC IS NOT REDUNDANT. `ExternalConsumer::set_out` panics when the
     * POINTER changes, and here it never does: the shell hands over one `IMidiOut` object for the whole
     * session and this swaps the DEVICE underneath it. So the consumer cannot see the change, and every
     * note sounding on the old device would be held by hardware that is about to stop being listened
     * to — the note-offs we owe delivered to a port that is already closed.
     */
    void apply_midi_device();

    /** `refresh_midi_devices`' twin for the INPUT list (E2). Same rules, a different enumeration. */
    void refresh_midi_in_devices();

    /**
     * Make the input port match `settings.midiInDevice` — unwire the sink, close, open the named one,
     * wire the sink back.
     *
     * ⚠️ THE ORDER IS THE WHOLE FUNCTION. `set_sink(nullptr)` before `close()`, so no byte from the old
     * device can arrive during the swap; `host_.reset_midi_in()` between them, because a cable pulled
     * mid-message leaves running status in force and the next port's first data byte would otherwise
     * complete a note nobody played on the previous device's channel; `set_sink` before `open`, because
     * a port that is open is a port already delivering.
     */
    void apply_midi_in_device();

    /**
     * ⭐ **THE THRU VERDICT (E4): is the port we listen on the same one we send on?**
     *
     * MIDI thru — a live key on an EXTERNAL instrument reaching the cable — is the feature. On a
     * LOOPBACK port it is instead an amplifying feedback loop (key → cable → the same port → key), and
     * a loopback is not exotic here: it is the only MIDI-in test rig this project has (§0.8).
     *
     * ⚠️ **ONE FUNCTION, CALLED FROM BOTH SIDES, BECAUSE THE VERDICT DEPENDS ON BOTH PORTS.** Either
     * device row can change it, so a rule spelled out at each site is a rule one of them will forget —
     * the repo has paid for that five times (the modal-guard predicate, `settingsDirty`, …). It is
     * derived from the two names the SCREEN is showing, which is the same data the user is reading.
     */
    void update_midi_thru();

    /** NEW, and the engine sync a fresh project needs (SongcoreHost::new_project). */
    void start_new_project();

    /** Slot 0 across the board, and no selection. Shared by NEW and LOAD. */
    void reset_editing_context();

    /** A .ptp replaced the document: clean, no selection, browser closed. */
    void load_project_done(const std::string& path);

    /** EXPORT. Renders SYNCHRONOUSLY; `on_render_progress_` repaints the frame from inside it. */
    void export_song(bool stems);

    /**
     * SONG-selection RESAMPLE — the APPLY of the RESAMPLE keyboard. Renders the live selection to a WAV
     * in Resampled/ and loads it into a fresh instrument. SYNCHRONOUS, exactly like export_song: it
     * stops the transport, suspends the audio device, repaints from the render's progress, and restores
     * the device after. `customBaseName` empty ⇒ auto `Resample_NNNN`. A no-op if a render is already
     * running or the selection has gone.
     */
    void resample_selection(const std::string& customBaseName);

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

    /**
     * Row 11's POSITION cell: A+DPAD drags the slice boundary under the cursor, as it drags a
     * selection edge one section up. Col 0 is the index and keeps its own cell — dragging there would
     * fight the step that chooses which marker you are looking at.
     */
    bool on_sample_slice_marker_row() const {
        return on_sample_editor() && s_.sampleEditor.cursorRow == 11 &&
               s_.sampleEditor.cursorCol == 1 &&
               s_.sampleEditor.sliceMethod != SampleEditorModule::SLICE_OFF;
    }

    /** A+DPAD on row 11 col 1. Mirrors `nudge_selection_edge`; what a boundary may DO is what differs. */
    void nudge_slice_marker(int64_t delta);

    /**
     * Row 11 under MANUAL, BOTH columns: a plain A is the lazy-chop tap, and the mapper therefore holds
     * it until A is released.
     *
     * ⚠️ Every other gesture on this row starts with the same A held down — A+DPAD steps the slice
     * number on col 0 and drags the boundary on col 1, A+B deletes one. Fire the tap on the PRESS and
     * each of those is preceded by a boundary nobody asked for. It is why the EQ slot cell defers its
     * A too — a cell with an A+DPAD of its own cannot also act on the press.
     */
    bool on_slice_tap_cell() const {
        return on_sample_editor() && !s_.sampleEditor.showConfirmClose &&
               s_.sampleEditor.cursorRow == 11 &&
               s_.sampleEditor.sliceMethod == SampleEditorModule::SLICE_MANUAL;
    }

    /** Put the selection on the slice row 11's cursor is pointing at — every boundary gesture ends here. */
    void select_current_slice();

    /** Copy the method's computed boundaries into the hand-placed list, so a nudge has a home. */
    void materialise_manual_markers();

    /** A+B on row 11: put the marker under the cursor back where its method would have put it. */
    void reset_slice_marker();

    /**
     * A on row 11 under MANUAL: cut a boundary at the PLAYHEAD, while the sample is sounding.
     *
     * Slicing by ear rather than by number — press START, listen, and tap A on each hit. It makes the
     * same kind of boundary a rightward drag does, so everything already built for one applies to it:
     * A+DPAD walks it, A+B removes it, and the list stays sorted however out of order the taps arrive.
     */
    void tap_slice_marker();

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

    /**
     * Re-read the length and the waveform after an op. `reset_selection` says the op CHANGED THE LENGTH,
     * and it does more than re-select: everything measured in frames — the selection, the instrument's
     * own sample and loop windows, and ⚠️ **every slice marker** — describes audio that no longer exists,
     * so all of it is reset rather than clamped.
     */
    void refresh_sample_view(bool reset_selection);

    /** SAVE / SAVE-AS / OVERWRITE all end here: write the WAV, adopt it, and leave the editor. */
    void save_sample_to(const std::string& path, bool adopt_name);

    /** CHOP: every slice out to `Samples/Chops/<name>/`, as its own WAV. */
    void sample_editor_chop();

    /** The slice (start, end) pairs the current method defines — CHOP's work list. */
    std::vector<std::pair<int64_t, int64_t>> current_slices() const;

    /**
     * ⚠️ The deferred half of the audition (see set_now). The preview arms an exact-frame window and
     * strips the instrument's EQ, sends and modulation — and the voice reads all four when it TRIGGERS,
     * 100 frames later, not when it is scheduled. So none of them can be put back until then, and these
     * three fields are what remembers to do it.
     *
     * A second START inside the window runs it IMMEDIATELY, before arming anything: otherwise the first
     * preview's restore would land in the middle of the second audition and undo it.
     */
    void run_due_sample_preview_restore(bool force = false);

    bool      previewRestorePending_ = false;
    long long previewRestoreAtMs_    = 0;
    int       previewRestoreInst_    = 0;

    /**
     * The playhead as it stood when A went down on the tap cell — 0..1, or −1 for "nothing was
     * sounding", which is the same value `playbackPosition` itself carries when there is no audition.
     *
     * ⚠️ Written by `on_a_deferred` and CONSUMED by `tap_slice_marker`, so a tap can never read a
     * position from a press that ended in a combo instead.
     */
    float sliceTapPlayhead_ = -1.0f;

    SampleEditorModule sample_{};
};

}  // namespace pt::ui
