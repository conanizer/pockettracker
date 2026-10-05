// The plain buttons — A, B, SELECT, START — and LIVE mode's two chords.

#include "ui/dispatch/dispatch_common.h"

#include "songcore/traversal.h"
#include "ui/navigation.h"
#include "ui/std_filesystem.h"

#include <algorithm>
#include <string>

namespace pt::ui {

namespace {

/** The chain row holds no phrase. */
bool chain_row_empty(const Chain& c, int row) { return c.phraseRefs[static_cast<size_t>(row)] == -1; }

}  // namespace

// ─── The plain buttons ───────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_button_a() {
    if (route(Gesture::A)) return;

    // A on a cell that OPENS a sub-screen — the two NAME rows and all five EQ cells. Before the per-screen
    // arms below.
    if (open_sub_screen_at_cursor(/*peek=*/false)) return;

    // INSTRUMENT's LOAD / SAVE / EDIT buttons and the pool's empty NAME slot. Not deferred (no A+DPAD to
    // protect), hence not in `open_sub_screen_at_cursor`.
    if (instrument_open_at_cursor()) return;

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

    // A on an EMPTY cell inserts the item last edited — so A lays down the last chain again, A,A a fresh
    // one.
    Project& p = host_.edit_project();
    hasInsertPos_ = false;

    switch (s_.currentScreen) {
        case ScreenType::PHRASE: {
            if (s_.cursorColumn != 1 || s_.selection.active) return;
            Phrase& ph = p.phrases[static_cast<size_t>(s_.currentPhrase)];
            PhraseEditorState ps{ph};
            ps.cursorRow    = s_.cursorRow;
            ps.cursorColumn = s_.cursorColumn;
            // A on an existing note inserts nothing, but holding it is how you listen.
            if (!phrase_.cursor_context(ps).capabilities.isEmpty) {
                preview_held_note();
                return;
            }

            songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];
            step.note       = s_.lastEditedNote;
            step.instrument = s_.lastEditedInstrument;
            step.volume     = s_.lastEditedVolume;
            mark_modified();

            // Arm A,A: a second press on this NOTE cell keeps the note and re-points it at the next FREE
            // instrument (as chain/song A,A advance the ref). Only the NOTE column arms.
            hasInsertPos_ = true;
            insertScreen_ = ScreenType::PHRASE;
            insertRow_    = s_.cursorRow;
            insertCol_    = s_.cursorColumn;

            preview_held_note();
            break;
        }

        case ScreenType::CHAIN: {
            Chain& chain = p.chains[static_cast<size_t>(s_.currentChain)];
            if (chain_row_empty(chain, s_.cursorRow)) {
                chain.phraseRefs[static_cast<size_t>(s_.cursorRow)]      = s_.lastEditedPhrase;
                chain.transposeValues[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedTranspose;
                mark_modified();
                hasInsertPos_ = true;   // arm A,A — a second press inserts the next UNUSED phrase
                insertScreen_ = ScreenType::CHAIN;
                insertRow_    = s_.cursorRow;
                insertCol_    = s_.cursorColumn;
            }
            break;
        }

        case ScreenType::SONG: {
            if (s_.selection.active) return;
            if (s_.cursorColumn < 1 || s_.cursorColumn > 8) return;
            songcore::Track& track = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)];
            while (static_cast<int>(track.chainRefs.size()) <= s_.cursorRow)
                track.chainRefs.push_back(-1);

            if (track.chainRefs[static_cast<size_t>(s_.cursorRow)] == -1) {
                track.chainRefs[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedChain;
                mark_modified();
                hasInsertPos_ = true;
                insertScreen_ = ScreenType::SONG;
                insertRow_    = s_.cursorRow;
                insertCol_    = s_.cursorColumn;
            }
            break;
        }

        // A on the end-of-pattern marker lays a step down at the default tick count (the cell's own insert).
        // ⚠️ Guarded on EMPTY — otherwise a bare A on an existing tick would step it.
        case ScreenType::GROOVE:
            if (cursor_context().capabilities.isEmpty) generic_input(pt::ui::increment);
            break;

