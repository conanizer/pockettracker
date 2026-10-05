// The held-button chords: A, B, R and L with the D-pad, delete, insert, clone, MUTE and SOLO.

#include "ui/dispatch/dispatch_common.h"

#include "songcore/traversal.h"
#include "ui/navigation.h"
#include "ui/song_pointer.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pt::ui {

namespace {

/** A phrase nobody has written a note into. */
bool phrase_is_blank(const Phrase& p) {
    for (const songcore::PhraseStep& s : p.steps)
        if (!songcore::step_is_empty(s)) return false;
    return true;
}

/** A chain that references no phrase. */
bool chain_is_blank(const Chain& c) {
    for (const int ref : c.phraseRefs)
        if (ref != -1) return false;
    return true;
}

/**
 * Search forward from `start`, wrapping once; −1 when the pool is full. Starting after the last edited
 * item hands you a free one near it, not slot 0 every time.
 */
template <typename Pred>
int first_from_wrapping(int start, int count, Pred pred) {
    for (int i = start; i < count; ++i)
        if (pred(i)) return i;
    for (int i = 0; i < start && i < count; ++i)
        if (pred(i)) return i;
    return -1;
}

/** Phrase IDs any chain references — "used" even when blank (a silent spacer inside a pad chain). */
std::set<int> used_phrase_ids(const Project& p) {
    std::set<int> used;
    for (const Chain& c : p.chains)
        for (const int ref : c.phraseRefs)
            if (ref != -1) used.insert(ref);
    return used;
}

/** Chain IDs any song track references — same "used even if blank" reasoning. */
std::set<int> used_chain_ids(const Project& p) {
    std::set<int> used;
    for (const songcore::Track& t : p.tracks)
        for (const int ref : t.chainRefs)
            if (ref != -1) used.insert(ref);
    return used;
}

}  // namespace

// ─── A + D-pad ───────────────────────────────────────────────────────────────────────────────────

// ⚠️ `pt::ui::on_a_b` must stay qualified: inside a member, unqualified lookup finds the member of the
// same name first. The other handlers are qualified to match.
//
// ⚠️ The HORIZONTAL axis is the small step (±1), the VERTICAL the large one (±`largeStep`) — the
// split handheld-tracker users already have in their fingers. Every screen override below keeps it.

/**
 * A+DPAD on the INSTRUMENT screen's TYPE cell. Switching type frees the slot's source, so a loaded slot
 * goes through the confirm dialog; only an empty one switches outright. ⚠️ Do not add a path around it.
 */
void InputDispatcher::request_instrument_type_toggle(int delta) {
    const Instrument& ins =
        host_.project().instruments[static_cast<size_t>(s_.currentInstrument)];

    if (ins.sampleFilePath.has_value() || ins.soundfontPath.has_value()) {
        s_.confirm.open(ConfirmDialogState::Kind::CHANGE_TYPE, delta);
        return;
    }
    toggle_instrument_type(delta);   // an empty slot has nothing to lose — switch it outright
}

/**
 * Step the TYPE cell by `delta`, wrapping through the types this build offers. ⚠️ EXTERNAL is the
 * last type, so a build that hides MIDI simply stops one short; an instrument already EXTERNAL keeps it.
 */
void InputDispatcher::toggle_instrument_type(int delta) {
    Project&    p   = host_.edit_project();
    Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];

    const int count = s_.caps.midi ? songcore::INSTRUMENT_TYPE_COUNT
                                   : songcore::INSTRUMENT_TYPE_COUNT - 1;
    const int cur   = static_cast<int>(ins.instrumentType);
    const int step  = delta < 0 ? -1 : +1;
    // An EXTERNAL instrument in a build that hides the type is outside the cycle; step from the last
    // reachable type.
    const int from  = (cur >= count) ? count - 1 : cur;
    const auto next = static_cast<songcore::InstrumentType>(((from + step) % count + count) % count);

    // The name the slot adopted from the source it is about to lose (see the adopt rule in browser.cpp).
    const std::string previousAutoName = instrument_auto_name(host_.project(), s_.currentInstrument);

    host_.set_instrument_type(s_.currentInstrument, next);

    // ⚠️ A type change drops the source, so a name ADOPTED from it must go too — otherwise the next
    // load reads it as a typed name and keeps it forever. A name the user typed survives.
    if (!previousAutoName.empty() && ins.name == previousAutoName)
        ins.name = songcore::default_instrument_name(ins.id);

    // Row 0 exists in all three layouts and the cursor is on its TYPE cell, so nothing to clamp; the
    // feed re-reads the SF preset next frame.
    s_.statusMessage = std::string("TYPE: ") + songcore::instrument_type_name(next);
    s_.statusSuccess = true;
}

/** True when the cursor is on INSTRUMENT's TYPE cell, the one A+DPAD does not merely increment. */
bool InputDispatcher::on_instrument_type_cell() const {
    return s_.currentScreen == ScreenType::INSTRUMENT && s_.instrumentCursorRow == 0 &&
           s_.instrumentCursorColumn == 1;
}

// A+DPAD is swallowed under the keyboard and the browser, so an A held over one never reaches the
// screen underneath.

