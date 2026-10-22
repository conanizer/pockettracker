#pragma once

// The combo matrix: a button event plus the modifiers held at that instant → one named handler.
// A template so a test can drive it with a recording stub; in the app `Dispatcher` is `InputDispatcher`.

#include "ui/buttons.h"

#include <cstdint>

namespace pt::ui {

// The mapper's memory between events.
struct MapperState {
    uint64_t lastAPress = 0;   // the A,A window

    // A went down on a cell whose A opens something but whose A+DPAD / A+B means something else: the
    // open waits for A's release, and any A-combo meanwhile cancels it.
    bool aPressedAlone = false;

    // The same for B where it both closes and is a modifier (the EQ editor's slot cycle).
    bool bPressedAlone = false;

    // R+A / R+B changed the mix on the press; R released first keeps it, A/B first reverts it.
    bool rComboArmed = false;

    // SELECT went down alone; any other press clears it, so help fires on release only for a bare tap
    // (on the file browser SELECT is a modifier).
    bool selectPressedAlone = false;
};

// ⚠️ The order of the checks is the specification: most specific first, each arm returns. L+B+A before
// L+A, A+B before A, R+A / R+B before A and B.
// ⚠️ Modifiers come from the event, never a poll: a frame's events arrive at once, and a poll would
// see the frame's end (B-then-A in one frame would delete on the B press).
template <class Dispatcher>
void handle_button(const ButtonEvent& e, Dispatcher& d, MapperState& ms, uint64_t now) {
    const ButtonMods& m = e.mods;

    // R's state for MIDI learn ("hold R, turn a knob"). ⚠️ Above every early return, or R could stay held.
    if (e.button == Button::R_SHIFT) d.on_r_held(e.action == ButtonAction::PRESSED);
    // A press with R held is a combo (R+DPAD switches screens): that hold never learns.
    else if (e.action == ButtonAction::PRESSED && m.r) d.on_r_combo_press();
    // A's state for what is drawn while it is held. Above every early return too.
    if (e.button == Button::A) d.on_a_held(e.action == ButtonAction::PRESSED);

    // ── Release ──────────────────────────────────────────────────────────────────────────────────
    if (e.action != ButtonAction::PRESSED) {
        // The mute/solo chord ends on a release and consumes it (an armed A must not reach
        // `on_a_released()`). On the A/B release, `m.r` is "is R still down?".
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
            if (ms.aPressedAlone) {   // no A-combo intervened: the deferred A fires now
                ms.aPressedAlone = false;
                d.on_button_a();
            }
            d.on_a_released();        // the FX helper commits here
        }
        if (e.button == Button::B) {
            if (ms.bPressedAlone) {
                ms.bPressedAlone = false;
                d.on_button_b();
            }
        }
        if (e.button == Button::SELECT) {
            if (ms.selectPressedAlone) {
                ms.selectPressedAlone = false;
                d.on_select();
            }
        }
        return;
    }

    // The full help overlay: any press closes it and is consumed — it covers the editor, so a press
    // falling through would edit a cell nobody can see. An audition is still silenced.
    if (d.help_full_open()) {
        ms.selectPressedAlone = false;
        d.on_help_dismiss();
        if (e.button != Button::START && !m.a) d.on_stop_preview();
        return;
    }

    // Any press but SELECT puts the compact panel away and falls through — it swallows nothing.
    if (e.button != Button::SELECT) {
        ms.selectPressedAlone = false;   // SELECT is a modifier here, not a tap
        d.on_help_dismiss();
    }

    // Any plain press silences an audition; START and A-combos (edits stay audible) do not.
    if (e.button != Button::START && !m.a) d.on_stop_preview();

    // ── A + … ────────────────────────────────────────────────────────────────────────────────────
    if (m.a && !m.l && !m.r) {
        switch (e.button) {
            // Each cancels the deferred A: it was a combo.
            case Button::B:          ms.aPressedAlone = false; d.on_a_b();     return;   // delete / reset
            case Button::DPAD_UP:    ms.aPressedAlone = false; d.on_a_up();    return;   // +16 / +1 octave (or the FX helper)
            case Button::DPAD_DOWN:  ms.aPressedAlone = false; d.on_a_down();  return;   // −16 / −1 octave
            case Button::DPAD_RIGHT: ms.aPressedAlone = false; d.on_a_right(); return;   // +1
            case Button::DPAD_LEFT:  ms.aPressedAlone = false; d.on_a_left();  return;   // −1
            case Button::START:      ms.aPressedAlone = false; d.on_a_start(); return;   // audition
            default: break;
        }
    }

    // ── SELECT + … : the file browser's chords ───────────────────────────────────────────────────
    // ⚠️ The mods include the button being pressed: on SELECT+R, R's press has already set `m.r`.
    if (m.select && !m.l && (!m.r || e.button == Button::R_SHIFT)) {
        switch (e.button) {
            case Button::A:       d.on_select_a(); return;   // rename     (opens the keyboard)
            case Button::B:       d.on_select_b(); return;   // delete     (arms the confirm)
            case Button::R_SHIFT: d.on_select_r(); return;   // new folder (opens the keyboard)
            default: break;
        }
    }

