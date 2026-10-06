// The held-button chords: A, B, R and L with the D-pad, delete, MUTE and SOLO. The screen's handler is
// asked first (ui/dispatch/screens/); what is here runs when it passes the gesture on.

#include "ui/dispatch/dispatch_common.h"

#include "ui/navigation.h"
#include "ui/song_pointer.h"

#include <algorithm>

namespace pt::ui {

// ─── A + D-pad ───────────────────────────────────────────────────────────────────────────────────

// ⚠️ `pt::ui::on_a_b` must stay qualified: inside a member, unqualified lookup finds the member of the
// same name first. The other handlers are qualified to match.
//
// ⚠️ The HORIZONTAL axis is the small step (±1), the VERTICAL the large one (±`largeStep`) — the
// split handheld-tracker users already have in their fingers. The screen handlers keep it too.

// A+DPAD is taken by the keyboard and by the browser's own handler, so an A held over either never
// edits a cell.

void InputDispatcher::on_a_up() {
    if (route(Gesture::A_UP)) return;
    selection_or_single(pt::ui::increment_fast);
}

void InputDispatcher::on_a_down() {
    if (route(Gesture::A_DOWN)) return;
    selection_or_single(pt::ui::decrement_fast);
}

void InputDispatcher::on_a_left() {
    if (route(Gesture::A_LEFT)) return;
    selection_or_single(pt::ui::decrement);
}

void InputDispatcher::on_a_right() {
    if (route(Gesture::A_RIGHT)) return;
    selection_or_single(pt::ui::increment);
}

void InputDispatcher::on_a_released() {
    // Ahead of the overlay test: nothing else can start a preview while A is down, so this silences
    // the one A began.
    if (heldNotePreview_) {
        heldNotePreview_ = false;
        host_.stop_preview(/*cut=*/true);
    }
    // Only a layer answers the release — the two pickers commit on it.
    route(Gesture::A_RELEASE);
}

void InputDispatcher::on_a_deferred() {
    // The mapper is holding this press; record the playhead now, since it will have moved by release.
    samplePending_.sliceTapPlayhead = on_slice_tap_cell() ? s_.sampleEditor.playbackPosition : -1.0f;
}

// ─── A+B: delete / reset ─────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_a_b() {
    if (route(Gesture::A_B)) return;
    generic_input(pt::ui::on_a_b);
}

// ─── A,A: insert the next UNUSED item — SONG, CHAIN and PHRASE answer it ─────────────────────────

void InputDispatcher::on_a_a() { route(Gesture::A_A); }

// ─── B + D-pad: which item am I looking at? — each screen answers it ─────────────────────────────

void InputDispatcher::on_b_left()  { route(Gesture::B_LEFT); }
void InputDispatcher::on_b_right() { route(Gesture::B_RIGHT); }
void InputDispatcher::on_b_up()    { route(Gesture::B_UP); }
void InputDispatcher::on_b_down()  { route(Gesture::B_DOWN); }

// ─── R + D-pad: move between screens — except on the modals ──────────────────────────────────────
//
// ⚠️ On the modals R+DPAD is not navigation. The FILE BROWSER and SAMPLE EDITOR answer it in their own
// handlers (sort / zoom, up a directory / swallowed). EQ EDITOR: all four swallowed. None may fall through to `navigate_*` — a popup is not a cell in the screen grid, and
// the user would land on a screen with the popup's state still live.

void InputDispatcher::on_r_up() {
    if (route(Gesture::R_UP)) return;
    const NavState ns = nav_state_of(s_);
    go_to_screen(s_, navigate_up(ns));
    s_.selection.exit();   // a selection belongs to the screen it was made on
}

void InputDispatcher::on_r_down() {
    if (route(Gesture::R_DOWN)) return;
    const NavState ns = nav_state_of(s_);
    go_to_screen(s_, navigate_down(ns));
    s_.selection.exit();
}

