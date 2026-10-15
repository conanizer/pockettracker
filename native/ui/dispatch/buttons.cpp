// The plain buttons — A, B, SELECT, START — and LIVE mode's two chords.

#include "ui/dispatch/dispatch_common.h"

#include "songcore/traversal.h"
#include "ui/song_pointer.h"

namespace pt::ui {

// ─── The plain buttons ───────────────────────────────────────────────────────────────────────────

// A and B are the screen's: every screen's handler answers them (ui/dispatch/screens/).
void InputDispatcher::on_button_a() { route(Gesture::A); }
void InputDispatcher::on_button_b() { route(Gesture::B); }

void InputDispatcher::on_select() {
    if (route(Gesture::SELECT)) return;
    // ⚠️ Bare SELECT is HELP, and arrives on the RELEASE (ui/button_mapper.h): on the browser SELECT is a
    // modifier, and any other press cancels this. FULL opens the overlay; SHORT toggles the compact
    // panel, which has no room on the FILE BROWSER.
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
    // ⚠️ The compact panel closes and the press still does its job; the FULL overlay's press is consumed
    // by the mapper. Both flags go, so the two are never up together.
    s_.helpOpen = false;
    s_.helpFull = false;
}

// Each screen that can start an audition stops its own — under the confirm and the FX picker too.
void InputDispatcher::on_stop_preview() { route(Gesture::STOP_PREVIEW); }

void InputDispatcher::on_start() {
    if (route(Gesture::START)) return;
    // The transport — also under the THEME and EQ editors, so an edit is heard while it is dialled.
    if (host_.is_playing()) {
        host_.stop();
        return;
    }
    switch (s_.currentScreen) {
        // SONG plays from the cursor row.
        case ScreenType::SONG:  host_.play_song(s_.cursorRow); break;

        // CHAIN plays on its own track (fader, mute, FX), not channel 1; the remembered song cell breaks a
        // tie.
        case ScreenType::CHAIN:
            host_.play_chain(s_.currentChain,
                             songcore::track_of_chain(*s_.project, s_.currentChain,
                                                      remembered_song_track()));
            break;

        // No song cursor: the song from the top. MIDI too — its OFFSET and the mapping list's VAL are
        // dialled against a playing song.
        case ScreenType::MIXER:
        case ScreenType::EFFECTS:
        case ScreenType::PROJECT:
        case ScreenType::SETTINGS:
        case ScreenType::MIDI:
        case ScreenType::MIDI_MAP: host_.play_song(0); break;

        // The rest play the phrase, through the chain on screen (one phrase may sit in five chains).
        default:
            host_.play_phrase(s_.currentPhrase,
                              songcore::track_of_phrase(*s_.project, s_.currentPhrase, s_.currentChain,
                                                        remembered_song_track()));
            break;
    }
}

// LIVE mode's chords: SONG's handler answers them. L+START is reserved everywhere else.
void InputDispatcher::on_l_start() { route(Gesture::L_START); }

// Below SONG, where START plays one chain or phrase, R+START plays every track from the song row you
// came through. ⚠️ A list, not a default: the chord reaches the browser and the sample editor too.
void InputDispatcher::on_r_start() {
    if (route(Gesture::R_START)) return;
    switch (s_.currentScreen) {
        case ScreenType::CHAIN:
        case ScreenType::PHRASE:
        case ScreenType::TABLE:
        case ScreenType::GROOVE:
        case ScreenType::SCALE:
        case ScreenType::INSTRUMENT:
        case ScreenType::INST_POOL:
        case ScreenType::MODS:
            if (host_.is_playing()) {
                host_.stop();
            } else if (host_.live_mode()) {
                // LIVE has no "all eight": the row launches as a scene, an empty cell silent.
                host_.play_song_live(pointer_song_row(s_), live_row_mask(pointer_song_row(s_)));
            } else {
                host_.play_song(pointer_song_row(s_));
            }
            break;
        default:
            break;
    }
}

void InputDispatcher::on_a_start() { route(Gesture::A_START); }

}  // namespace pt::ui
