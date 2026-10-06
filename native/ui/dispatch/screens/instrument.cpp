// The three screens that edit one instrument — INSTRUMENT, the POOL and MODS: their buttons, and the
// TYPE cell's switch.

#include "ui/dispatch/dispatch_common.h"

#include "ui/instrument_row_layout.h"

#include <algorithm>
#include <string>

namespace pt::ui {

namespace {

/** B+LEFT/RIGHT on INSTRUMENT and MODS: the previous or next instrument, wrapping 00↔7F. */
void cycle_instrument(AppState& s, int delta) {
    s.currentInstrument    = ((s.currentInstrument + delta) % 128 + 128) % 128;
    s.lastEditedInstrument = s.currentInstrument;
}

}  // namespace

// ─── The buttons ─────────────────────────────────────────────────────────────────────────────────

GestureResult InputDispatcher::instrument_screen(Gesture g) {
    switch (g) {
        // A cell that opens on the release (NAME, EQ) first, then the buttons that fire on the press.
        case Gesture::A:
            return (open_sub_screen_at_cursor(/*peek=*/false) || instrument_open_at_cursor())
                       ? GestureResult::TAKEN : GestureResult::PASS;

        // The TYPE cell has no coarse step, so both axes walk it — through the confirm dialog on a
        // loaded slot.
        case Gesture::A_UP:
        case Gesture::A_RIGHT:
            if (!on_instrument_type_cell()) return GestureResult::PASS;
            request_instrument_type_toggle(+1);
            return GestureResult::TAKEN;
        case Gesture::A_DOWN:
        case Gesture::A_LEFT:
            if (!on_instrument_type_cell()) return GestureResult::PASS;
            request_instrument_type_toggle(-1);
            return GestureResult::TAKEN;

        case Gesture::B_LEFT:  cycle_instrument(s_, -1); return GestureResult::TAKEN;
        case Gesture::B_RIGHT: cycle_instrument(s_, +1); return GestureResult::TAKEN;

        default:
            return instrument_audition(g);
    }
}

GestureResult InputDispatcher::pool_screen(Gesture g) {
    switch (g) {
        // The EQ column opens on the release; the NAME column of an EMPTY slot loads a source into it.
        case Gesture::A:
            return (open_sub_screen_at_cursor(/*peek=*/false) || pool_load_into_empty_slot())
                       ? GestureResult::TAKEN : GestureResult::PASS;

        // The NAME column: A+B CLEARS the slot, freeing its sample (and the .sf2, if this was its last
        // user) — a host verb, not a field write. The TYPE survives.
        case Gesture::A_B:
            if (s_.poolCursorColumn != 0) return GestureResult::PASS;
            host_.clear_instrument(s_.currentInstrument);
            mark_modified();
            return GestureResult::TAKEN;

        // Pages by 16 but CLAMPS at the ends, where a single D-pad step wraps 00↔7F. No B+LEFT/RIGHT:
        // here the D-pad already selects the instrument.
        case Gesture::B_UP:
            s_.currentInstrument    = std::max(0, s_.currentInstrument - 16);
            s_.lastEditedInstrument = s_.currentInstrument;
            return GestureResult::TAKEN;
        case Gesture::B_DOWN:
            s_.currentInstrument    = std::min(static_cast<int>(s_.project->instruments.size()) - 1,
                                               s_.currentInstrument + 16);
            s_.lastEditedInstrument = s_.currentInstrument;
            return GestureResult::TAKEN;

        default:
            return instrument_audition(g);
    }
}

GestureResult InputDispatcher::mods_screen(Gesture g) {
    switch (g) {
        // MODS is a view of one instrument, so B+LEFT/RIGHT steps it as on INSTRUMENT.
        case Gesture::B_LEFT:  cycle_instrument(s_, -1); return GestureResult::TAKEN;
        case Gesture::B_RIGHT: cycle_instrument(s_, +1); return GestureResult::TAKEN;
        default:               return instrument_audition(g);
    }
}

// ⚠️ START is not the transport here: it auditions the instrument at its root on the preview lane,
// ringing until the next plain press — over a running song too (a ninth voice; it steals nothing).
// The lane borrows the fader of the song cell you came through, so a pad heard mostly through its
// sends auditions where it really sits.
GestureResult InputDispatcher::instrument_audition(Gesture g) {
    switch (g) {
        case Gesture::START:
            host_.set_preview_track(audition_track());
            host_.preview_instrument(s_.currentInstrument);
            return GestureResult::TAKEN;
        case Gesture::STOP_PREVIEW:
            host_.stop_preview();
            return GestureResult::TAKEN;
        default:
            return GestureResult::PASS;
    }
}

// ─── What A opens ────────────────────────────────────────────────────────────────────────────────

bool InputDispatcher::instrument_open_at_cursor() {
    const Instrument& ins  = host_.project().instruments[static_cast<size_t>(s_.currentInstrument)];
    const bool        isSF = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;
    const int         row  = s_.instrumentCursorRow;
    const int         col  = s_.instrumentCursorColumn;

    // Row 0 — TYPE (col 1), LOAD (col 2) browses for a source, EDIT (col 3) opens the sample editor.
    // EXTERNAL draws neither button; refuse rather than browse for a source it cannot have.
    if (row == 0 && col >= 2 && !instrument_has_source_row(ins.instrumentType)) return true;

    if (row == 0 && col == 2) {
        open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                          isSF ? browser_dir(BrowserDir::SOUNDFONTS) : browser_dir(BrowserDir::SAMPLES),
                          isSF ? soundfont_extensions() : sample_extensions());
        return true;
    }
    // Samplers only: a SoundFont has no single waveform to cut. EDIT is not drawn on SF, so the isSF
    // guard only consumes the press.
    if (row == 0 && col == 3) {
        if (isSF) return true;   // handled: the press is CONSUMED, it just opens nothing
        open_sample_editor();
        return true;
    }

