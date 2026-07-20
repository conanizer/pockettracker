#pragma once

// ─── THE COMBO MATRIX ────────────────────────────────────────────────────────────────────────────
//
// `ButtonEvent` in, one named dispatcher handler out. The C++ twin of `InputMapper.handleButtonAction`
// (input/ButtonHandlers.kt:402), and the first arrow of Android's InputMapper → ButtonHandlers →
// AppInputDispatcher chain.
//
// ── WHY THIS MOVED, AND WHY IT IS A TEMPLATE (convergence C0.1) ──────────────────────────────────
//
// It lived in `linux/main.cpp`'s anonymous namespace from Phase 3 S1 until convergence C0, where it
// had two properties that were fine for a Linux port and are not fine for a shared shell:
//
//   • **it existed in exactly one copy, in a file Android cannot link.** C1 gives Android the same
//     SDL shell, and the identical matrix has to run there. A second copy is the drift Phase 3 spent
//     itself killing;
//   • ⚠️ **no tool could reach it.** `tools/ptdispatch` drives the dispatcher's *named handlers*
//     directly — one layer BELOW this function — so every assertion it makes starts after the
//     question this file answers. A block whose own comment says "the order of these checks IS the
//     specification" had no coverage of that order at all, on either side: Kotlin's
//     `handleButtonAction` is equally untooled (testdata/README.md §3 names it as an uncovered hole),
//     which is why there is no golden to compare against and `tools/ptmapper` asserts instead.
//
// The template parameter is what fixes the second one, and it costs nothing: `Dispatcher` is
// `InputDispatcher` in every shipping build (no virtual call, no indirection, the same code
// generated as when this took a concrete reference), and `tools/ptmapper` instantiates it with a stub
// that records which handler was called instead of doing it. That stub is the only reason the order
// below can be measured rather than re-read.
//
// ⚠️ **A `Dispatcher` must provide every `on_*` named below plus `defer_a_to_release()` and
// `defer_b_to_release()`.** A template only type-checks what it instantiates, so a typo here is
// caught by the shell's own instantiation (which is a full build, every session) and by ptmapper's.

#include "ui/buttons.h"

#include <cstdint>

namespace pt::ui {

/**
 * The mapper's own memory — everything the combo matrix must remember BETWEEN events, and nothing
 * else. Kotlin keeps the same two fields on `InputMapper` itself.
 */
struct MapperState {
    /** The A,A double-tap window. */
    uint64_t lastAPress = 0;

    /**
     * ⚠️ The DEFERRED-A latch (`InputMapper.aPressedAlone`). Set when A goes down on a cell whose A
     * OPENS something — and whose A+DPAD/A+B means something ELSE. The open is then held until A is
     * RELEASED, and CANCELLED outright if any A-combo fires in between.
     *
     * Without it, holding A on the INSTRUMENT NAME cell and pressing B to reset it would open the
     * keyboard first and land the combo on top of it. The dispatcher decides which cells qualify
     * (`defer_a_to_release()`); the mapper owns the latch, because it is the mapper that sees the
     * press, the combo and the release as one gesture.
     */
    bool aPressedAlone = false;