// On the SAMPLE EDITOR, A+DPAD on rows 3..8 drags the selection's active edge (START on col 0, END on
// col 1), scaled by the zoom so a nudge is about one visible pixel.
static int64_t sample_fine_step(const SampleEditorState& se) {
    return std::max<int64_t>(1, static_cast<int64_t>(se.totalFrames) / (256LL << se.zoomLevel));
}
static int64_t sample_coarse_step(const SampleEditorState& se) {
    return std::max<int64_t>(1, static_cast<int64_t>(se.totalFrames) / (16LL << se.zoomLevel));
}

// ⚠️ The EQ arm comes first in all five A-combo handlers: the other arms ask about `currentScreen`,
// which is the screen UNDERNEATH the overlay.
//
// The THEME arm comes first in all four, and holds the editor's whole edit:
//   A+LEFT / A+RIGHT → THEME row: previous / next built-in palette; colour row: channel ∓0x01.
//   A+UP   / A+DOWN  → THEME row: the palette too; colour row: channel ±0x10.

void InputDispatcher::on_a_up() {
    if (layer_takes(Gesture::A_UP)) return;
    if (overlay_swallows(Overlay::THEME | Overlay::EQ | Overlay::FX_HELPER | Overlay::MAP_PICK)) return;
    if (theme_open()) {
        theme_dpad_edit(+1, +0x10);
        return;
    }
    if (eq_open()) { generic_input(pt::ui::increment_fast); return; }
    if (s_.fxHelper.isOpen) { fx_move_up(s_.fxHelper); return; }
    if (s_.mapPicker.isOpen) { map_picker_move_up(s_.mapPicker); return; }
    if (on_sample_selection_row()) { nudge_selection_edge(+sample_coarse_step(s_.sampleEditor)); return; }
    if (on_sample_slice_marker_row()) { nudge_slice_marker(+sample_coarse_step(s_.sampleEditor)); return; }
    if (on_fx_type_column()) {
        s_.fxHelper = fx_helper_opened_at(current_fx_type_code(),
                                          fx_layout_for(visible_effect_type_count()));
        return;
    }
    if (on_map_dest_cell()) {
        s_.mapPicker = map_picker_opened_at(static_cast<songcore::MapDestId>(
            s_.project->midiMappings[static_cast<size_t>(s_.midiMapCursorRow)].dest));
        return;
    }
    // The TYPE cell has no coarse step, so both axes walk it — through the confirm dialog on a loaded slot.
    if (on_instrument_type_cell()) { request_instrument_type_toggle(+1); return; }
    selection_or_single(pt::ui::increment_fast);
}

void InputDispatcher::on_a_down() {
    if (layer_takes(Gesture::A_DOWN)) return;
    if (overlay_swallows(Overlay::THEME | Overlay::EQ | Overlay::FX_HELPER | Overlay::MAP_PICK)) return;
    if (theme_open()) {
        theme_dpad_edit(-1, -0x10);
        return;
    }
    if (eq_open()) { generic_input(pt::ui::decrement_fast); return; }
    if (s_.fxHelper.isOpen) { fx_move_down(s_.fxHelper); return; }
    if (s_.mapPicker.isOpen) { map_picker_move_down(s_.mapPicker); return; }
    if (on_sample_selection_row()) { nudge_selection_edge(-sample_coarse_step(s_.sampleEditor)); return; }
    if (on_sample_slice_marker_row()) { nudge_slice_marker(-sample_coarse_step(s_.sampleEditor)); return; }
    if (on_fx_type_column()) {
        s_.fxHelper = fx_helper_opened_at(current_fx_type_code(),
                                          fx_layout_for(visible_effect_type_count()));
        return;
    }
    if (on_map_dest_cell()) {
        s_.mapPicker = map_picker_opened_at(static_cast<songcore::MapDestId>(
            s_.project->midiMappings[static_cast<size_t>(s_.midiMapCursorRow)].dest));
        return;
    }
    if (on_instrument_type_cell()) { request_instrument_type_toggle(-1); return; }
    selection_or_single(pt::ui::decrement_fast);
}

void InputDispatcher::on_a_left() {
    if (layer_takes(Gesture::A_LEFT)) return;
    if (overlay_swallows(Overlay::THEME | Overlay::EQ | Overlay::FX_HELPER | Overlay::MAP_PICK)) return;
    if (theme_open()) {
        theme_dpad_edit(-1, -0x01);
        return;
    }
    if (eq_open()) { generic_input(pt::ui::decrement); return; }
    if (s_.fxHelper.isOpen) { fx_move_left(s_.fxHelper); return; }
    if (s_.mapPicker.isOpen) { map_picker_move_left(s_.mapPicker); return; }
    if (on_sample_selection_row()) { nudge_selection_edge(-sample_fine_step(s_.sampleEditor)); return; }
    if (on_sample_slice_marker_row()) { nudge_slice_marker(-sample_fine_step(s_.sampleEditor)); return; }
    if (on_instrument_type_cell()) { request_instrument_type_toggle(-1); return; }
    selection_or_single(pt::ui::decrement);
}

void InputDispatcher::on_a_right() {
    if (layer_takes(Gesture::A_RIGHT)) return;
    if (overlay_swallows(Overlay::THEME | Overlay::EQ | Overlay::FX_HELPER | Overlay::MAP_PICK)) return;
    if (theme_open()) {
        theme_dpad_edit(+1, +0x01);
        return;
    }
    if (eq_open()) { generic_input(pt::ui::increment); return; }
    if (s_.fxHelper.isOpen) { fx_move_right(s_.fxHelper); return; }
    if (s_.mapPicker.isOpen) { map_picker_move_right(s_.mapPicker); return; }
    if (on_sample_selection_row()) { nudge_selection_edge(+sample_fine_step(s_.sampleEditor)); return; }
    if (on_sample_slice_marker_row()) { nudge_slice_marker(+sample_fine_step(s_.sampleEditor)); return; }
    if (on_instrument_type_cell()) { request_instrument_type_toggle(+1); return; }
    selection_or_single(pt::ui::increment);
}