    // Row 5 — the INSTRUMENT PRESET (.pti: params, mods, table, source path). SAVE (col 2), LOAD (col 3).
    if (row == 5 && col == 2) {
        const std::string dir  = fs_.instruments_directory();
        const std::string name = ins.name.empty() ? songcore::default_instrument_name(ins.id) : ins.name;
        open_qwerty(QwertyContext::INSTRUMENT_SAVE, name, "SAVE PRESET:", dir, /*max_length=*/20,
                    /*clear_on_first_b=*/true);
        return true;
    }
    if (row == 5 && col == 3) {
        open_file_browser(AppState::BrowserPurpose::LOAD_PRESET, browser_dir(BrowserDir::INSTRUMENTS),
                          {"pti"});
        return true;
    }

    // Row 1 (NAME) and the EQ cell are not here: their A must wait for the RELEASE, so they live in
    // `open_sub_screen_at_cursor`. These are read-only buttons that fire on the press.
    return false;
}

bool InputDispatcher::pool_load_into_empty_slot() {
    // A loaded slot is managed from INSTRUMENT, where you can see what you would be replacing.
    if (s_.poolCursorColumn != 0) return false;
    const Instrument& ins  = host_.project().instruments[static_cast<size_t>(s_.currentInstrument)];
    const bool        isSF = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;
    // ⚠️ An EXTERNAL slot has no source: without this it would open the SAMPLES browser, and a load
    // would flip the slot's type back.
    if (!instrument_has_source_row(ins.instrumentType)) return false;
    if (isSF ? ins.soundfontPath.has_value() : !songcore::instrument_is_free(ins)) return false;

    open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                      isSF ? browser_dir(BrowserDir::SOUNDFONTS) : browser_dir(BrowserDir::SAMPLES),
                      isSF ? soundfont_extensions() : sample_extensions());
    return true;
}

// ─── The TYPE cell ───────────────────────────────────────────────────────────────────────────────

bool InputDispatcher::on_instrument_type_cell() const {
    return s_.currentScreen == ScreenType::INSTRUMENT && s_.instrumentCursorRow == 0 &&
           s_.instrumentCursorColumn == 1;
}

/**
 * Switching type frees the slot's source, so a loaded slot goes through the confirm dialog; only an
 * empty one switches outright. ⚠️ Do not add a path around it.
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

    // The name the slot adopted from the source it is about to lose (see the adopt rule in
    // screens/file_browser.cpp).
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

// ─── The cursor and the edit ─────────────────────────────────────────────────────────────────────

CursorContext InputDispatcher::instrument_context() const {
    const Project& p = *s_.project;
    InstrumentEditorState is{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
    is.cursorRow     = s_.instrumentCursorRow;
    is.cursorColumn  = s_.instrumentCursorColumn;
    // The PRESET row's range is the SF2's own list length.
    is.sfPresetName  = s_.sfPresetName;
    is.sfPresetCount = s_.sfPresetCount;
    is.sfPresetIndex = s_.sfPresetIndex;
    is.allowOscLoop  = s_.caps.loopWindow;
    return instrument_.cursor_context(is);
}

bool InputDispatcher::instrument_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    const InstrumentInputResult r = instrument_.handle_input(
        p.instruments[static_cast<size_t>(s_.currentInstrument)], s_.instrumentCursorRow,
        s_.instrumentCursorColumn, action);

    // The PRESET row: the bank+preset behind an index live in the SF2's list, which only the
    // engine has opened — resolved here so the module stays a pure function of the Project.
    if (r.presetIndexChanged) host_.set_sf_preset_by_index(s_.currentInstrument, r.presetIndex);
    return r.modified;
}

songcore::MapTarget InputDispatcher::instrument_knob() const {
    const Project& p = *s_.project;
    InstrumentEditorState is{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
    is.cursorRow    = s_.instrumentCursorRow;
    is.cursorColumn = s_.instrumentCursorColumn;
    // The module holds the instrument by reference and never knew its number — finished here.
    songcore::MapTarget t = instrument_.map_target(is);
    t.scope = static_cast<uint8_t>(s_.currentInstrument);
    return t;
}

CursorContext InputDispatcher::pool_context() const {
    const Project& p = *s_.project;
    InstrumentPoolState ps{p};
    ps.selectedInstrument = s_.currentInstrument;
    ps.cursorColumn       = s_.poolCursorColumn;
    return pool_.cursor_context(ps);
}

bool InputDispatcher::pool_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    return pool_.handle_input(p.instruments[static_cast<size_t>(s_.currentInstrument)],
                              s_.poolCursorColumn, action);
}

CursorContext InputDispatcher::mods_context() const {
    const Project& p = *s_.project;
    ModulationState ms{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
    ms.cursorRow  = s_.modCursorRow;
    ms.cursorPair = s_.modCursorPair;
    ms.cursorSide = s_.modCursorSide;
    return mods_.cursor_context(ms);
}

bool InputDispatcher::mods_edit(const InputAction& action) {
    Project& p = host_.edit_project();
    ModulationState ms{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
    ms.cursorPair = s_.modCursorPair;
    ms.cursorSide = s_.modCursorSide;
    return mods_
        .handle_input(p.instruments[static_cast<size_t>(s_.currentInstrument)],
                      ms.active_slot_index(), s_.modCursorRow, action)
        .modified;
}

}  // namespace pt::ui