// ─── R+LEFT/R+RIGHT: carry the edited item across screens ────────────────────────────────────────
//
// What makes SONG-over-chain-04 → R+RIGHT land ON chain 04. CAPTURE: the ref under the departing
// screen's cursor becomes the lastEdited memory (PHRASE asks whether the CELL is empty; CHAIN and SONG
// guard on `ref >= 0`). APPLY: the arriving screen jumps to the matching lastEdited item.
//
// ⚠️ Horizontal moves only, and only when the screen actually changes — R+UP/DOWN must not sync.
void InputDispatcher::sync_last_edited_on_screen_switch(ScreenType from, ScreenType to) {
    const Project& p = *s_.project;

    switch (from) {
        case ScreenType::PHRASE:
            // `currentScreen` is still the departing PHRASE, so cursor_context() describes its cell.
            // No `>= 0` guard on the instrument — the clamp on arrival makes -1 safe.
            if (!cursor_context().capabilities.isEmpty) {
                s_.lastEditedInstrument = p.phrases[static_cast<size_t>(s_.currentPhrase)]
                                              .steps[static_cast<size_t>(s_.cursorRow)]
                                              .instrument;
            }
            break;

        case ScreenType::CHAIN: {
            const int ref = p.chains[static_cast<size_t>(s_.currentChain)]
                                .phraseRefs[static_cast<size_t>(s_.cursorRow)];
            if (ref >= 0) s_.lastEditedPhrase = ref;
            break;
        }

        case ScreenType::SONG: {
            // The column is the track, 1-based; a track's chainRefs may be shorter than 256 rows.
            const auto& refs = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)].chainRefs;
            if (s_.cursorRow < static_cast<int>(refs.size()) &&
                refs[static_cast<size_t>(s_.cursorRow)] >= 0) {
                s_.lastEditedChain = refs[static_cast<size_t>(s_.cursorRow)];
            }
            break;
        }

        default:
            break;
    }

    switch (to) {
        case ScreenType::PHRASE: s_.currentPhrase = s_.lastEditedPhrase; break;
        case ScreenType::CHAIN:  s_.currentChain  = s_.lastEditedChain;  break;
        case ScreenType::INSTRUMENT: {
            // Clamp into the pool and mirror the clamped value back, so a captured -1 lands on 00.
            const int last          = static_cast<int>(p.instruments.size()) - 1;
            s_.currentInstrument    = std::min(last, std::max(0, s_.lastEditedInstrument));
            s_.lastEditedInstrument = s_.currentInstrument;
            break;
        }
        default:
            break;
    }
}

void InputDispatcher::on_r_left() {
    if (route(Gesture::R_LEFT)) return;
    const NavState ns = nav_state_of(s_);
    const NavResult r = navigate_left(ns);
    if (r.screen != s_.currentScreen) sync_last_edited_on_screen_switch(s_.currentScreen, r.screen);
    go_to_screen(s_, r);
    s_.selection.exit();
}

void InputDispatcher::on_r_right() {
    if (route(Gesture::R_RIGHT)) return;
    const NavState ns = nav_state_of(s_);
    const NavResult r = navigate_right(ns);
    // ⚠️ The NAV = SONG entry gate sits above the sync: a refused press must leave nothing behind, and
    // the sync writes the lastEdited memory. R+RIGHT is the only gated direction (ui/song_pointer.h).
    if (!song_relative_entry_allowed(s_, r.screen)) return;
    if (r.screen != s_.currentScreen) sync_last_edited_on_screen_switch(s_.currentScreen, r.screen);
    go_to_screen(s_, r);
    s_.selection.exit();
}

// ─── L+B / L+A: the selection and the clipboard — only the four grid screens answer them ─────────

void InputDispatcher::on_l_b() { route(Gesture::L_B); }
void InputDispatcher::on_l_a() { route(Gesture::L_A); }

// ─── R+A / R+B: MUTE and SOLO ────────────────────────────────────────────────────────────────────