void InputDispatcher::on_a_released() {
    // Ahead of the overlay test: nothing else can start a preview while A is down, so this silences
    // the one A began.
    if (heldNotePreview_) {
        heldNotePreview_ = false;
        host_.stop_preview(/*cut=*/true);
    }
    if (layer_takes(Gesture::A_RELEASE)) return;

    // Both pickers commit on RELEASE, so you can hold A, read the list, and let go on your choice.
    if (top_overlay() == Overlay::MAP_PICK) { apply_map_picker_choice(); return; }
    if (top_overlay() != Overlay::FX_HELPER) return;
    apply_fx_type_change(s_.fxHelper.selected_effect_code());
    s_.fxHelper = FxHelperState{};
}

void InputDispatcher::on_a_deferred() {
    // The mapper is holding this press; record the playhead now, since it will have moved by release.
    sliceTapPlayhead_ = on_slice_tap_cell() ? s_.sampleEditor.playbackPosition : -1.0f;
}

// ─── A+B: delete / reset ─────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_a_b() {
    if (layer_takes(Gesture::A_B)) return;
    if (overlay_swallows(Overlay::EQ)) return;

    // A+B in the EQ editor resets the param under the cursor: FREQ 0x80 (≈450 Hz), GAIN 120 (0 dB),
    // Q 0x80. TYPE has no default.
    if (eq_open()) { generic_input(pt::ui::on_a_b); return; }

    if (s_.selection.active) {
        const SelectionBounds b = s_.selection.bounds();
        Project&              p = host_.edit_project();
        switch (s_.currentScreen) {
            case ScreenType::PHRASE:
                clip_.delete_phrase_steps(p, s_.currentPhrase, b.topLeftRow, b.topLeftColumn,
                                          b.bottomRightRow, b.bottomRightColumn);
                break;
            case ScreenType::CHAIN:
                clip_.delete_chain_rows(p, s_.currentChain, b.topLeftRow, b.topLeftColumn,
                                        b.bottomRightRow, b.bottomRightColumn);
                break;
            case ScreenType::SONG:
                clip_.delete_song_cells(p, b.topLeftRow, b.topLeftColumn, b.bottomRightRow,
                                        b.bottomRightColumn);
                break;
            case ScreenType::TABLE:
                clip_.delete_table_rows(p, s_.currentTable, b.topLeftRow, b.topLeftColumn,
                                        b.bottomRightRow, b.bottomRightColumn);
                break;
            default:
                s_.selection.exit();
                return;
        }
        mark_modified();
        s_.selection.exit();
        return;
    }

    // The SAMPLE EDITOR's SELECTION row (8): A+B resets the edge under the cursor to the sample's own
    // bound — START to 0, END to the last frame.
    if (on_sample_editor() && s_.sampleEditor.cursorRow == 8) {
        SampleEditorState& se = s_.sampleEditor;
        if (se.cursorCol == 0)      se.selectionStart = 0;
        else if (se.cursorCol == 1) se.selectionEnd   = se.totalFrames;
        return;
    }

    // The SLICE DETAIL row (11): A+B puts the boundary back where its method would — the detected
    // position under TRANSIENT, the arithmetic cut under DIVIDE. Under MANUAL it deletes the boundary.
    if (on_sample_editor() && s_.sampleEditor.cursorRow == 11) { reset_slice_marker(); return; }

    // The pool's NAME column: A+B CLEARS the slot, freeing its sample (and the .sf2, if this was its
    // last user) — a host verb, not a field write. The TYPE survives.
    if (s_.currentScreen == ScreenType::INST_POOL && s_.poolCursorColumn == 0) {
        host_.clear_instrument(s_.currentInstrument);
        mark_modified();
        return;
    }

    generic_input(pt::ui::on_a_b);
}

// ─── A,A: insert the next UNUSED item ────────────────────────────────────────────────────────────

// The EQ editor cannot be raised on any screen these handlers act on, so the overlay guards in
// on_a_a / on_l_* are defensive — kept so an EQ cell added to a clipboard screen cannot paste into a
// phrase the user cannot see.

