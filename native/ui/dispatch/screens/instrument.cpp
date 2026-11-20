// The three screens that edit one instrument — INSTRUMENT, the POOL and MODS: their buttons, and the
// TYPE cell's switch.

#include "ui/dispatch/dispatch_common.h"

#include "ui/cursor_move.h"
#include "ui/instrument_row_layout.h"

#include <algorithm>
#include <string>

namespace pt::ui {

namespace {

/** Show another instrument; its layout may have no cell where the cursor was. */
void show_instrument(AppState& s, int id) {
    s.currentInstrument    = id;
    s.lastEditedInstrument = id;
    snap_instrument_cursor(s);
}

/** B+LEFT/RIGHT: the previous or next instrument, wrapping 00↔7F. */
void cycle_instrument(AppState& s, int delta) {
    show_instrument(s, step_wrapping(s.currentInstrument, delta, songcore::POOL_INSTRUMENTS));
}

/** B+UP/DOWN: 16 on or back, wrapping like a single step (7F up is 0F); UP raises the number. */
void page_instrument(AppState& s, int direction) {
    show_instrument(s, step_wrapping(s.currentInstrument, direction * 16, songcore::POOL_INSTRUMENTS));
}

}  // namespace

// ─── The buttons ─────────────────────────────────────────────────────────────────────────────────

GestureResult InputDispatcher::instrument_screen(Gesture g) {
    switch (g) {
        // NAME and EQ, then the four buttons. All of them open on A's release.
        case Gesture::A:
            return (open_sub_screen_at_cursor(/*peek=*/false) || instrument_open_at_cursor(/*peek=*/false))
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
        case Gesture::B_UP:    page_instrument(s_, +1);  return GestureResult::TAKEN;
        case Gesture::B_DOWN:  page_instrument(s_, -1);  return GestureResult::TAKEN;

        default:
            return instrument_audition(g);
    }
}

GestureResult InputDispatcher::pool_screen(Gesture g) {
    switch (g) {
        // The EQ column opens on the release; the NAME column of an EMPTY slot loads a source into it.
        case Gesture::A:
            return (open_sub_screen_at_cursor(/*peek=*/false) || pool_load_into_empty_slot(/*peek=*/false))
                       ? GestureResult::TAKEN : GestureResult::PASS;

        // NAME column: A+B clears the slot through the host (freeing its sample or .sf2); TYPE survives.
        case Gesture::A_B:
            if (s_.poolCursorColumn != 0) return GestureResult::PASS;
            host_.clear_instrument(s_.currentInstrument);
            mark_modified();
            return GestureResult::TAKEN;

        // Pages by 16, clamping where a step wraps; UP is toward 00 (a list). No B+LEFT/RIGHT: the
        // D-pad already selects.
        case Gesture::B_UP:
        case Gesture::B_DOWN:
            s_.currentInstrument    = page_clamping(s_.currentInstrument, (g == Gesture::B_UP) ? -1 : +1,
                                                    static_cast<int>(s_.project->instruments.size()));
            s_.lastEditedInstrument = s_.currentInstrument;
            return GestureResult::TAKEN;

        default:
            return instrument_audition(g);
    }
}

GestureResult InputDispatcher::mods_screen(Gesture g) {
    switch (g) {
        // MODS is a view of one instrument, so B+D-PAD steps it as on INSTRUMENT.
        case Gesture::B_LEFT:  cycle_instrument(s_, -1); return GestureResult::TAKEN;
        case Gesture::B_RIGHT: cycle_instrument(s_, +1); return GestureResult::TAKEN;
        case Gesture::B_UP:    page_instrument(s_, +1);  return GestureResult::TAKEN;
        case Gesture::B_DOWN:  page_instrument(s_, -1);  return GestureResult::TAKEN;
        default:               return instrument_audition(g);
    }
}

// A+START auditions at the root on the preview lane until the next plain press, over a running song too.
// The lane borrows the fader of the song cell you came through.
GestureResult InputDispatcher::instrument_audition(Gesture g) {
    switch (g) {
        case Gesture::A_START:
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

bool InputDispatcher::instrument_open_at_cursor(bool peek) {
    const Instrument& ins  = host_.project().instruments[static_cast<size_t>(s_.currentInstrument)];
    const bool        isSF = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;

    const InstrumentCell cell =
        instrument_cell_at(ins.instrumentType, s_.instrumentCursor.row, s_.instrumentCursor.column);
    // A file with one preset has nothing to choose from.
    const bool patchList = cell == InstrumentCell::PATCH && isSF && s_.sfPresetCount > 1;
    const bool modelList = cell == InstrumentCell::MACRO_MODEL;
    if (peek) {
        return cell == InstrumentCell::SOURCE_LOAD || cell == InstrumentCell::SOURCE_EDIT ||
               cell == InstrumentCell::PRESET_SAVE || cell == InstrumentCell::PRESET_LOAD || patchList ||
               modelList;
    }
    if (patchList) {
        open_sf_preset_picker();
        return true;
    }
    if (modelList) {
        open_macro_model_picker();
        return true;
    }
    switch (cell) {
        case InstrumentCell::SOURCE_LOAD:
            open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                              isSF ? browser_dir(BrowserDir::SOUNDFONTS) : browser_dir(BrowserDir::SAMPLES),
                              isSF ? soundfont_extensions() : sample_extensions());
            return true;
        case InstrumentCell::SOURCE_EDIT:
            open_sample_editor();
            return true;

        // The INSTRUMENT PRESET (.pti: params, mods, table, source path).
        case InstrumentCell::PRESET_SAVE: {
            const std::string dir  = fs_.instruments_directory();
            const std::string name = ins.name.empty() ? songcore::default_instrument_name(ins.id) : ins.name;
            open_qwerty(QwertyContext::INSTRUMENT_SAVE, name, "SAVE PRESET:", dir, /*max_length=*/20,
                        /*clear_on_first_b=*/true);
            return true;
        }
        case InstrumentCell::PRESET_LOAD:
            open_file_browser(AppState::BrowserPurpose::LOAD_PRESET, browser_dir(BrowserDir::INSTRUMENTS),
                              {"pti"});
            return true;

        // NAME and EQ are not here: they live in `open_sub_screen_at_cursor`.
        default:
            return false;
    }
}

bool InputDispatcher::pool_load_into_empty_slot(bool peek) {
    // A loaded slot is managed from INSTRUMENT, where you can see what you would be replacing.
    if (s_.poolCursorColumn != 0) return false;
    const Instrument& ins  = host_.project().instruments[static_cast<size_t>(s_.currentInstrument)];
    const bool        isSF = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;
    // ⚠️ EXTERNAL has no source: it would open SAMPLES, and a load would flip its type.
    if (!instrument_has_source_row(ins.instrumentType)) return false;
    if (isSF ? ins.soundfontPath.has_value() : !songcore::instrument_is_free(ins)) return false;
    if (peek) return true;

    open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                      isSF ? browser_dir(BrowserDir::SOUNDFONTS) : browser_dir(BrowserDir::SAMPLES),
                      isSF ? soundfont_extensions() : sample_extensions());
    return true;
}

// ─── The TYPE cell ───────────────────────────────────────────────────────────────────────────────

bool InputDispatcher::on_instrument_type_cell() const {
    if (s_.currentScreen != ScreenType::INSTRUMENT) return false;
    const Instrument& ins = host_.project().instruments[static_cast<size_t>(s_.currentInstrument)];
    return instrument_cell_at(ins.instrumentType, s_.instrumentCursor.row, s_.instrumentCursor.column) ==
           InstrumentCell::TYPE;
}

/** Anything but an untouched slot goes through the dialog. ⚠️ Do not add a path around it. */
void InputDispatcher::request_instrument_type_toggle(int delta) {
    if (instrument_is_untouched()) {
        toggle_instrument_type(delta, /*keepSettings=*/false);   // nothing to lose
        return;
    }
    s_.confirm.open(ConfirmDialogState::Kind::CHANGE_TYPE);
    s_.confirm.step = delta;
}

/** No source, and nothing set but what an empty slot of this type has. The kept place does not count. */
bool InputDispatcher::instrument_is_untouched() const {
    const Instrument& ins = host_.project().instruments[static_cast<size_t>(s_.currentInstrument)];
    if (ins.sampleFilePath.has_value() || ins.soundfontPath.has_value()) return false;

    const auto settings = [&ins](Instrument x) {
        songcore::keep_instrument_place(x, ins);
        songcore::InstrumentPreset p;
        p.instrument = std::move(x);
        return songcore::serialize_instrument_preset(p);
    };
    Instrument blank;
    blank.instrumentType = ins.instrumentType;
    return settings(ins) == settings(blank);
}

/** Step TYPE, wrapping through this build's types: one without MIDI passes over EXTERNAL. */
void InputDispatcher::toggle_instrument_type(int delta, bool keepSettings) {
    Project&    p   = host_.edit_project();
    Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];