void InputDispatcher::mute_solo_targets(int (&out)[8], int& count) const {
    count = 0;
    switch (s_.currentScreen) {
        case ScreenType::SONG: {
            // A selection makes the chord act on every channel it covers.
            if (s_.selection.active) {
                const SelectionBounds b = s_.selection.bounds();
                for (int col = b.topLeftColumn; col <= b.bottomRightColumn; ++col)
                    if (col >= 1 && col <= 8) out[count++] = col - 1;
                return;
            }
            if (s_.cursorColumn >= 1 && s_.cursorColumn <= 8) out[count++] = s_.cursorColumn - 1;
            return;
        }
        case ScreenType::MIXER:
            // ⚠️ The row is part of the address: row 1 puts the REV and DEL returns under the columns
            // of tracks 1-2. Column 8 is MASTER and has no mute — a no-op. The selection is not consulted.
            if (s_.mixerMasterRow == 0 && s_.mixerCursorColumn >= 0 && s_.mixerCursorColumn <= 7)
                out[count++] = s_.mixerCursorColumn;
            else if (s_.mixerMasterRow == 1 && s_.mixerCursorColumn == 0)
                out[count++] = songcore::MIX_CH_REVERB;
            else if (s_.mixerMasterRow == 1 && s_.mixerCursorColumn == 1)
                out[count++] = songcore::MIX_CH_DELAY;
            return;
        default:
            return;   // every other screen: the chord is the consumed no-op it has always been
    }
}

void InputDispatcher::toggle_mute_solo(bool solo) {
    // A selection made earlier in this batch of events must count as older than this toggle.
    run_selection_recency();

    int targets[8];
    int count = 0;
    mute_solo_targets(targets, count);
    if (count == 0) return;

    Project& p = host_.edit_project();

    // ⚠️ Snapshot all ten pairs, on the FIRST toggle of a chord only, so a revert undoes everything the
    // chord did, not just the last press.
    if (!mixSnapshot_.live) {
        for (int ch = 0; ch < MIX_CHANNELS; ++ch) {
            const songcore::MixChannelFlags f = songcore::mix_channel_flags(p, ch);
            if (!f.mute) continue;
            mixSnapshot_.mute[ch] = *f.mute;
            mixSnapshot_.solo[ch] = *f.solo;
        }
        mixSnapshot_.live = true;
    }

    for (int i = 0; i < count; ++i) {
        const songcore::MixChannelFlags f = songcore::mix_channel_flags(p, targets[i]);
        if (!f.mute) continue;   // a channel the project does not have
        bool& flag = solo ? *f.solo : *f.mute;
        flag = !flag;
    }

    s_.lastClearable = AppState::Clearable::MUTE;

    // ⚠️ No mark_dirty_and_arm_autosave(): muting is a performance action, not an edit. It must not
    // arm the autosave or make the song dirty. push_globals() sweeps all eight tracks because a solo
    // changes the other seven too.
    host_.push_globals();
}

void InputDispatcher::restore_full_playback() {
    Project& p = host_.edit_project();
    for (songcore::Track& t : p.tracks) { t.mute = false; t.solo = false; }
    p.reverbMute = p.reverbSolo = p.delayMute = p.delaySolo = false;
    s_.lastClearable = AppState::Clearable::NONE;
    host_.push_globals();
}

void InputDispatcher::on_r_b() {
    if (route(Gesture::R_B)) return;
    if (!mute_solo_chord_live()) return;
    toggle_mute_solo(/*solo=*/false);
}

void InputDispatcher::on_r_a() {
    if (route(Gesture::R_A)) return;
    if (!mute_solo_chord_live()) return;
    toggle_mute_solo(/*solo=*/true);
}

// Is R+A/R+B a MUTE/SOLO here? Asked by the chord and by the deferred B alike.
bool InputDispatcher::mute_solo_chord_live() const {
    if (top_layer()) return false;   // a modal owns the buttons while it is up
    return s_.currentScreen == ScreenType::SONG || s_.currentScreen == ScreenType::MIXER;
}