    // ── B + DPAD: which item ─────────────────────────────────────────────────────────────────────
    if (m.b && !m.l && !m.r && !m.a) {
        switch (e.button) {
            // Each cancels the deferred B, or B+RIGHT in the EQ editor would close it on release.
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
            case Button::SELECT:   // reserved: consumed, not passed down
            case Button::A:
            case Button::B:       return;
            case Button::L_SHIFT:
            case Button::R_SHIFT: d.on_l_r(); return;
            default: break;
        }
    }

    // ── L+B+A: clone — before L+A, which would paste ─────────────────────────────────────────────
    if (m.l && m.b && !m.r && e.button == Button::A) {
        d.on_l_b_a();
        return;
    }

    // ── L + … : selection and the clipboard ──────────────────────────────────────────────────────
    if (m.l && !m.r) {
        switch (e.button) {
            case Button::A:     d.on_l_a(); return;   // cut (in a selection) / paste (outside one)
            case Button::B:     d.on_l_b(); return;   // enter selection, then widen it
            case Button::START: d.on_l_start(); return;   // LIVE's row queue; never play/stop
            default: break;
        }
        // L + a direction fills rows — L alone, so a held clipboard chord cannot fire it.
        if (!m.a && !m.b && !m.select) {
            switch (e.button) {
                case Button::DPAD_UP:    d.on_l_up();    return;
                case Button::DPAD_DOWN:  d.on_l_down();  return;
                case Button::DPAD_LEFT:  d.on_l_left();  return;
                case Button::DPAD_RIGHT: d.on_l_right(); return;
                default: break;
            }
        }
    }

    // ── R + DPAD: move between screens ───────────────────────────────────────────────────────────
    if (m.r && !m.l) {
        switch (e.button) {
            case Button::DPAD_UP:    d.on_r_up();    return;
            case Button::DPAD_DOWN:  d.on_r_down();  return;
            case Button::DPAD_LEFT:  d.on_r_left();  return;
            case Button::DPAD_RIGHT: d.on_r_right(); return;
            // MUTE and SOLO. ⚠️ Both arm the latch even where nothing happens: a chord armed on SONG
            // and released on MIXER must still be closed out.
            case Button::A:          d.on_r_a(); ms.rComboArmed = true; return;
            case Button::B:          ms.bPressedAlone = false; d.on_r_b(); ms.rComboArmed = true; return;
            // ⚠️ R arriving second: half the time B lands first. Where R+B mutes, B is held
            // (`defer_b_to_release`) so it can still be claimed here — `bPressedAlone`, not `m.b`,
            // because a B that already acted must not also be a chord.
            case Button::R_SHIFT:
                if (ms.bPressedAlone) {
                    ms.bPressedAlone = false;
                    d.on_r_b();
                    ms.rComboArmed = true;
                }
                return;
            case Button::START:      d.on_r_start(); return;   // LIVE's stop; the song from below SONG
            default: break;
        }
    }

    // ── A, and the A,A double-tap (300 ms) ───────────────────────────────────────────────────────
    if (e.button == Button::A && !m.l && !m.r) {
        // Defer first; a deferred cell is never a double-tap cell, so the window is cleared.
        if (d.defer_a_to_release()) {
            ms.aPressedAlone = true;
            ms.lastAPress    = 0;
            d.on_a_deferred();   // a deferred tap aimed at the playhead reads it now
            return;
        }
        if (now - ms.lastAPress < 300) {
            ms.lastAPress = 0;   // so a triple-tap is not two double-taps
            d.on_a_a();
        } else {
            ms.lastAPress = now;
            d.on_button_a();
        }
        return;
    }

    // ── B ────────────────────────────────────────────────────────────────────────────────────────
    if (e.button == Button::B && !m.l && !m.r && !m.a) {
        if (d.defer_b_to_release()) {
            ms.bPressedAlone = true;
            return;
        }
        d.on_button_b();
        return;
    }

    // Before the no-modifier guard: SELECT's own press sets `m.select`. SELECT only arms here.
    if (e.button == Button::SELECT && !m.l && !m.r && !m.a && !m.b) {
        ms.selectPressedAlone = true;
        return;
    }
    if (e.button == Button::START && !m.l && !m.r && !m.a && !m.b && !m.select) { d.on_start(); return; }

    // ── The plain D-pad ──────────────────────────────────────────────────────────────────────────
    if (m.l || m.r || m.a || m.b || m.select) return;

    switch (e.button) {
        case Button::DPAD_UP:    d.on_dpad_up();    break;
        case Button::DPAD_DOWN:  d.on_dpad_down();  break;
        case Button::DPAD_LEFT:  d.on_dpad_left();  break;
        case Button::DPAD_RIGHT: d.on_dpad_right(); break;
        default: break;
    }
}

}  // namespace pt::ui
