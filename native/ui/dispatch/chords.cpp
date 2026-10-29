// The held-button chords: A, B, R and L with the D-pad, delete, MUTE and SOLO. The screen's handler is
// asked first (ui/dispatch/screens/); what is here runs when it passes the gesture on.

#include "ui/dispatch/dispatch_common.h"

#include "ui/mixer_cell_layout.h"
#include "ui/navigation.h"
#include "ui/song_pointer.h"

#include <algorithm>

namespace pt::ui {

// ─── A + D-pad ───────────────────────────────────────────────────────────────────────────────────

// ⚠️ `pt::ui::on_a_b` must stay qualified: unqualified, a member's lookup finds the member. The others
// match. The horizontal axis is the small step, the vertical the large one.

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
    // Nothing else starts a preview while A is down, so this silences the one A began.
    if (heldNotePreview_) {
        heldNotePreview_ = false;
        host_.stop_preview(/*cut=*/true);
    }
    insertHeld_ = false;
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
// ⚠️ The modals (browser, sample editor, EQ) answer R+DPAD themselves: a popup is not a grid cell, and
// falling through would leave its state live on another screen.

void InputDispatcher::on_r_up() {
    if (route(Gesture::R_UP)) return;
    go_to_screen(s_, navigate_up(nav_state_of(s_)));
    s_.selection.exit();   // a selection belongs to the screen it was made on
}

void InputDispatcher::on_r_down() {
    if (route(Gesture::R_DOWN)) return;
    go_to_screen(s_, navigate_down(nav_state_of(s_)));
    s_.selection.exit();
}

// ─── R+LEFT/R+RIGHT: carry the edited item across screens ────────────────────────────────────────
//
// SONG over chain 04 → R+RIGHT lands on chain 04: the departing cursor's ref becomes lastEdited and the
// arriving screen jumps to it. ⚠️ Horizontal moves only, and only when the screen changes.
void InputDispatcher::sync_last_edited_on_screen_switch(ScreenType from, ScreenType to) {
    const Project& p = *s_.project;

    switch (from) {
        case ScreenType::PHRASE:
            // currentScreen is still PHRASE, so cursor_context() is its cell. A -1 is clamped on arrival.
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
    const NavResult r = navigate_left(nav_state_of(s_));
    if (r.screen != s_.currentScreen) sync_last_edited_on_screen_switch(s_.currentScreen, r.screen);
    go_to_screen(s_, r);
    s_.selection.exit();
}

void InputDispatcher::on_r_right() {
    if (route(Gesture::R_RIGHT)) return;
    const NavResult r = navigate_right(nav_state_of(s_));
    // ⚠️ The NAV = SONG gate before the sync: a refused press must leave nothing behind.
    if (!song_relative_entry_allowed(s_, r.screen)) return;
    if (r.screen != s_.currentScreen) sync_last_edited_on_screen_switch(s_.currentScreen, r.screen);
    go_to_screen(s_, r);
    s_.selection.exit();
}

// ─── L+B / L+A: the selection and the clipboard — only the four grid screens answer them ─────────

void InputDispatcher::on_l_b() { route(Gesture::L_B); }

// ─── L + D-pad: fill rows — PHRASE answers it ────────────────────────────────────────────────────

void InputDispatcher::on_l_up()    { route(Gesture::L_UP); }
void InputDispatcher::on_l_down()  { route(Gesture::L_DOWN); }
void InputDispatcher::on_l_left()  { route(Gesture::L_LEFT); }
void InputDispatcher::on_l_right() { route(Gesture::L_RIGHT); }
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
            // ⚠️ The row is part of the address (row 1 holds the REV and DEL returns); the master
            // has no mute.
            switch (mixer_cell_at(s_.mixerMasterRow, s_.mixerCursorColumn)) {
                case MixerCell::TRACK_VOL: out[count++] = s_.mixerCursorColumn;      break;
                case MixerCell::REV_WET:   out[count++] = songcore::MIX_CH_REVERB;   break;
                case MixerCell::DLY_WET:   out[count++] = songcore::MIX_CH_DELAY;    break;
                case MixerCell::MASTER_VOL:
                case MixerCell::MASTER_EQ:
                case MixerCell::MASTER_FX:
                case MixerCell::LIMITER:
                case MixerCell::NONE:      break;
            }
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

    // ⚠️ Snapshot on the chord's FIRST toggle only, so a revert undoes the whole chord.
    if (!mixSnapshot_.live) {
        for (int ch = 0; ch < MIX_CHANNELS; ++ch)
            if (const bool* m = songcore::mix_channel_mute(p, ch)) mixSnapshot_.mute[ch] = *m;
        mixSnapshot_.dry  = p.dryMute;
        mixSnapshot_.live = true;
    }

    if (!solo) {
        for (int i = 0; i < count; ++i)
            if (bool* m = songcore::mix_channel_mute(p, targets[i])) *m = !*m;
    } else {
        // The targets' kind is the eight tracks, or the two returns. A return heard alone also turns
        // the tracks' own sound off; they keep feeding it.
        const bool returns = targets[0] >= songcore::MIX_CH_REVERB;
        const int  first   = returns ? songcore::MIX_CH_REVERB : 0;
        const int  last    = returns ? songcore::MIX_CH_DELAY : 7;
        bool on[MIX_CHANNELS] = {};
        for (int i = 0; i < count; ++i) on[targets[i]] = true;

        bool already = p.dryMute == returns;
        for (int ch = first; ch <= last; ++ch)
            if (const bool* m = songcore::mix_channel_mute(p, ch)) already = already && *m == !on[ch];
        for (int ch = first; ch <= last; ++ch)
            if (bool* m = songcore::mix_channel_mute(p, ch)) *m = !already && !on[ch];
        p.dryMute = !already && returns;
    }

    s_.lastClearable = AppState::Clearable::MUTE;

    // ⚠️ Not an edit: no dirty, no autosave. All eight tracks are pushed — R+A changes the others.
    host_.push_globals();
}

void InputDispatcher::restore_full_playback() {
    Project& p = host_.edit_project();
    for (songcore::Track& t : p.tracks) t.mute = false;
    p.reverbMute = p.delayMute = p.dryMute = false;
    s_.lastClearable = AppState::Clearable::NONE;
    host_.push_globals();
}

void InputDispatcher::on_r_b() {
    if (route(Gesture::R_B)) return;
    if (fx_off_chord_live()) { toggle_fx_off(); return; }
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
    mixSnapshot_.live   = false;
    fxOffSnapshot_.live = false;
}

void InputDispatcher::on_r_combo_revert() {
    if (fxOffSnapshot_.live) {
        fxOffSnapshot_.live = false;
        Project& p = host_.edit_project();
        auto restore = [&](auto& rows) {
            for (size_t r = 0; r < rows.size() && r < 16; ++r)
                for (int slot = 1; slot <= 3; ++slot)
                    songcore::set_fx_off(rows[r], slot, fxOffSnapshot_.off[r][slot - 1]);
        };
        if (fxOffSnapshot_.table) restore(p.tables[static_cast<size_t>(fxOffSnapshot_.id)].rows);
        else                      restore(p.phrases[static_cast<size_t>(fxOffSnapshot_.id)].steps);
        mark_modified(/*table_touched=*/fxOffSnapshot_.table);
    }
    if (!mixSnapshot_.live) return;   // the chord armed on a screen that has no channels
    Project& p = host_.edit_project();
    for (int ch = 0; ch < MIX_CHANNELS; ++ch)
        if (bool* m = songcore::mix_channel_mute(p, ch)) *m = mixSnapshot_.mute[ch];
    p.dryMute         = mixSnapshot_.dry;
    mixSnapshot_.live = false;
    host_.push_globals();
}

// ─── R+B on PHRASE / TABLE: switch commands off ──────────────────────────────────────────────────

bool InputDispatcher::fx_off_chord_live() const {
    if (top_layer()) return false;
    return s_.currentScreen == ScreenType::PHRASE || s_.currentScreen == ScreenType::TABLE;
}

void InputDispatcher::toggle_fx_off() {
    // A selection made earlier in this batch of events must count as older than this toggle.
    run_selection_recency();

    const bool onTable = s_.currentScreen == ScreenType::TABLE;
    const int  id      = onTable ? s_.currentTable : s_.currentPhrase;
    // FX1's name column; each slot is a name and a value column, and either one names the slot.
    const int  fx1Col  = onTable ? 3 : 4;

    int top  = onTable ? s_.tableCursorRow    : s_.cursorRow;
    int left = onTable ? s_.tableCursorColumn : s_.cursorColumn;
    int bottom = top, right = left;
    if (s_.selection.active) {
        const SelectionBounds b = s_.selection.bounds();
        top = b.topLeftRow;  bottom = b.bottomRightRow;
        left = b.topLeftColumn; right = b.bottomRightColumn;
    }

    Project& p = host_.edit_project();
    auto flip = [&](auto& rows) {
        bool any = false;
        for (int r = std::max(0, top); r <= bottom && r < static_cast<int>(rows.size()) && r < 16; ++r) {
            for (int slot = 1; slot <= 3; ++slot) {
                const int nameCol = fx1Col + 2 * (slot - 1);
                if (nameCol + 1 < left || nameCol > right) continue;
                auto& row = rows[static_cast<size_t>(r)];
                if (songcore::row_fx_type(row, slot) == songcore::FX_NONE) continue;   // nothing to switch

                // ⚠️ Snapshot before the first flip, so the B release can put everything back.
                if (!fxOffSnapshot_.live) {
                    fxOffSnapshot_ = FxOffSnapshot{true, onTable, id, {}};
                    for (size_t k = 0; k < rows.size() && k < 16; ++k)
                        for (int sl = 1; sl <= 3; ++sl)
                            fxOffSnapshot_.off[k][sl - 1] = songcore::fx_off(rows[k], sl);
                }
                songcore::set_fx_off(row, slot, !songcore::fx_off(row, slot));
                any = true;
            }
        }
        return any;
    };
    const bool any = onTable ? flip(p.tables[static_cast<size_t>(id)].rows)
                             : flip(p.phrases[static_cast<size_t>(id)].steps);
    if (!any) return;
    s_.lastClearable = AppState::Clearable::FX_OFF;
    mark_modified(/*table_touched=*/onTable);
}

bool InputDispatcher::open_grid_has_fx_off() const {
    const auto any_off = [](const auto& rows) {
        for (const auto& r : rows)
            if (r.fx1Off || r.fx2Off || r.fx3Off) return true;
        return false;
    };
    const Project& p = *s_.project;
    if (s_.currentScreen == ScreenType::PHRASE) return any_off(p.phrases[static_cast<size_t>(s_.currentPhrase)].steps);
    if (s_.currentScreen == ScreenType::TABLE)  return any_off(p.tables[static_cast<size_t>(s_.currentTable)].rows);
    return false;
}

void InputDispatcher::clear_open_grid_fx_off() {
    const auto clear = [](auto& rows) {
        for (auto& r : rows) r.fx1Off = r.fx2Off = r.fx3Off = false;
    };
    Project& p = host_.edit_project();
    const bool onTable = s_.currentScreen == ScreenType::TABLE;
    if (onTable) clear(p.tables[static_cast<size_t>(s_.currentTable)].rows);
    else         clear(p.phrases[static_cast<size_t>(s_.currentPhrase)].steps);
    s_.lastClearable = AppState::Clearable::NONE;
    mark_modified(/*table_touched=*/onTable);
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

bool InputDispatcher::mix_touched() {
    Project& p = host_.edit_project();   // the resolver hands out pointers; nothing is written here
    for (int ch = 0; ch < MIX_CHANNELS; ++ch)
        if (const bool* m = songcore::mix_channel_mute(p, ch); m && *m) return true;
    return p.dryMute;
}

bool InputDispatcher::l_r_has_more() {
    return !lrRouted_ && (s_.selection.active || mix_touched() || open_grid_has_fx_off());
}

void InputDispatcher::on_l_r() {
    lrRouted_ = route(Gesture::L_R);
    if (lrRouted_) return;

    // One press clears one thing, newest first: the mix, or the selection with its buffer. ⚠️ A rung
    // with nothing to clear falls through to the other, or L+R reads as dead.
    const bool mix_touched = this->mix_touched();

    // On PHRASE and TABLE the switched-off commands of the open one; another phrase's stay as they are.
    const bool fx_touched = open_grid_has_fx_off();

    if (s_.lastClearable == AppState::Clearable::FX_OFF && fx_touched) {
        clear_open_grid_fx_off();
        return;
    }
    if (s_.lastClearable == AppState::Clearable::MUTE && mix_touched) {
        restore_full_playback();
        return;
    }

    // Inside a selection: leave it, keep the buffer. Outside: clear the buffer (the top strip's readout).
    if (s_.selection.active) {
        s_.selection.exit();   // buffer untouched
        return;
    }

    const bool had_buffer = !clip_.info().empty();
    clip_.clear();

    // Nothing to clear here, but the commands or the mix have something: take it, the screen's own first.
    if (had_buffer) return;
    if (fx_touched)       clear_open_grid_fx_off();
    else if (mix_touched) restore_full_playback();
}

// ─── SELECT + A / B / R: the browser's file chords — only its handler answers them ───────────────

void InputDispatcher::on_select_a() { route(Gesture::SELECT_A); }
void InputDispatcher::on_select_b() { route(Gesture::SELECT_B); }
void InputDispatcher::on_select_r() { route(Gesture::SELECT_R); }

// ─── L+B+A: clone — SONG, CHAIN and PHRASE answer it ─────────────────────────────────────────────

void InputDispatcher::on_l_b_a() {
    if (route(Gesture::L_B_A)) return;
    // Below the sites: on every screen the chord ends the selection, after any clone.
    s_.selection.exit();
}

}  // namespace pt::ui