void InputDispatcher::on_r_combo_commit() {
    // R came up first, so what the chord did stands; the next chord takes a fresh snapshot.
    mixSnapshot_.live = false;
}

void InputDispatcher::on_r_combo_revert() {
    if (!mixSnapshot_.live) return;   // the chord armed on a screen that has no channels
    Project& p = host_.edit_project();
    for (int ch = 0; ch < MIX_CHANNELS; ++ch) {
        const songcore::MixChannelFlags f = songcore::mix_channel_flags(p, ch);
        if (!f.mute) continue;
        *f.mute = mixSnapshot_.mute[ch];
        *f.solo = mixSnapshot_.solo[ch];
    }
    mixSnapshot_.live = false;
    host_.push_globals();
}

unsigned InputDispatcher::selection_signature() const {
    unsigned h = clip_.has_data() ? 1u : 0u;
    h = h * 31u + static_cast<unsigned>(clip_.type());
    h = h * 31u + static_cast<unsigned>(clip_.width());
    h = h * 31u + static_cast<unsigned>(clip_.height());
    h = h * 31u + (s_.selection.active ? 1u : 0u);
    if (s_.selection.active) {
        const SelectionBounds b = s_.selection.bounds();
        h = h * 31u + static_cast<unsigned>(b.topLeftRow);
        h = h * 31u + static_cast<unsigned>(b.topLeftColumn);
        h = h * 31u + static_cast<unsigned>(b.bottomRightRow);
        h = h * 31u + static_cast<unsigned>(b.bottomRightColumn);
    }
    return h;
}

void InputDispatcher::run_selection_recency() {
    const unsigned sig = selection_signature();
    if (sig == selectionSig_) return;
    selectionSig_    = sig;
    s_.lastClearable = AppState::Clearable::SELECTION;
}

void InputDispatcher::on_l_r() {
    if (route(Gesture::L_R)) return;

    // One press undoes one thing, most recent first (`s_.lastClearable`): the mix (any channel muted
    // or soloed) or the selection with its buffer. ⚠️ A rung with nothing to clear falls through to
    // the other, or L+R reads as a dead button. The mix check covers all MIX_CHANNELS, so the REV and
    // DEL returns count.
    const bool mix_touched = [&] {
        Project& p = host_.edit_project();   // the resolver hands out pointers; nothing is written here
        for (int ch = 0; ch < MIX_CHANNELS; ++ch) {
            const songcore::MixChannelFlags f = songcore::mix_channel_flags(p, ch);
            if (f.mute && (*f.mute || *f.solo)) return true;
        }
        return false;
    }();

    if (s_.lastClearable == AppState::Clearable::MUTE && mix_touched) {
        restore_full_playback();
        return;
    }

    // Inside a selection: leave it, but keep the copy buffer. Outside one: clear the buffer — the only
    // way to dismiss the clipboard readout on the top strip.
    if (s_.selection.active) {
        s_.selection.exit();   // buffer untouched
        return;
    }

    // The readout is drawn on every screen, so the clear works everywhere.
    const bool had_buffer = !clip_.info().empty();
    clip_.clear();

    // Nothing on the selection rung to clear, but the mix has something: take it rather than leave
    // the press doing nothing at all.
    if (!had_buffer && mix_touched) restore_full_playback();
}

// ─── SELECT + A / B / R: the browser's file chords — only its handler answers them ───────────────

void InputDispatcher::on_select_a() { route(Gesture::SELECT_A); }
void InputDispatcher::on_select_b() { route(Gesture::SELECT_B); }
void InputDispatcher::on_select_r() { route(Gesture::SELECT_R); }

// ─── L+B+A: clone — SONG, CHAIN and PHRASE answer it ─────────────────────────────────────────────

void InputDispatcher::on_l_b_a() {
    if (route(Gesture::L_B_A)) return;
    // Below the sites, so every screen does it: the chord ends a selection — after SONG, CHAIN or
    // PHRASE has cloned.
    s_.selection.exit();
}

}  // namespace pt::ui