void InputDispatcher::on_a_a() {
    if (layer_takes(Gesture::A_A)) return;
    if (overlay_swallows(Overlay::NONE)) return;

    // The sample editor's row 11 needs no arm: its A is deferred, and the mapper clears `lastAPress`
    // on every defer, so no tap reaches this handler.

    // ⚠️ RESAMPLE goes before the double-tap gate: under a SONG selection the first A copied rather
    // than inserting, so the gate would return and this arm would never run.
    if (s_.currentScreen == ScreenType::SONG && s_.selection.active) {
        open_qwerty(QwertyContext::RESAMPLE, resample_base_name(fs_), "SAMPLE NAME:", "",
                    /*max_length=*/20, /*clear_on_first_b=*/true);
        return;
    }

    // ⚠️ Tap tempo: a second A inside 300 ms (= 200 BPM) arrives here, not at `on_button_a`. Without
    // this arm every other fast tap would be swallowed by the gate below and the tempo would halve.
    if (s_.currentScreen == ScreenType::PROJECT &&
        s_.projectCursorRow == static_cast<int>(ProjectRow::TEMPO) &&
        s_.projectCursorColumn == 2) {
        tap_tempo();
        return;
    }

    // A double-tap only counts if the cursor has not moved between the presses. ⚠️ The PHRASE
    // audition is owed on both exits — a quick second A never reaches `on_button_a`.
    if (!hasInsertPos_ || insertScreen_ != s_.currentScreen || insertRow_ != s_.cursorRow ||
        insertCol_ != s_.cursorColumn) {
        preview_held_note();
        return;
    }
    hasInsertPos_ = false;

    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::SONG) {
        if (s_.cursorColumn < 1 || s_.cursorColumn > 8) return;
        songcore::Track& track = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)];

        const int next = first_from_wrapping(s_.lastEditedChain + 1, 256, [&](int i) {
            return chain_is_blank(p.chains[static_cast<size_t>(i)]);
        });
        if (next < 0) return;

        while (static_cast<int>(track.chainRefs.size()) <= s_.cursorRow) track.chainRefs.push_back(-1);
        track.chainRefs[static_cast<size_t>(s_.cursorRow)] = next;
        s_.lastEditedChain                                 = next;
        mark_modified();

    } else if (s_.currentScreen == ScreenType::CHAIN) {
        Chain& chain = p.chains[static_cast<size_t>(s_.currentChain)];

        const int next = first_from_wrapping(s_.lastEditedPhrase + 1, 256, [&](int i) {
            return phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
        });
        if (next < 0) return;

        chain.phraseRefs[static_cast<size_t>(s_.cursorRow)]      = next;
        chain.transposeValues[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedTranspose;
        s_.lastEditedPhrase                                      = next;
        mark_modified();

    } else if (s_.currentScreen == ScreenType::PHRASE) {
        // Advance the NOTE cell's instrument to the next FREE slot. `instrument_is_free` also skips
        // configured SoundFonts, which have a null sampleFilePath too.
        if (s_.cursorColumn != 1) return;   // the NOTE column only
        Phrase&               ph   = p.phrases[static_cast<size_t>(s_.currentPhrase)];
        songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];

        const int count = static_cast<int>(p.instruments.size());
        const int next  = first_from_wrapping(s_.lastEditedInstrument + 1, count, [&](int i) {
            return songcore::instrument_is_free(p.instruments[static_cast<size_t>(i)]);
        });
        if (next >= 0) {
            step.instrument         = next;
            s_.lastEditedInstrument = next;
            mark_modified();
        }
        preview_held_note();
    }
}

// ─── B + D-pad: which item am I looking at? ──────────────────────────────────────────────────────

void InputDispatcher::cycle_current_item(int delta) {
    // ⚠️ The THEME editor swallows B+LEFT/RIGHT: A+LEFT/RIGHT already walks the palettes, and the
    // press would otherwise reach SETTINGS underneath.
    if (theme_open()) return;

    // ⚠️ In the EQ editor B+LEFT/RIGHT changes the SLOT and CLAMPS at 0 and 127 where everything else
    // wraps: wrapping would silently re-point the mixer channel at an unrelated curve.
    if (eq_open()) {
        const int newSlot = std::min(127, std::max(0, s_.eq.slotIndex + delta));
        s_.eq.slotIndex   = newSlot;
        apply_caller_eq_slot_change(newSlot);
        return;
    }

    // ⭐ On SONG, B+LEFT/RIGHT toggles the transport mode; both directions toggle,
    // since there are only two modes. ⚠️ Gated on SONG alone — the handler is shared.
    if (s_.currentScreen == ScreenType::SONG) {
        host_.set_live_mode(!host_.live_mode());
        return;
    }

    // ⭐ Under NAV = SONG this walks the SONG row instead of the pool. CHAIN steps to the nearest filled
    // cell either side; PHRASE to the nearest whose chain also holds a phrase at this chain row
    // (songcore/traversal.h). Both clamp. `chainRow` is not reset — the CHAIN→PHRASE gate already
    // refuses an empty row.
    if (s_.settings.navSongRelative &&
        (s_.currentScreen == ScreenType::CHAIN || s_.currentScreen == ScreenType::PHRASE)) {
        const int requireRow = (s_.currentScreen == ScreenType::PHRASE) ? pointer_chain_row(s_) : -1;
        const int songRow    = pointer_song_row(s_);
        const int track      = songcore::next_song_cell_h(*s_.project, songRow, pointer_track(s_),
                                                          delta, requireRow);
        set_pointer_song_cell(s_, songRow, track);
        refresh_song_relative_refs(s_);
        return;
    }

    // A flooring modulo, so −1 wraps to the top.
    auto wrap = [delta](int value, int max) {
        const int n = max + 1;
        return ((value + delta) % n + n) % n;
    };

    switch (s_.currentScreen) {
        case ScreenType::CHAIN:
            s_.currentChain    = wrap(s_.currentChain, 255);
            s_.lastEditedChain = s_.currentChain;
            break;
        case ScreenType::PHRASE:
            s_.currentPhrase    = wrap(s_.currentPhrase, 255);
            s_.lastEditedPhrase = s_.currentPhrase;
            break;
        case ScreenType::TABLE:
            s_.currentTable    = wrap(s_.currentTable, 127);
            s_.lastEditedTable = s_.currentTable;
            break;
        case ScreenType::GROOVE:
            s_.currentGroove = wrap(s_.currentGroove, 127);
            break;
        case ScreenType::SCALE:
            s_.currentScale = wrap(s_.currentScale, songcore::POOL_SCALES - 1);
            break;
        // INSTRUMENT and MODS cycle the instrument (MODS is a view of one). Not INST_POOL: there the
        // D-pad already selects it.
        case ScreenType::INSTRUMENT:
        case ScreenType::MODS:
            s_.currentInstrument    = wrap(s_.currentInstrument, 127);
            s_.lastEditedInstrument = s_.currentInstrument;
            break;
        default:
            break;
    }
}

