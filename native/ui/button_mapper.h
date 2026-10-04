#pragma once

// ─── THE COMBO MATRIX ────────────────────────────────────────────────────────────────────────────
//
// `ButtonEvent` in, one named dispatcher handler out — "which of the ~30 handlers does this press
// mean?", from the button plus the modifiers held at that instant.
//
// A template so a test can instantiate it with a recording stub and test the check ORDER;
// in the app `Dispatcher` is `InputDispatcher` (no virtual call).
// ⚠️ A `Dispatcher` must provide every `on_*` named below plus `defer_a_to_release()`,
// `defer_b_to_release()`, `on_a_deferred()`, `on_r_held(bool)` and `help_full_open()`.

#include "ui/buttons.h"

#include <cstdint>

namespace pt::ui {

/** The mapper's memory between events, and nothing else. */
struct MapperState {
    /** The A,A double-tap window. */
    uint64_t lastAPress = 0;

    /**
     * ⚠️ The DEFERRED-A latch. Set when A goes down on a cell whose A OPENS something but whose
     * A+DPAD / A+B means something else: the open waits for A's RELEASE and is cancelled by any A-combo
     * in between (else A+B on the NAME cell would open the keyboard first). The dispatcher picks the
     * cells (`defer_a_to_release()`); the mapper sees press, combo and release as one gesture.
     */
    bool aPressedAlone = false;

    /**
     * ⚠️ The DEFERRED-B latch, the mirror of the above, for the EQ EDITOR: B is both the close and the
     * modifier of the slot cycle (B+LEFT/RIGHT), so the close waits for B's release and any B-combo
     * cancels it.
     */
    bool bPressedAlone = false;

    /**
     * ⚠️ The MUTE/SOLO chord latch. R+B and R+A change the mix on the press, and which button comes UP
     * first decides whether it stays: R first keeps it, A/B first reverts it — a latching and a
     * momentary mute in one chord. The dispatcher is told to commit or revert.
     */
    bool rComboArmed = false;