        // The two screens whose rows are BUTTONS: A is the action.
        case ScreenType::PROJECT:  project_action();  break;
        case ScreenType::SETTINGS: settings_action(); break;
        case ScreenType::MIDI:     midi_action();     break;
        case ScreenType::MIDI_MAP: midi_map_action(); break;

        default:
            break;
    }
}

void InputDispatcher::on_button_b() {
    if (route(Gesture::B)) return;

    // ⚠️ B LEAVES SETTINGS. Its POSITION matters:
    //   • AFTER the modals — the THEME EDITOR, EQ editor or keyboard over it own B;
    //   • BEFORE the selection arm — B in a selection copies and returns, and SETTINGS has no clipboard,
    //     so the screen would be intermittently stuck.
    if (s_.currentScreen == ScreenType::SETTINGS) {
        s_.selection.exit();
        NavResult nav;
        nav.screen = s_.settingsReturnScreen;
        nav.column = s_.previousColumn;   // SETTINGS owns no column — keep the one it came in with
        go_to_screen(s_, nav);            // not a bare assignment: cursors are saved/restored here
        return;
    }

    // MIDI leaves the same way, same position — and B is its ONLY way out (it is not on the grid).
    if (s_.currentScreen == ScreenType::MIDI) {
        s_.selection.exit();
        NavResult nav;
        nav.screen = s_.midiReturnScreen;
        nav.column = s_.previousColumn;
        go_to_screen(s_, nav);
        return;
    }

    // …and the mapping list, one level further in, back to MIDI (stored, like the two above).
    if (s_.currentScreen == ScreenType::MIDI_MAP) {
        s_.selection.exit();
        NavResult nav;
        nav.screen = s_.midiMapReturnScreen;
        nav.column = s_.previousColumn;
        go_to_screen(s_, nav);
        return;
    }

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
        return;
    }

    // B inside a selection COPIES it and exits. Outside one, B on the main-row screens does nothing.
    if (!s_.selection.active) return;

    const SelectionBounds b = s_.selection.bounds();
    const Project&        p = *s_.project;

    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
            clip_.copy_phrase_steps(p, s_.currentPhrase, b.topLeftRow, b.topLeftColumn,
                                    b.bottomRightRow, b.bottomRightColumn);
            break;
        case ScreenType::CHAIN:
            clip_.copy_chain_rows(p, s_.currentChain, b.topLeftRow, b.topLeftColumn,
                                  b.bottomRightRow, b.bottomRightColumn);
            break;
        case ScreenType::SONG:
            clip_.copy_song_cells(p, b.topLeftRow, b.topLeftColumn, b.bottomRightRow,
                                  b.bottomRightColumn);
            break;
        case ScreenType::TABLE:
            clip_.copy_table_rows(p, s_.currentTable, b.topLeftRow, b.topLeftColumn,
                                  b.bottomRightRow, b.bottomRightColumn);
            break;
        default:
            break;
    }
    s_.selection.exit();
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

void InputDispatcher::on_stop_preview() {
    if (route(Gesture::STOP_PREVIEW)) return;
    // ⚠️ Runs under the confirm and the FX picker too (their layers pass it on): a dialog over an
    // INSTRUMENT audition must not leave the note hanging, and the screen behind the picker started it.

    // Only screens that can START an audition stop one: PHRASE when its preview setting is on; the
    // instrument screens always (their START rings out until stopped). The browser answers for itself.
    const bool previewScreen = (s_.currentScreen == ScreenType::TABLE) ||
                               on_instrument_screen() ||
                               (s_.currentScreen == ScreenType::PHRASE && s_.settings.notePreviewEnabled);
    if (previewScreen) host_.stop_preview();
}

