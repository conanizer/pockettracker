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
// ── SCOPE: the five screens that exist ───────────────────────────────────────────────────────────
//
// SONG, CHAIN, PHRASE, TABLE, GROOVE. The Kotlin dispatcher is ~3200 lines, but well over half of it
// serves screens the port has not reached — the file browser, the sample editor, INSTRUMENT, MIXER,
// SETTINGS, the EQ editor, the qwerty keyboard. Porting their input against modules that do not
// exist would be writing code no one can run and no tool can measure; each lands WITH its screen, in
// the session that draws it. The structure here is built to receive them: a new screen is a new arm
// in `generic_input()` and `apply_edit()`, not a new branch in every handler.
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
#include "ui/modules/chain_editor.h"
#include "ui/modules/groove_editor.h"
#include "ui/modules/phrase_editor.h"
#include "ui/modules/song_editor.h"
#include "ui/modules/table_editor.h"

namespace pt::ui {

class InputDispatcher {
  public:
    InputDispatcher(AppState& state, songcore::SongcoreHost& host) : s_(state), host_(host) {}

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

    /** The clipboard, for the top-strip readout ("PHR:2x3"). */
    const Clipboard& clipboard() const { return clip_; }

  private:
    AppState&              s_;
    songcore::SongcoreHost& host_;
    long long              now_ms_ = 0;

    Clipboard clip_{};

    SongEditorModule   song_{};
    ChainEditorModule  chain_{};
    PhraseEditorModule phrase_{};
    TableModule        table_{};
    GrooveModule       groove_{};

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
};

}  // namespace pt::ui