    /**
     * ⚠️ The DEFERRED-SELECT latch (HELP). Set when SELECT goes down alone, cleared by any other press,
     * so the handler fires on release only for a bare tap. On the FILE BROWSER SELECT is purely a
     * modifier (SELECT+A/B/R); firing on the press would flash help on every rename, delete and mkdir.
     */
    bool selectPressedAlone = false;
};

/**
 * The COMBO MATRIX: owns only "which named handler does this press mean?". What a handler DOES is
 * `pt::ui::InputDispatcher`.
 *
 * ⚠️ THE ORDER OF THESE CHECKS IS THE SPECIFICATION. Most-specific first, each arm RETURNS: L+B+A
 * before L+A (or a clone pastes), A+B before plain A (or a delete inserts), R+A / R+B before plain A
 * and B (or holding R to change screens edits underneath).
 * ⚠️ Modifiers come from the EVENT, never `input.is_held()`: a frame's events arrive at once, so a poll
 * would see the end of the frame — B-then-A in one frame would fire A+B (delete) on the B press.
 */
template <class Dispatcher>
void handle_button(const ButtonEvent& e, Dispatcher& d, MapperState& ms, uint64_t now) {
    const ButtonMods& m = e.mods;

    // ── R's held state is published first, above every early return ──────────────────────────────
    //
    // MIDI learn is "hold R and turn a knob"; the knob is a CC, never a ButtonEvent, so all the matrix
    // can do is say when R is down. ⚠️ At the very top, or an early-returning arm could leave R "held".
    if (e.button == Button::R_SHIFT) d.on_r_held(e.action == ButtonAction::PRESSED);

    // ── RELEASE ──────────────────────────────────────────────────────────────────────────────────
    if (e.action != ButtonAction::PRESSED) {
        // ⚠️ The MUTE/SOLO chord ends on a release, and which release is the specification. Tested
        // first because it CONSUMES the release (an armed A must not reach `on_a_released()`; the FX
        // helper it would commit is armed only by a plain A, which never arms this).
        // `m.r`, never `is_held()`: on the A/B release `m.r` is exactly "is R still down?".
        if (ms.rComboArmed) {
            if (e.button == Button::R_SHIFT) {
                ms.rComboArmed = false;
                d.on_r_combo_commit();
                return;
            }
            if ((e.button == Button::A || e.button == Button::B) && m.r) {
                ms.rComboArmed = false;
                d.on_r_combo_revert();
                return;
            }
        }
        if (e.button == Button::A) {
            // The deferred single-A: no A-combo intervened, so the open fires now, on release.
            if (ms.aPressedAlone) {
                ms.aPressedAlone = false;
                d.on_button_a();
            }
            // The FX helper commits on A's RELEASE: hold A, browse the grid, let go on the effect.
            d.on_a_released();
        }
        if (e.button == Button::B) {
            // The deferred single-B (EQ editor close), mirror of the A latch.
            if (ms.bPressedAlone) {
                ms.bPressedAlone = false;
                d.on_button_b();
            }
        }
        if (e.button == Button::SELECT) {
            // The deferred single-SELECT: nothing else was pressed while it was held — help, or the
            // keyboard's abort. ANY other press clears it, not just a recognised combo, because
            // SELECT+DPAD is unclaimed and must not read as a tap.
            if (ms.selectPressedAlone) {
                ms.selectPressedAlone = false;
                d.on_select();
            }
        }
        return;
    }

    // ── The FULL help overlay: any press closes it and goes NO further ───────────────────────────
    //
    // ⚠️ CONSUMED — the opposite of the compact panel below: the overlay covers the editor, so a press
    // falling through would edit a cell the user cannot see. SELECT's release then finds no armed tap,
    // so the overlay cannot reopen itself. A ringing audition is still silenced.
    if (d.help_full_open()) {
        ms.selectPressedAlone = false;
        d.on_help_dismiss();
        if (e.button != Button::START && !m.a) d.on_stop_preview();
        return;
    }

    // ── HELP: any press but SELECT puts the compact panel away ───────────────────────────────────
    //
    // ⚠️ Above every arm, and it consumes nothing: `on_help_dismiss` clears a flag and the press falls
    // through, so help cannot swallow or reorder a gesture. SELECT is excluded — its press arms the tap,
    // and dismissing here would close the panel just before the release reopens it.
    if (e.button != Button::SELECT) {
        ms.selectPressedAlone = false;   // SELECT is being used as a modifier, not tapped
        d.on_help_dismiss();
    }

    // Silence a ringing audition on any plain press. START (plays) and anything with A held (edit
    // combos, which should stay audible) are exempt.
    if (e.button != Button::START && !m.a) d.on_stop_preview();

    // ── A + … : the tracker's core editing gesture ───────────────────────────────────────────────
    if (m.a && !m.l && !m.r) {
        switch (e.button) {
            // ⚠️ Every arm CANCELS the deferred A: it was a combo, so the held open must not fire.
            case Button::B:          ms.aPressedAlone = false; d.on_a_b();     return;   // delete / reset
            case Button::DPAD_UP:    ms.aPressedAlone = false; d.on_a_up();    return;   // +16 / +1 octave (or the FX helper)
            case Button::DPAD_DOWN:  ms.aPressedAlone = false; d.on_a_down();  return;   // −16 / −1 octave
            case Button::DPAD_RIGHT: ms.aPressedAlone = false; d.on_a_right(); return;   // +1
            case Button::DPAD_LEFT:  ms.aPressedAlone = false; d.on_a_left();  return;   // −1
            default: break;
        }
    }

    // ── SELECT + … : the file browser's file-management chords ───────────────────────────────────
    //
    // ⚠️ `(!m.r || e.button == R_SHIFT)`, not `!m.r`: the mods snapshot INCLUDES the button being
    // pressed, so on SELECT+R, R's own press has already set `m.r`.
    if (m.select && !m.l && (!m.r || e.button == Button::R_SHIFT)) {
        switch (e.button) {
            case Button::A:       d.on_select_a(); return;   // rename     (opens the keyboard)
            case Button::B:       d.on_select_b(); return;   // delete     (arms the confirm)
            case Button::R_SHIFT: d.on_select_r(); return;   // new folder (opens the keyboard)
            default: break;
        }
    }

    // ── B + DPAD: WHICH item am I looking at? ────────────────────────────────────────────────────
    if (m.b && !m.l && !m.r && !m.a) {
        switch (e.button) {
            // ⚠️ Every arm CANCELS the deferred B, or cycling the EQ slot with B+RIGHT would shut the
            // editor when B came up.
            case Button::DPAD_LEFT:  ms.bPressedAlone = false; d.on_b_left();  return;   // prev item / EQ slot −1
            case Button::DPAD_RIGHT: ms.bPressedAlone = false; d.on_b_right(); return;   // next item / EQ slot +1
            case Button::DPAD_UP:    ms.bPressedAlone = false; d.on_b_up();    return;   // SONG / pool: page up
            case Button::DPAD_DOWN:  ms.bPressedAlone = false; d.on_b_down();  return;   // SONG / pool: page down
            default: break;
        }
    }

    // ── L+R: leave selection mode ────────────────────────────────────────────────────────────────
    if (m.l && m.r) {
        switch (e.button) {
            // Reserved chords: consumed so they cannot fall through to a single-button handler.
            case Button::SELECT:
            case Button::A:
            case Button::B:       return;
            case Button::L_SHIFT:
            case Button::R_SHIFT: d.on_l_r(); return;
            default: break;
        }
    }

    // ── L+B+A: clone. BEFORE the L+button block, or L+A would paste instead. ─────────────────────
    if (m.l && m.b && !m.r && e.button == Button::A) {
        d.on_l_b_a();
        return;
    }

    // ── L + … : selection and the clipboard ──────────────────────────────────────────────────────
    if (m.l && !m.r) {
        switch (e.button) {
            case Button::A:     d.on_l_a(); return;   // cut (in a selection) / paste (outside one)
            case Button::B:     d.on_l_b(); return;   // enter selection, then widen it
            // LIVE mode's row queue on SONG; reserved elsewhere (the handler's own gate returns). START
            // must not toggle playback here either way.
            case Button::START: d.on_l_start(); return;
            default: break;                           // L+DPAD is reserved
        }
    }

    // ── R + DPAD: move between screens ───────────────────────────────────────────────────────────
    if (m.r && !m.l) {
        switch (e.button) {
            case Button::DPAD_UP:    d.on_r_up();    return;
            case Button::DPAD_DOWN:  d.on_r_down();  return;
            case Button::DPAD_LEFT:  d.on_r_left();  return;
            case Button::DPAD_RIGHT: d.on_r_right(); return;
            // MUTE and SOLO, on the cursor's channel or every channel in the selection.
            // ⚠️ Both ARM the latch even where the dispatcher does nothing: a chord armed on SONG and
            // released after an R+DPAD to MIXER must still be closed out.
            case Button::A:          d.on_r_a(); ms.rComboArmed = true; return;
            case Button::B:          ms.bPressedAlone = false; d.on_r_b(); ms.rComboArmed = true; return;
            // ⚠️ R arriving SECOND is the other half of the mute chord — half the time B lands first.
            // Screens where R+B mutes hold their B (`defer_b_to_release`) so this arm can still claim
            // it; `bPressedAlone`, not `m.b`, is the test, because a B that already acted must not
            // also be a chord. A has no counterpart: elsewhere its press has already inserted.
            case Button::R_SHIFT:
                if (ms.bPressedAlone) {
                    ms.bPressedAlone = false;
                    d.on_r_b();
                    ms.rComboArmed = true;
                }
                return;
            // LIVE mode's stop queue on SONG; reserved and consumed elsewhere, like L+START.
            case Button::START:      d.on_r_start(); return;
            default: break;
        }
    }

    // ── A, and the A,A double-tap ────────────────────────────────────────────────────────────────
    // 300 ms. The window is the mapper's, passed in rather than a function-local static so a test can
    // drive it.
    if (e.button == Button::A && !m.l && !m.r) {
        // ⚠️ The DEFER first: on a cell whose A opens a sub-screen, hold the action until release so an
        // A+DPAD or A+B is not pre-empted. Such a cell is never a double-tap cell, and `lastAPress` is
        // cleared so the next A cannot complete a double-tap that never happened.
        if (d.defer_a_to_release()) {
            ms.aPressedAlone = true;
            ms.lastAPress    = 0;
            // ⚠️ The dispatcher is told the press happened: a deferred action aimed at a MOVING number
            // (the sample editor's playhead) must read it now, not a reaction time later.
            d.on_a_deferred();
            return;
        }
        if (now - ms.lastAPress < 300) {
            ms.lastAPress = 0;   // …so a triple-tap does not read as two double-taps
            d.on_a_a();          // insert the next UNUSED chain/phrase
        } else {
            ms.lastAPress = now;
            d.on_button_a();     // insert the LAST-EDITED one
        }
        return;
    }

    // ── B, and the deferred close ────────────────────────────────────────────────────────────────
    if (e.button == Button::B && !m.l && !m.r && !m.a) {
        // ⚠️ The DEFER, as A's: in the EQ editor B closes but also modifies the slot cycle.
        if (d.defer_b_to_release()) {
            ms.bPressedAlone = true;
            return;
        }
        d.on_button_b();   // copy a selection / leave the browser / back out of the sample editor
        return;
    }

    // ⚠️ SELECT and START are checked before the "no modifiers" guard below: SELECT's own press sets
    // `m.select`, which that guard would reject.
    // ⚠️ SELECT ARMS and does not act — `on_select` fires on release (see the latch above).
    if (e.button == Button::SELECT && !m.l && !m.r && !m.a && !m.b) {
        ms.selectPressedAlone = true;
        return;
    }
    if (e.button == Button::START && !m.l && !m.r && !m.a && !m.b && !m.select) { d.on_start(); return; }

    // ── The D-pad, unmodified: move the cursor (or drag a selection's edge) ──────────────────────
    if (m.l || m.r || m.a || m.b || m.select) return;  // a modifier is down: not a plain press

    switch (e.button) {
        case Button::DPAD_UP:    d.on_dpad_up();    break;
        case Button::DPAD_DOWN:  d.on_dpad_down();  break;
        case Button::DPAD_LEFT:  d.on_dpad_left();  break;
        case Button::DPAD_RIGHT: d.on_dpad_right(); break;
        default: break;
    }
}

}  // namespace pt::ui