    const songcore::InstrumentType next = songcore::next_instrument_type(ins.instrumentType, delta, s_.caps.midi);

    const NameFollow name = name_follows(ins);
    host_.set_instrument_type(s_.currentInstrument, next, keepSettings);
    name.settle(ins);

    // Row 0's TYPE cell exists in every layout: nothing to clamp.
    set_status(s_, std::string("TYPE: ") + songcore::instrument_type_name(next), true);
}

// ─── The cursor and the edit ─────────────────────────────────────────────────────────────────────

CursorContext InputDispatcher::instrument_context() const {
    const Project& p = *s_.project;
    InstrumentEditorState is{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
    is.cursorRow     = s_.instrumentCursor.row;
    is.cursorColumn  = s_.instrumentCursor.column;
    // The PRESET row's range is the SF2's own list length.
    is.sfPresetName  = s_.sfPresetName;
    is.sfPresetCount = s_.sfPresetCount;
    is.sfPresetIndex = s_.sfPresetIndex;
    is.allowOscLoop  = s_.caps.loopWindow;
    return instrument_.cursor_context(is);
}

bool InputDispatcher::instrument_edit(const InputAction& action) {
    Project&    p   = host_.edit_project();
    Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];
    const bool  onModel = instrument_cell_at(ins.instrumentType, s_.instrumentCursor.row,
                                             s_.instrumentCursor.column) == InstrumentCell::MACRO_MODEL;
    const NameFollow name = name_follows(ins);
    const InstrumentInputResult r =
        instrument_.handle_input(ins, s_.instrumentCursor.row, s_.instrumentCursor.column, action);
    if (onModel && r.modified) name.settle(ins);

    // The PRESET row: only the engine knows the SF2's list, so the index is resolved here.
    if (r.presetIndexChanged) host_.set_sf_preset_by_index(s_.currentInstrument, r.presetIndex);
    return r.modified;
}

songcore::MapTarget InputDispatcher::instrument_knob() const {
    const Project& p = *s_.project;
    InstrumentEditorState is{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
    is.cursorRow    = s_.instrumentCursor.row;
    is.cursorColumn = s_.instrumentCursor.column;
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