// The THEME and EQ arms live inside cycle_current_item: one swallows B+LEFT/RIGHT, the other re-points
// the EQ slot with it.
void InputDispatcher::on_b_left() {
    if (layer_takes(Gesture::B_LEFT)) return;
    if (overlay_swallows(Overlay::THEME | Overlay::EQ)) return;
    cycle_current_item(-1);
}

void InputDispatcher::on_b_right() {
    if (layer_takes(Gesture::B_RIGHT)) return;
    if (overlay_swallows(Overlay::THEME | Overlay::EQ)) return;
    cycle_current_item(+1);
}

/**
 * B+UP/DOWN under NAV = SONG. On CHAIN it walks the track column to the nearest filled song row,
 * skipping gaps. On PHRASE it walks the chain's own filled rows and never leaves the chain.
 *
 * ⚠️ Returns true even when the walk clamps — falling through would page the song from under the pointer.
 */
bool InputDispatcher::song_relative_b_vertical(int delta) {
    if (!s_.settings.navSongRelative) return false;

    if (s_.currentScreen == ScreenType::CHAIN) {
        const int track = pointer_track(s_);
        const int row   = songcore::next_song_cell_v(*s_.project, pointer_song_row(s_), track, delta);
        set_pointer_song_cell(s_, row, track);
        refresh_song_relative_refs(s_);
        return true;
    }
    if (s_.currentScreen == ScreenType::PHRASE) {
        const int row = songcore::next_chain_row(*s_.project, s_.currentChain, pointer_chain_row(s_),
                                                 delta, /*wrap=*/false);
        set_pointer_chain_row(s_, row);
        refresh_song_relative_refs(s_);
        return true;
    }
    return false;
}

void InputDispatcher::on_b_up() {
    if (layer_takes(Gesture::B_UP)) return;
    if (overlay_swallows(Overlay::NONE)) return;
    if (song_relative_b_vertical(-1)) return;

    // B+UP/DOWN steps the GROOVE screen's quantize from any cell on it.
    if (s_.currentScreen == ScreenType::GROOVE) {
        s_.grooveQuantize = (s_.grooveQuantize + 1) % GROOVE_QUANTIZE_COUNT;
        return;
    }

    // The pool pages by 16 but CLAMPS at the ends, where a single D-pad step wraps 00↔7F.
    if (s_.currentScreen == ScreenType::INST_POOL) {
        s_.currentInstrument    = std::max(0, s_.currentInstrument - 16);
        s_.lastEditedInstrument = s_.currentInstrument;
        return;
    }
    if (s_.currentScreen != ScreenType::SONG) return;
    s_.cursorRow = std::max(0, s_.cursorRow - 16);
    scroll_song_to_row(s_, s_.cursorRow);
}

void InputDispatcher::on_b_down() {
    if (layer_takes(Gesture::B_DOWN)) return;
    if (overlay_swallows(Overlay::NONE)) return;
    if (song_relative_b_vertical(+1)) return;

    if (s_.currentScreen == ScreenType::GROOVE) {
        s_.grooveQuantize =
            (s_.grooveQuantize + GROOVE_QUANTIZE_COUNT - 1) % GROOVE_QUANTIZE_COUNT;
        return;
    }

    if (s_.currentScreen == ScreenType::INST_POOL) {
        const int last = static_cast<int>(s_.project->instruments.size()) - 1;
        s_.currentInstrument    = std::min(last, s_.currentInstrument + 16);
        s_.lastEditedInstrument = s_.currentInstrument;
        return;
    }
    if (s_.currentScreen != ScreenType::SONG) return;
    s_.cursorRow = std::min(255, s_.cursorRow + 16);
    scroll_song_to_row(s_, s_.cursorRow);
}

// ─── R + D-pad: move between screens — except on the modals ──────────────────────────────────────
//
// ⚠️ On the modals R+DPAD is not navigation. KEYBOARD: R+UP/DOWN switches layout, R+LEFT/RIGHT moves
// the text cursor. BROWSER: R+UP/DOWN cycles the sort, R+LEFT goes up a directory. SAMPLE EDITOR:
// R+UP/DOWN zooms, R+LEFT/RIGHT swallowed. EQ EDITOR: all four swallowed. None may fall through to
// `navigate_*` — a popup is not a cell in the screen grid, and the user would land on a screen with the
// popup's state still live.

