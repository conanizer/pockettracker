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
    // modifier, and any other press cancels this.
    s_.helpFull = true;
}

// ⚠️ The press that closes it is consumed by the mapper: the overlay covers the cells.
void InputDispatcher::on_help_dismiss() { s_.helpFull = false; }

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
        case ScreenType::SONG:  host_.play_song(s_.songCursor.row); break;

        // CHAIN plays from the cursor row, on its own track (fader, mute, FX), not channel 1; the
        // remembered song cell breaks a tie.
        case ScreenType::CHAIN:
            host_.play_chain(s_.currentChain,
                             songcore::track_of_chain(*s_.project, s_.currentChain,
                                                      pointer_track(s_)),
                             s_.chainCursor.row);
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
                                                        pointer_track(s_)));
            break;
    }
}

// SONG answers L+START (queue the row, in LIVE or not) and LIVE's R+START; reserved everywhere else.
void InputDispatcher::on_l_start() { route(Gesture::L_START); }

// How far into the song row you came through the CHAIN or PHRASE on screen sits: whole phrases before
// its chain row. 0 when the screen was not reached through that song cell.
static int steps_into_song_row(const AppState& s) {
    if (s.currentScreen != ScreenType::CHAIN && s.currentScreen != ScreenType::PHRASE) return 0;
    const songcore::Project& p = *s.project;
    if (s.currentChain < 0 ||
        songcore::chain_at(p, pointer_track(s), s.songCursor.row) != s.currentChain) return 0;
    const int row = std::clamp(s.chainCursor.row, 0, songcore::CHAIN_ROWS - 1);
    if (s.currentScreen == ScreenType::PHRASE &&
        songcore::phrase_at(p, s.currentChain, row) != s.currentPhrase) return 0;
    const songcore::Chain& chain = p.chains[static_cast<size_t>(s.currentChain)];
    int phrases = 0;
    for (int r = 0; r < row; ++r)
        if (!songcore::chain_is_empty(chain, r)) ++phrases;
    return phrases * 16;
}

// Below SONG, where START plays one chain or phrase, R+START plays every track from the song row you
// came through — on CHAIN and PHRASE from that chain row's phrase, the other tracks as far in.
// ⚠️ A list, not a default: the chord reaches the browser and the sample editor too.
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
                host_.play_song_live(s_.songCursor.row, live_row_mask(s_.songCursor.row),
                                     steps_into_song_row(s_));
            } else {
                host_.play_song(s_.songCursor.row, steps_into_song_row(s_));
            }
            break;
        default:
            break;
    }
}

void InputDispatcher::on_a_start() { route(Gesture::A_START); }

}  // namespace pt::ui
