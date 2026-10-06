// The plain buttons — A, B, SELECT, START — and LIVE mode's two chords.

#include "ui/dispatch/dispatch_common.h"

#include "songcore/traversal.h"

#include <algorithm>

namespace pt::ui {

// ─── The plain buttons ───────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_button_a() {
    if (route(Gesture::A)) return;

    // A on a cell that OPENS a sub-screen — the two NAME rows and all five EQ cells. Before the per-screen
    // arms below.
    if (open_sub_screen_at_cursor(/*peek=*/false)) return;

    // The SCALE screen's SAVE / LOAD cells — a screen, not an overlay, so placed among the "A on a
    // button" arms.
    if (s_.currentScreen == ScreenType::SCALE) {
        scale_row_action();
        return;
    }

    // The GROOVE panel. ⚠️ Not the tick grid — a bare A there lays a step down (the insert arm below).
    if (s_.currentScreen == ScreenType::GROOVE && s_.grooveCursorColumn == GROOVE_COL_PANEL) {
        groove_row_action();
        return;
    }

    // A on the end-of-pattern marker lays a step down at the default tick count (the cell's own insert).
    // ⚠️ Guarded on EMPTY — otherwise a bare A on an existing tick would step it.
    if (s_.currentScreen == ScreenType::GROOVE && cursor_context().capabilities.isEmpty)
        generic_input(pt::ui::increment);
}

void InputDispatcher::on_button_b() {
    if (route(Gesture::B)) return;

    // ⚠️ EFFECTS' TIME row: B toggles DELAY SYNC (milliseconds ↔ note divisions). A gesture of its own,
    // because the cell's value means different things on either side (0x40 a length; 4 a 1/16 note).
    // B is otherwise free on this screen, so no release latch is needed.
    // delayTime is re-clamped into 0..B on the way IN: a free time of 0xF0 would index past the names.
    if (s_.currentScreen == ScreenType::EFFECTS &&
        s_.effectsCursorRow == EffectModule::ROW_DLY_TIME) {
        songcore::Project& p = host_.edit_project();
        p.delaySync = !p.delaySync;
        if (p.delaySync) p.delayTime = std::min(std::max(p.delayTime, 0), 11);
        mark_modified();
    }
}

void InputDispatcher::on_select() {
    if (route(Gesture::SELECT)) return;
    // ⚠️ Bare SELECT is HELP — nothing else.
    // ⚠️ It arrives on the RELEASE (ui/button_mapper.h): on the browser SELECT is a modifier, and any other
    // press during it cancels this. (Unrelated to the A-deferral, which keeps sub-screen cells editable.)
    // The EQ and theme editors let it through: they leave the oscilloscope strip drawn, so the panel has
    // a place, and their cell names (EQ FILL, Q, MTR BG) need it. ⚠️⚠️ The browser lets it through too —
    // safe only because this runs on an uninterrupted release.

    // ── HELP ─────────────────────────────────────────────────────────────────────────────────────
    //
    // SETTINGS > HELP: FULL is the overlay everywhere; SHORT is the compact panel, toggled by SELECT.
    // ⚠️ The FILE BROWSER has no box for the panel (it fills 640×480), so SHORT shows nothing there; the
    // SAMPLE EDITOR's waveform panel hosts it.
    switch (static_cast<HelpMode>(s_.settings.helpMode)) {
        case HelpMode::OFF:
            return;
        case HelpMode::FULL:
            s_.helpOpen = false;
            s_.helpFull = true;
            return;
        case HelpMode::SHORT:
            if (on_browser()) return;
            s_.helpOpen = !s_.helpOpen;
            return;
    }
}

void InputDispatcher::on_help_dismiss() {
    // ⚠️ The compact panel does NOT consume the press: it closes and the press does its job — the panel
    // covers no cell. ⚠️⚠️ The FULL overlay is the opposite (the mapper consumes it). Clearing both flags
    // here keeps them from ever being up together.
    s_.helpOpen = false;
    s_.helpFull = false;
}

// Each screen that can START an audition stops its own. ⚠️ It reaches them under the confirm and the FX
// picker too (their layers pass it on): a dialog over an audition must not leave the note hanging.
void InputDispatcher::on_stop_preview() { route(Gesture::STOP_PREVIEW); }

void InputDispatcher::on_start() {
    if (route(Gesture::START)) return;
    // ⚠️ Runs under the THEME and EQ editors too (their layers pass it on): the transport underneath is
    // how you hear an edit while dialling it.

    // START is the transport — except where a screen's handler took it: the instrument screens and TABLE
    // audition, LIVE mode on SONG launches.
    if (host_.is_playing()) {
        host_.stop();
        return;
    }
    switch (s_.currentScreen) {
        // ⚠️ SONG starts at the CURSOR ROW — "play from here".
        case ScreenType::SONG:  host_.play_song(s_.cursorRow); break;

        // ⚠️ CHAIN plays on the TRACK it belongs to — its fader, mute, voice slot and per-track FX — not
        // channel 1. The remembered song cell only breaks a tie among tracks that hold this chain.
        case ScreenType::CHAIN:
            host_.play_chain(s_.currentChain,
                             songcore::track_of_chain(*s_.project, s_.currentChain,
                                                      remembered_song_track()));
            break;

        // MIXER, EFFECTS, PROJECT and SETTINGS have no song cursor: they play the SONG from the top —
        // on the mixer you want the mix.
        case ScreenType::MIXER:
        case ScreenType::EFFECTS:
        case ScreenType::PROJECT:
        case ScreenType::SETTINGS:
        // MIDI too: its OFFSET is dialled by ear against a playing song. The mapping list likewise: its VAL
        // column only moves while something sounds.
        case ScreenType::MIDI:
        case ScreenType::MIDI_MAP: host_.play_song(0); break;

        // PHRASE, GROOVE, SCALE…: the phrase, asked through the chain on screen first (the same phrase may
        // sit in five chains).
        default:
            host_.play_phrase(s_.currentPhrase,
                              songcore::track_of_phrase(*s_.project, s_.currentPhrase, s_.currentChain,
                                                        remembered_song_track()));
            break;
    }
}

// LIVE mode's chords: SONG's handler answers them; everywhere else they are reserved and do nothing.
void InputDispatcher::on_l_start() { route(Gesture::L_START); }
void InputDispatcher::on_r_start() { route(Gesture::R_START); }

}  // namespace pt::ui