void InputDispatcher::on_r_up() {
    if (layer_takes(Gesture::R_UP)) return;
    if (overlay_swallows(Overlay::QWERTY | Overlay::BROWSER)) return;
    if (qwerty_open()) { s_.qwerty.layout = 0; clamp_col(s_.qwerty); return; }
    if (on_browser()) { browser_cycle_sort(+1); return; }
    // Sample-editor ZOOM: R+UP/DOWN step `zoomLevel` (0=1×…4=16×); the feed re-bins the waveform.
    if (on_sample_editor()) {
        if (s_.sampleEditor.showConfirmClose) return;   // the ARE YOU SURE? dialog owns the buttons
        s_.sampleEditor.zoomLevel = std::min(s_.sampleEditor.zoomLevel + 1, 4);
        return;
    }
    const NavState ns = nav_state_of(s_);
    go_to_screen(s_, navigate_up(ns));
    s_.selection.exit();   // a selection belongs to the screen it was made on
}

void InputDispatcher::on_r_down() {
    if (layer_takes(Gesture::R_DOWN)) return;
    if (overlay_swallows(Overlay::QWERTY | Overlay::BROWSER)) return;
    if (qwerty_open()) { s_.qwerty.layout = 1; clamp_col(s_.qwerty); return; }
    if (on_browser()) { browser_cycle_sort(-1); return; }
    if (on_sample_editor()) {   // ZOOM OUT — see on_r_up
        if (s_.sampleEditor.showConfirmClose) return;
        s_.sampleEditor.zoomLevel = std::max(s_.sampleEditor.zoomLevel - 1, 0);
        return;
    }
    const NavState ns = nav_state_of(s_);
    go_to_screen(s_, navigate_down(ns));
    s_.selection.exit();
}

void InputDispatcher::browser_cycle_sort(int delta) {
    FileBrowserState& b = s_.fileBrowser;

    // Steps the modes by index, so the enum's order is behaviour (ui/filesystem.h).
    const int next = (static_cast<int>(b.sortMode) + delta + FILE_SORT_MODE_COUNT) % FILE_SORT_MODE_COUNT;
    b.sortMode = static_cast<FileSortMode>(next);

    // ⚠️ Rebuild rather than re-sort in place, or the tie-break depends on the previous sort mode.
    rebuild_items(b, fs_);

    // The cursor stays put, so the row under it now holds a different file — the list is re-ordered.
    b.statusMessage = file_sort_label(b.sortMode);
    b.statusSuccess = true;
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
    if (layer_takes(Gesture::R_LEFT)) return;
    if (overlay_swallows(Overlay::QWERTY | Overlay::BROWSER)) return;
    if (qwerty_open()) {
        move_text_cursor_left(s_.qwerty);
        return;
    }
    if (on_sample_editor()) return;   // see on_r_right
    if (on_browser())  { navigate_to_parent(s_.fileBrowser, fs_); return; }
    const NavState ns = nav_state_of(s_);
    const NavResult r = navigate_left(ns);
    if (r.screen != s_.currentScreen) sync_last_edited_on_screen_switch(s_.currentScreen, r.screen);
    go_to_screen(s_, r);
    s_.selection.exit();
}

void InputDispatcher::on_r_right() {
    if (layer_takes(Gesture::R_RIGHT)) return;
    if (overlay_swallows(Overlay::QWERTY | Overlay::BROWSER)) return;
    if (qwerty_open()) {
        move_text_cursor_right(s_.qwerty);
        return;
    }
    // ⚠️ Swallowed on the sample editor: it has no cell in the screen grid, so navigating would fall
    // through to PHRASE and bypass ARE YOU SURE?, silently discarding an unsaved edit.
    if (on_sample_editor()) return;
    if (on_browser())  return;   // no "down a directory" — that is what A on a folder is for
    const NavState ns = nav_state_of(s_);
    const NavResult r = navigate_right(ns);
    // ⚠️ The NAV = SONG entry gate sits above the sync: a refused press must leave nothing behind, and
    // the sync writes the lastEdited memory. R+RIGHT is the only gated direction (ui/song_pointer.h).
    if (!song_relative_entry_allowed(s_, r.screen)) return;
    if (r.screen != s_.currentScreen) sync_last_edited_on_screen_switch(s_.currentScreen, r.screen);
    go_to_screen(s_, r);
    s_.selection.exit();
}

// ─── L: selection and the clipboard ──────────────────────────────────────────────────────────────

void InputDispatcher::on_l_b() {
    if (layer_takes(Gesture::L_B)) return;
    if (overlay_swallows(Overlay::BROWSER)) return;

    // ⚠️ The browser's selection is a plain anchor..cursor range over a list (a second tap inside the
    // window selects all) — a different machine from the grid editors' CELL→ROW→SCREEN widener.
    if (on_browser()) {
        FileBrowserState& b = s_.fileBrowser;
        if (b.mode != BrowserMode::NORMAL) return;

        if (!b.selectionMode) {
            b.selectionMode   = true;
            b.selectionAnchor = b.cursor;
            b.lastSelectTapMs = now_ms_;
        } else if (now_ms_ - b.lastSelectTapMs <= 500) {
            // Tap again inside the window: select everything, skipping the ".." row.
            const int first = b.first_selectable();
            const int last  = std::max(static_cast<int>(b.items.size()) - 1, first);
            b.selectionAnchor = first;
            b.cursor          = last;
            b.scroll          = std::max(0, last - BROWSER_VISIBLE_ROWS + 1);
            b.lastSelectTapMs = 0;   // …so a third tap re-anchors rather than re-selecting all
        } else {
            b.selectionAnchor = b.cursor;   // the window lapsed — start a fresh range here
            b.lastSelectTapMs = now_ms_;
        }
        return;
    }

    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
        case ScreenType::CHAIN:
        case ScreenType::SONG:
        case ScreenType::TABLE:
            s_.selection.handle_select_b(now_ms_, cursor_row(), cursor_column(),
                                         max_selection_column(), max_selection_row());
            break;
        default:
            break;  // GROOVE has one column and no clipboard type — nothing to select
    }
}

