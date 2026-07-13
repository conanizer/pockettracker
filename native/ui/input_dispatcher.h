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
// ── SCOPE: the eleven screens that exist ─────────────────────────────────────────────────────────
//
// SONG, CHAIN, PHRASE, TABLE, GROOVE, INSTRUMENT, INST.POOL, MODS (S4), MIXER, EFFECTS (S5), and —
// since S6a — the FILE BROWSER, with the QWERTY KEYBOARD overlay behind it. The Kotlin dispatcher is
// ~3200 lines, and what is still missing here serves screens the port has not reached: the sample
// editor, PROJECT, SETTINGS, the EQ editor. Each lands WITH its screen, in the session that draws it.
// The structure is built to receive them: a new screen is a new arm in `generic_input()` and
// `apply_edit()`, not a new branch in every handler.
//
// ⚠️ Some cells still open a SUB-SCREEN that does not exist — INSTRUMENT's EDIT (the sample editor,
// S6b) and every EQ cell (the EQ overlay: INSTRUMENT's, the pool's, MIXER's master EQ, EFFECTS' two
// input EQs). On Android a plain A opens that overlay; here it is a no-op, and A+DPAD still dials the
// slot NUMBER, which is the part of the cell that is a plain value.
//
// ── ⚠️ THE MODAL RULE, which arrives with S6a ────────────────────────────────────────────────────
//
// The QWERTY keyboard is the app's first true modal, and the FILE BROWSER is the first full-screen
// popup. Both OWN THE BUTTONS while they are up, and every handler below therefore opens with the same
// two questions in the same order — keyboard first, then browser — before it does anything else. That
// order is the specification: the keyboard can be open ON TOP of the browser (SELECT+A to rename a
// file), and a D-pad press there must move the KEY cursor, not the file cursor.
//
// Kotlin enforces this the same way (an `if (qwertyKeyboardState.isOpen) { … ; return }` at the top of
// each handler) and its own comment states the rule the hard way: "every new show*Dialog-style modal
// state MUST be added to this predicate". A modal that one handler forgets is a button that does the
// wrong thing exactly once — and that is a bug nobody reports, because it looks like a mis-press.
//
// ── THE SHELL IS THE InputMapper ─────────────────────────────────────────────────────────────────
//
// Android's chain is InputMapper → ButtonHandlers → AppInputDispatcher: the mapper owns the physical
// keys, the held-modifier state and the key repeat, and calls a NAMED handler. The split is kept
// exactly — `linux/sdl-input.h` is the mapper, and the methods below are the ButtonHandlers. That is
// why there is no `handle(ButtonEvent)` here: pt-ui must not know SDL exists (a `ButtonEvent` is an
// SDL-side type), and a dispatcher that took one could not be driven by a headless tool.
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
#include "ui/modules/file_browser.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/instrument_editor.h"
#include "ui/modules/instrument_pool.h"
#include "ui/modules/mixer.h"
#include "ui/modules/modulation.h"
#include "ui/modules/phrase_editor.h"
#include "ui/modules/qwerty_keyboard.h"
#include "ui/modules/song_editor.h"
#include "ui/modules/table_editor.h"

#include <string>
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

    /** The frame's clock reading. Feeds the L+B multi-tap window and nothing else. */
    void set_now(long long now_ms) { now_ms_ = now_ms; }

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

    /** The clipboard, for the top-strip readout ("PHR:2x3"). */
    const Clipboard& clipboard() const { return clip_; }

    // ── Opening the sub-screens (the shell needs these too, for its start-up state) ──────────────

    /**
     * Show the browser, filtered, in `directory`, and remember what A will do with the pick.
     * `previousScreen` is captured here — it is where B returns to.
     */
    void open_file_browser(AppState::BrowserPurpose purpose, const std::string& directory,
                           const std::vector<std::string>& extensions);

  private:
    AppState&               s_;
    songcore::SongcoreHost& host_;
    FileSystem&             fs_;
    long long               now_ms_ = 0;

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

    /** An edit happened: tell the sequencer, so a note already scheduled past the cursor is redone. */
    void mark_modified(bool table_touched = false);

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

    // ── INSTRUMENT's three buttons, which S4 drew and could not press ────────────────────────────
    /**
     * A on one of the four cells that open something: the preset LOAD/SAVE on row 0, and the source
     * LOAD on the SOURCE row. Returns true if it handled the press.
     *
     * (EDIT — the fourth — is the sample editor, and still opens nothing. S6b.)
     */
    bool instrument_open_at_cursor();
};

}  // namespace pt::ui