void InputDispatcher::on_start() {
    if (route(Gesture::START)) return;
    // ⚠️ Runs under the THEME and EQ editors too (their layers pass it on): the transport underneath is
    // how you hear an edit while dialling it.

    // ⚠️ START IS NOT ALWAYS THE TRANSPORT. On INSTRUMENT, INST.POOL, MODS and TABLE it AUDITIONS the
    // instrument at its root on the preview lane, ringing until the next plain press — over a running
    // song too (a ninth voice; it steals nothing).
    // ⚠️ TABLE auditions THROUGH the table on screen (instrument N owns table N), or you would hear the
    // instrument's own table instead of the one you are checking.
    // The lane borrows the fader of the song cell you came through, so a pad heard mostly through its
    // sends auditions where it really sits.
    if (on_instrument_screen()) {
        host_.set_preview_track(audition_track());
        host_.preview_instrument(s_.currentInstrument);
        return;
    }
    if (s_.currentScreen == ScreenType::TABLE) {
        host_.set_preview_track(audition_track());
        host_.preview_instrument(s_.currentTable, /*tableIdOverride=*/s_.currentTable);
        return;
    }

    // ⚠️ In LIVE mode START on SONG QUEUES, it does not toggle the transport: it launches the cursor's
    // cell. Stopping is R+START (one channel) or STOP (everything).
    if (live_song_gesture()) {
        const int track = s_.cursorColumn - 1;   // on SONG the column IS the track, 1-based
        if (!host_.is_playing()) {
            // Nothing running: this channel starts NOW, with the transport; the other seven begin silent.
            host_.play_song_live(s_.cursorRow, 1 << track);
            return;
        }
        // ⭐ The two-press launch: the first press queues for the end of the playing chain, a second on the
        // SAME cell promotes it to the next phrase boundary — the slot says which press this is.
        // ⚠️ Only while still ARMED: promoting a committed launch would cut short a chain still being heard.
        const songcore::LiveSlot q = host_.live_queue(track);
        const bool               immediate = q.armed() && !q.stop && q.targetRow == s_.cursorRow;
        host_.queue_live(track, s_.cursorRow, immediate);
        return;
    }

    // Everywhere else START is the transport.
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

// ─── LIVE mode's two chords ──────────────────────────────────────────────────────────────────────
//
// Reserved chords taking on a meaning: off SONG or outside LIVE they still do nothing. The gate matters
// because the mapper's two arms are GLOBAL.

int InputDispatcher::live_row_mask(int songRow) const {
    int mask = 0;
    for (int t = 0; t < 8; ++t) {
        // chainRefs is a growing list: a row past a track's end is empty.
        const std::vector<int>& refs = s_.project->tracks[static_cast<size_t>(t)].chainRefs;
        const int chainId = (songRow >= 0 && songRow < static_cast<int>(refs.size()))
                                ? refs[static_cast<size_t>(songRow)] : -1;
        if (chainId >= 0 && chainId < 256) mask |= 1 << t;
    }
    return mask;
}

bool InputDispatcher::live_row_armed(int songRow) const {
    for (int t = 0; t < 8; ++t) {
        const songcore::LiveSlot q = host_.live_queue(t);
        if (!q.stop && q.targetRow == songRow) return true;
    }
    return false;
}

void InputDispatcher::on_l_start() {
    if (route(Gesture::L_START)) return;
    if (!live_song_gesture()) return;
    if (!host_.is_playing()) {
        // From a standing start the row launches together on one downbeat; an empty cell starts silent.
        host_.play_song_live(s_.cursorRow, live_row_mask(s_.cursorRow));
        return;
    }
    host_.queue_live_row(s_.cursorRow, live_row_armed(s_.cursorRow));
}

void InputDispatcher::on_r_start() {
    if (route(Gesture::R_START)) return;
    if (!live_song_gesture()) return;
    if (!host_.is_playing()) return;   // nothing sounding, nothing to queue a stop for
    const int track = s_.cursorColumn - 1;
    // The launch's two-press promotion: chain end, then next phrase boundary.
    const songcore::LiveSlot sq = host_.live_queue(track);
    host_.queue_live_stop(track, sq.armed() && sq.stop);
}

}  // namespace pt::ui