void InputDispatcher::on_l_a() {
    if (layer_takes(Gesture::L_A)) return;
    // ⚠️ Every layer an arm below tests for must be in this set, or the gesture is thrown away here.
    if (overlay_swallows(Overlay::THEME | Overlay::BROWSER)) return;

    // ⚠️ Must return: `currentScreen` is still SETTINGS underneath, and falling through would edit
    // a screen the user cannot see.
    if (theme_open()) {
        const int color = theme_color_index(s_.themeEditor.cursorRow);
        if (color >= 0) s_.themeEditor.locks.toggle(color);
        return;
    }

    // On the browser L+A cuts/pastes FILES — the same shape as the grid editors below.
    if (on_browser()) {
        FileBrowserState& b = s_.fileBrowser;
        if (b.mode != BrowserMode::NORMAL) return;

        if (b.selectionMode) {
            std::vector<std::string> files = browser_selected_paths();
            if (files.empty()) return;
            const size_t n = files.size();

            b.fileClipboard      = std::move(files);
            b.fileClipboardIsCut = true;
            b.selectionMode      = false;
            b.selectionAnchor    = -1;
            b.statusMessage = "CUT " + std::to_string(n) + (n == 1 ? " FILE" : " FILES");
            b.statusSuccess = true;
        } else if (!b.fileClipboard.empty()) {
            browser_paste();
        }
        return;
    }

    // Inside a selection L+A CUTS; outside one it PASTES.
    Project& p = host_.edit_project();

    if (s_.selection.active) {
        const SelectionBounds b = s_.selection.bounds();
        switch (s_.currentScreen) {
            case ScreenType::PHRASE:
                clip_.cut_phrase_steps(p, s_.currentPhrase, b.topLeftRow, b.topLeftColumn,
                                       b.bottomRightRow, b.bottomRightColumn);
                break;
            case ScreenType::CHAIN:
                clip_.cut_chain_rows(p, s_.currentChain, b.topLeftRow, b.topLeftColumn,
                                     b.bottomRightRow, b.bottomRightColumn);
                break;
            case ScreenType::SONG:
                clip_.cut_song_cells(p, b.topLeftRow, b.topLeftColumn, b.bottomRightRow,
                                     b.bottomRightColumn);
                break;
            case ScreenType::TABLE:
                clip_.cut_table_rows(p, s_.currentTable, b.topLeftRow, b.topLeftColumn,
                                     b.bottomRightRow, b.bottomRightColumn);
                break;
            default:
                s_.selection.exit();
                return;
        }
        mark_modified();
        s_.selection.exit();
        return;
    }

    // Paste. The target id is the item being edited; SONG has none (its clip carries its own tracks).
    int targetId = 0;
    switch (s_.currentScreen) {
        case ScreenType::PHRASE: targetId = s_.currentPhrase; break;
        case ScreenType::CHAIN:  targetId = s_.currentChain;  break;
        case ScreenType::TABLE:  targetId = s_.currentTable;  break;
        default:                 targetId = 0;                break;
    }
    const PasteResult r =
        clip_.paste(p, s_.currentScreen, targetId, cursor_row(), cursor_column());
    if (r.kind == PasteResult::Kind::SUCCESS && r.itemsPasted > 0) mark_modified();
}

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
    if (layer_takes(Gesture::R_B)) return;
    if (!mute_solo_chord_live()) return;
    toggle_mute_solo(/*solo=*/false);
}

void InputDispatcher::on_r_a() {
    if (layer_takes(Gesture::R_A)) return;
    // ⚠️ Before the mute/solo guard: the editor stands on SETTINGS, which that guard would refuse.
    if (theme_open()) {
        if (theme_color_index(s_.themeEditor.cursorRow) >= 0) theme_roll_palette(/*rowOnly=*/true);
        return;
    }
    if (!mute_solo_chord_live()) return;
    toggle_mute_solo(/*solo=*/true);
}