    /**
     * ⚠️ The DEFERRED-B latch (`InputMapper.bPressedAlone`), the exact mirror of the above, and it
     * exists for exactly one screen: the EQ EDITOR (S8).
     *
     * There, B is BOTH the close AND the modifier of the slot cycle (B+LEFT/RIGHT walks the 128-preset
     * bank). Fire the close on B's own press and the cycle is unreachable — you would be back on the
     * mixer before LEFT ever arrived. So the close is held until B is RELEASED, and CANCELLED if any
     * B-combo fires in between.
     */
    bool bPressedAlone = false;
};

/**
 * The COMBO MATRIX — a 1:1 port of `InputMapper.handleButtonAction`, and the last piece of the input
 * chain that is allowed to be platform-specific.
 *
 * Android's chain is InputMapper → ButtonHandlers → AppInputDispatcher. This function is the first
 * arrow: it owns nothing but the question "which of the ~30 named handlers does this press mean?",
 * and it answers it from the button plus the modifiers held at the instant it happened. Everything
 * downstream — what a handler DOES — is `pt::ui::InputDispatcher`, which is portable C++ and has never
 * heard of SDL.
 *
 * ⚠️ **THE ORDER OF THESE CHECKS IS THE SPECIFICATION.** They run most-specific-first, and each arm
 * RETURNS. L+B+A must be tested before L+A or a clone would paste; A+B before a plain A or a delete
 * would insert; R+A is consumed and does nothing, so that holding R to change screens cannot also fire
 * an edit underneath. Reordering them silently changes what the tracker does.
 *
 * ⚠️ **The modifiers come from the EVENT, never from `input.is_held()`.** See ui/buttons.h: SDL delivers
 * a frame's worth of events at once, so a poll-time read describes the end of the frame rather than the
 * instant of each event — and B-then-A inside one 16 ms frame would fire A+B (delete) on the B press.
 */
template <class Dispatcher>
void handle_button(const ButtonEvent& e, Dispatcher& d, MapperState& ms, uint64_t now) {
    const ButtonMods& m = e.mods;

    // ── RELEASE ──────────────────────────────────────────────────────────────────────────────────
    if (e.action != ButtonAction::PRESSED) {
        if (e.button == Button::A) {
            // The DEFERRED single-A: it went down on a sub-screen-opening cell and no A-combo
            // intervened, so the open fires NOW, on the release, rather than on the press.
            if (ms.aPressedAlone) {
                ms.aPressedAlone = false;
                d.on_button_a();
            }
            // The FX helper commits on the RELEASE of A — which is what lets you hold A, read your way
            // through the effect grid, and let go on the one you want.
            d.on_a_released();
        }
        if (e.button == Button::B) {
            // The DEFERRED single-B: it went down inside the EQ editor and no B-combo intervened, so
            // the CLOSE fires now rather than on the press. Mirror of the A latch above.
            if (ms.bPressedAlone) {
                ms.bPressedAlone = false;
                d.on_button_b();
            }
        }
        return;
    }

    // Silence a ringing audition on any "plain" press. START is exempt (it starts playback), and so is
    // anything pressed while A is held — that covers every edit combo, and an edit should stay audible.
    if (e.button != Button::START && !m.a) d.on_stop_preview();

    // ── A + … : the tracker's core editing gesture ───────────────────────────────────────────────
    if (m.a && !m.l && !m.r) {
        switch (e.button) {
            // ⚠️ Every arm CANCELS the deferred A first: the gesture turned out to be a combo, so the
            // open it was holding must not fire when A comes back up.
            case Button::B:          ms.aPressedAlone = false; d.on_a_b();     return;   // delete / reset
            case Button::DPAD_UP:    ms.aPressedAlone = false; d.on_a_up();    return;   // +1 (or the FX helper)
            case Button::DPAD_DOWN:  ms.aPressedAlone = false; d.on_a_down();  return;   // −1
            case Button::DPAD_RIGHT: ms.aPressedAlone = false; d.on_a_right(); return;   // +16 / +1 octave
            case Button::DPAD_LEFT:  ms.aPressedAlone = false; d.on_a_left();  return;   // −16 / −1 octave
            default: break;
        }
    }

    // ── SELECT + … : the file browser's file-management chords ───────────────────────────────────
    //
    // ⚠️ `(!m.r || e.button == R_SHIFT)`, not `!m.r`, and this is `ButtonHandlers.kt:621` to the
    // character. **The mods snapshot INCLUDES the button being pressed** (that is what makes the L+R
    // arm below work at all, and why the plain-SELECT arm has to be checked ahead of the no-modifier
    // guard). So on the SELECT+R chord, R's own press has already set `m.r` — a flat `!m.r` would
    // reject the very gesture it is trying to match. R may be held here only when R IS the button.
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
            // ⚠️ Every arm CANCELS the deferred B, for the same reason the A-combos cancel the deferred
            // A: the gesture turned out to be a combo, so the CLOSE it was holding must not fire when B
            // comes back up. Without this, cycling the EQ slot with B+RIGHT would shut the editor the
            // moment you let go of B.
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
            // Reserved chords: consumed so they cannot fall through to a single-button handler
            // mid-chord and do something the user never asked for.
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
            case Button::START: return;               // reserved — START must not toggle playback here
            default: break;                           // L+DPAD is the file browser's; it has no screen yet
        }
    }

    // ── R + DPAD: move between screens ───────────────────────────────────────────────────────────
    if (m.r && !m.l) {
        switch (e.button) {
            case Button::DPAD_UP:    d.on_r_up();    return;
            case Button::DPAD_DOWN:  d.on_r_down();  return;
            case Button::DPAD_LEFT:  d.on_r_left();  return;
            case Button::DPAD_RIGHT: d.on_r_right(); return;
            // Reserved, and consumed: R is held to navigate, and a stray A must not edit underneath it.
            case Button::A:
            case Button::B:
            case Button::START:      return;
            default: break;
        }
    }

    // ── A, and the A,A double-tap ────────────────────────────────────────────────────────────────
    // 300 ms. The window belongs to the MAPPER on Android (`InputMapper.lastAPress`), so it lives
    // here rather than in the dispatcher — and it is passed in rather than kept in a function-local
    // static, because a mapper that hides its own state cannot be driven by a test.
    if (e.button == Button::A && !m.l && !m.r) {
        // ⚠️ The DEFER, first: on a cell whose A opens a sub-screen, hold the action until A comes back
        // up, so an A+DPAD or A+B on the same cell is not pre-empted by an immediate open. A deferred
        // cell is never a double-tap cell — there is nothing there to insert — so the A,A path below is
        // skipped for it, and `lastAPress` is cleared so the NEXT A press cannot read as the second
        // half of a double-tap that never happened.
        if (d.defer_a_to_release()) {
            ms.aPressedAlone = true;
            ms.lastAPress    = 0;
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
        // ⚠️ The DEFER, exactly as A's above: inside the EQ editor B is a CLOSE, but it is also the
        // modifier of the slot cycle — so hold it until B comes back up and let a B+DPAD cancel it.
        if (d.defer_b_to_release()) {
            ms.bPressedAlone = true;
            return;
        }
        d.on_button_b();   // copy a selection / leave the browser / back out of the sample editor
        return;
    }

    // ⚠️ SELECT and START are checked EXPLICITLY, ahead of the "no modifiers" guard below, and the
    // reason is easy to miss: pressing SELECT sets `m.select` — its own press would be swallowed by a
    // guard that rejects any modifier. Kotlin carries the same explicit check for the same reason.
    if (e.button == Button::SELECT && !m.l && !m.r && !m.a && !m.b) { d.on_select(); return; }
    if (e.button == Button::START && !m.l && !m.r && !m.a && !m.b && !m.select) { d.on_start(); return; }

    // ── The D-pad, unmodified: move the cursor (or drag a selection's edge) ──────────────────────
    if (m.l || m.r || m.a || m.b || m.select) return;  // any modifier down: not a plain press

    switch (e.button) {
        case Button::DPAD_UP:    d.on_dpad_up();    break;
        case Button::DPAD_DOWN:  d.on_dpad_down();  break;
        case Button::DPAD_LEFT:  d.on_dpad_left();  break;
        case Button::DPAD_RIGHT: d.on_dpad_right(); break;
        default: break;
    }
}

}  // namespace pt::ui