// Is R+A/R+B a MUTE/SOLO here? Asked by the chord and by the deferred B alike.
bool InputDispatcher::mute_solo_chord_live() const {
    if (overlay_swallows(Overlay::NONE)) return false;   // a modal owns the buttons while it is up
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
    if (layer_takes(Gesture::L_R)) return;
    if (overlay_swallows(Overlay::BROWSER)) return;
    if (on_browser()) {
        s_.fileBrowser.selectionMode   = false;
        s_.fileBrowser.selectionAnchor = -1;
        return;
    }

    // One press undoes one thing, most recent first (`s_.lastClearable`): the mix (any channel muted
    // or soloed) or the selection with its buffer. ⚠️ A rung with nothing to clear falls through to
    // the other, or L+R reads as a dead button. The mix check covers all MIX_CHANNELS, so the REV and
    // DEL returns count.
    const bool mix_touched = [&] {
        if (s_.currentScreen == ScreenType::SAMPLE_EDITOR) return false;
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

    // ⚠️ A deny-list: the readout is drawn on every screen, so the clear must work everywhere except
    // SAMPLE_EDITOR, where L+R is the editor's own selection.
    const bool had_buffer = !clip_.info().empty();
    if (s_.currentScreen != ScreenType::SAMPLE_EDITOR) clip_.clear();

    // Nothing on the selection rung to clear, but the mix has something: take it rather than leave
    // the press doing nothing at all.
    if (!had_buffer && mix_touched) restore_full_playback();
}

// ─── L+B+A: clone ────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_l_b_a() {
    if (layer_takes(Gesture::L_B_A)) return;
    if (overlay_swallows(Overlay::NONE)) return;

    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::SONG) {
        if (s_.cursorColumn < 1 || s_.cursorColumn > 8) { s_.selection.exit(); return; }
        songcore::Track& track = p.tracks[static_cast<size_t>(s_.cursorColumn - 1)];
        const int currentChainId =
            (s_.cursorRow < static_cast<int>(track.chainRefs.size()))
                ? track.chainRefs[static_cast<size_t>(s_.cursorRow)]
                : -1;

        if (currentChainId != -1) {
            const Chain&        src         = p.chains[static_cast<size_t>(currentChainId)];
            const std::set<int> usedChains  = used_chain_ids(p);
            const std::set<int> usedPhrases = used_phrase_ids(p);

            // The destination must be a FREE chain: blank AND unreferenced.
            const int dstChainId = first_from_wrapping(currentChainId + 1, 256, [&](int i) {
                return usedChains.count(i) == 0 && chain_is_blank(p.chains[static_cast<size_t>(i)]);
            });

            // A DEEP clone: every phrase the chain references gets its own free slot, so the copy is
            // fully independent. `reserved` stops two source phrases claiming the same destination;
            // duplicate refs inside the chain map to the SAME clone, which is what keeps a chain that
            // plays phrase 5 twice still playing one phrase twice.
            std::vector<int>   srcPhraseIds;
            for (const int ref : src.phraseRefs)
                if (ref != -1 &&
                    std::find(srcPhraseIds.begin(), srcPhraseIds.end(), ref) == srcPhraseIds.end())
                    srcPhraseIds.push_back(ref);

            std::set<int>      reserved;
            std::map<int, int> phraseMap;
            bool               enoughPhrases = true;
            for (const int pid : srcPhraseIds) {
                const int slot = first_from_wrapping(0, 256, [&](int i) {
                    return reserved.count(i) == 0 && usedPhrases.count(i) == 0 &&
                           phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
                });
                if (slot < 0) { enoughPhrases = false; break; }
                reserved.insert(slot);
                phraseMap[pid] = slot;
            }

            // Capacity is checked in FULL before anything is written. Abort, never half-clone: a
            // partial clone leaves a chain pointing at phrases that were never copied.
            if (dstChainId < 0) {
                s_.statusMessage = "NO FREE CHAINS";
                s_.statusSuccess = false;
            } else if (!enoughPhrases) {
                s_.statusMessage = "NO FREE PHRASES";
                s_.statusSuccess = false;
            } else {
                for (const auto& kv : phraseMap)
                    p.phrases[static_cast<size_t>(kv.second)].steps =
                        p.phrases[static_cast<size_t>(kv.first)].steps;

                Chain& dst = p.chains[static_cast<size_t>(dstChainId)];
                for (size_t i = 0; i < src.phraseRefs.size(); ++i) {
                    const int ref     = src.phraseRefs[i];
                    dst.phraseRefs[i] = (ref == -1) ? -1 : phraseMap[ref];
                }
                dst.transposeValues = src.transposeValues;

                track.chainRefs[static_cast<size_t>(s_.cursorRow)] = dstChainId;
                s_.lastEditedChain = dstChainId;
                s_.statusMessage   = "CHAIN CLONED";
                s_.statusSuccess   = true;
                mark_modified();
            }
        }

    } else if (s_.currentScreen == ScreenType::CHAIN) {
        Chain&    chain           = p.chains[static_cast<size_t>(s_.currentChain)];
        const int currentPhraseId = chain.phraseRefs[static_cast<size_t>(s_.cursorRow)];
        if (currentPhraseId != -1) {
            const std::set<int> usedPhrases = used_phrase_ids(p);
            const int next = first_from_wrapping(currentPhraseId + 1, 256, [&](int i) {
                return usedPhrases.count(i) == 0 && phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
            });
            if (next >= 0) {
                p.phrases[static_cast<size_t>(next)].steps =
                    p.phrases[static_cast<size_t>(currentPhraseId)].steps;
                chain.phraseRefs[static_cast<size_t>(s_.cursorRow)] = next;
                s_.lastEditedPhrase                                 = next;
                mark_modified();
            }
        }

    } else if (s_.currentScreen == ScreenType::PHRASE) {
        const int srcPhraseId = s_.currentPhrase;
        const std::set<int> usedPhrases = used_phrase_ids(p);
        const int next = first_from_wrapping(srcPhraseId + 1, 256, [&](int i) {
            return i != srcPhraseId && usedPhrases.count(i) == 0 &&
                   phrase_is_blank(p.phrases[static_cast<size_t>(i)]);
        });
        if (next >= 0) {
            p.phrases[static_cast<size_t>(next)].steps =
                p.phrases[static_cast<size_t>(srcPhraseId)].steps;
            s_.currentPhrase = next;   // …and follow the clone, so you are editing the copy
            mark_modified();
        }
    }

    s_.selection.exit();
}

}  // namespace pt::ui
