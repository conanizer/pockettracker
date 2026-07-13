#include "ui/input_dispatcher.h"

#include "songcore/timing.h"
#include "songcore/traversal.h"
#include "ui/cursor_move.h"
#include "ui/navigation.h"
#include "ui/std_filesystem.h"   // path_name / path_stem / path_extension / to_lower

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pt::ui {

using songcore::Chain;
using songcore::Instrument;
using songcore::Note;
using songcore::Phrase;
using songcore::Project;

namespace {

/** Chain.isEmpty(row) — the row holds no phrase. */
bool chain_row_empty(const Chain& c, int row) { return c.phraseRefs[static_cast<size_t>(row)] == -1; }

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
 * Kotlin's `((start..255) + (0 until start)).firstOrNull { pred }` — search forward from `start`,
 * wrapping once. Returns −1 when the pool is full.
 *
 * The wrap is the point: inserting the next unused phrase should hand you one NEAR the one you were
 * just editing, not slot 0 every time. Starting at `lastEdited + 1` and wrapping is what does that.
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

// ─── The cursor ──────────────────────────────────────────────────────────────────────────────────

int InputDispatcher::cursor_row() const {
    switch (s_.currentScreen) {
        case ScreenType::TABLE:  return s_.tableCursorRow;
        case ScreenType::GROOVE: return s_.grooveCursorRow;
        default:                 return s_.cursorRow;
    }
}

int InputDispatcher::cursor_column() const {
    switch (s_.currentScreen) {
        case ScreenType::TABLE:  return s_.tableCursorColumn;
        case ScreenType::GROOVE: return 1;
        default:                 return s_.cursorColumn;
    }
}

void InputDispatcher::set_cursor_row(int row) {
    switch (s_.currentScreen) {
        case ScreenType::TABLE:  s_.tableCursorRow = row;  break;
        case ScreenType::GROOVE: s_.grooveCursorRow = row; break;
        default:                 s_.cursorRow = row;       break;
    }
}

int InputDispatcher::max_selection_column() const {
    switch (s_.currentScreen) {
        case ScreenType::PHRASE: return 9;
        case ScreenType::CHAIN:  return 2;
        case ScreenType::SONG:   return 8;
        case ScreenType::TABLE:  return 8;
        default:                 return 1;
    }
}

int InputDispatcher::max_selection_row() const {
    // SONG is 256 rows deep and shows 16. A SCREEN-scope selection there means the whole ARRANGEMENT,
    // not the visible window — which is the only reason `maxRow` is a parameter at all.
    return (s_.currentScreen == ScreenType::SONG) ? 255 : 15;
}

bool InputDispatcher::on_instrument_screen() const {
    return s_.currentScreen == ScreenType::INSTRUMENT ||
           s_.currentScreen == ScreenType::INST_POOL ||
           s_.currentScreen == ScreenType::MODS;
}

bool InputDispatcher::on_globals_screen() const {
    return s_.currentScreen == ScreenType::MIXER || s_.currentScreen == ScreenType::EFFECTS;
}

CursorContext InputDispatcher::cursor_context() const {
    const Project& p = *s_.project;
    switch (s_.currentScreen) {
        case ScreenType::SONG: {
            SongEditorState ss{p};
            ss.cursorRow   = s_.cursorRow;
            ss.cursorTrack = s_.cursorColumn;  // on SONG the column IS the track
            return song_.cursor_context(ss);
        }
        case ScreenType::CHAIN: {
            ChainEditorState cs{p.chains[static_cast<size_t>(s_.currentChain)]};
            cs.cursorRow    = s_.cursorRow;
            cs.cursorColumn = s_.cursorColumn;
            return chain_.cursor_context(cs);
        }
        case ScreenType::PHRASE: {
            PhraseEditorState ps{p.phrases[static_cast<size_t>(s_.currentPhrase)]};
            ps.cursorRow    = s_.cursorRow;
            ps.cursorColumn = s_.cursorColumn;
            return phrase_.cursor_context(ps);
        }
        case ScreenType::TABLE: {
            TableState ts{p.tables[static_cast<size_t>(s_.currentTable)]};
            ts.cursorRow    = s_.tableCursorRow;
            ts.cursorColumn = s_.tableCursorColumn;
            return table_.cursor_context(ts);
        }
        case ScreenType::GROOVE: {
            GrooveState gs{p.grooves[static_cast<size_t>(s_.currentGroove)]};
            gs.cursorRow    = s_.grooveCursorRow;
            gs.cursorColumn = 1;
            return groove_.cursor_context(gs);
        }

        case ScreenType::INSTRUMENT: {
            InstrumentEditorState is{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
            is.cursorRow     = s_.instrumentCursorRow;
            is.cursorColumn  = s_.instrumentCursorColumn;
            // The PRESET row's range is the SF2's own list length, so the context needs it.
            is.sfPresetName  = s_.sfPresetName;
            is.sfPresetCount = s_.sfPresetCount;
            is.sfPresetIndex = s_.sfPresetIndex;
            return instrument_.cursor_context(is);
        }

        case ScreenType::INST_POOL: {
            InstrumentPoolState ps{p};
            ps.selectedInstrument = s_.currentInstrument;
            ps.cursorColumn       = s_.poolCursorColumn;
            return pool_.cursor_context(ps);
        }

        case ScreenType::MODS: {
            ModulationState ms{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
            ms.cursorRow  = s_.modCursorRow;
            ms.cursorPair = s_.modCursorPair;
            ms.cursorSide = s_.modCursorSide;
            return mods_.cursor_context(ms);
        }

        case ScreenType::MIXER: {
            MixerState ms{p};
            ms.cursorColumn   = s_.mixerCursorColumn;
            ms.mixerMasterRow = s_.mixerMasterRow;
            return mixer_.cursor_context(ms);
        }

        case ScreenType::EFFECTS: {
            EffectState es{p};
            es.cursorRow = s_.effectsCursorRow;
            return effects_.cursor_context(es);
        }

        default:
            return cc::none();  // a placeholder screen has nothing to edit
    }
}

// ─── Applying an edit ────────────────────────────────────────────────────────────────────────────

bool InputDispatcher::apply_edit(const InputAction& action) {
    Project& p = host_.edit_project();  // the SAME Project the Sequencer is reading

    switch (s_.currentScreen) {
        case ScreenType::SONG: {
            const SongInputResult r = song_.handle_input(p, s_.cursorRow, s_.cursorColumn, action);
            if (r.hasChain) s_.lastEditedChain = r.lastEditedChain;
            return r.modified;
        }

        case ScreenType::CHAIN: {
            const ChainInputResult r = chain_.handle_input(
                p.chains[static_cast<size_t>(s_.currentChain)], s_.cursorRow, s_.cursorColumn, action);
            if (r.hasPhrase)    s_.lastEditedPhrase    = r.lastEditedPhrase;
            if (r.hasTranspose) s_.lastEditedTranspose = r.lastEditedTranspose;
            return r.modified;
        }

        case ScreenType::PHRASE: {
            Phrase& ph = p.phrases[static_cast<size_t>(s_.currentPhrase)];
            const PhraseInputResult r = phrase_.handle_input(ph, s_.cursorRow, s_.cursorColumn, action);
            if (!r.modified) return false;

            // The "last edited" memory + the audition, exactly where Kotlin does them. Note the two
            // guards: the STEP must have a note (editing the velocity of an empty step remembers
            // nothing), and only an edit to the NOTE column auditions — dialling a velocity should
            // not retrigger the voice under your fingers.
            if (r.hasNote || r.hasVolume || r.hasInstrument) {
                const songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];
                if (step.note != Note::EMPTY()) {
                    s_.lastEditedNote       = step.note;
                    s_.lastEditedVolume     = step.volume;
                    s_.lastEditedInstrument = step.instrument;
                    if (s_.notePreviewEnabled && r.hasNote) preview_edited_note();
                }
            }
            return true;
        }

        case ScreenType::TABLE:
            return table_
                .handle_input(p.tables[static_cast<size_t>(s_.currentTable)], s_.tableCursorRow,
                              s_.tableCursorColumn, action)
                .modified;

        case ScreenType::GROOVE:
            return groove_
                .handle_input(p.grooves[static_cast<size_t>(s_.currentGroove)], s_.grooveCursorRow,
                              /*cursor_column=*/1, action)
                .modified;

        case ScreenType::INSTRUMENT: {
            const InstrumentInputResult r = instrument_.handle_input(
                p.instruments[static_cast<size_t>(s_.currentInstrument)], s_.instrumentCursorRow,
                s_.instrumentCursorColumn, action);

            // The PRESET row. The module deliberately does not resolve it — the bank+preset behind an
            // index live in the SF2's own list, which only the ENGINE has opened. Keeping that one
            // lookup here is what keeps the module a pure function of the Project, and therefore
            // measurable by tools/ptinput.
            if (r.presetIndexChanged) host_.set_sf_preset_by_index(s_.currentInstrument, r.presetIndex);
            return r.modified;
        }

        case ScreenType::INST_POOL:
            return pool_.handle_input(p.instruments[static_cast<size_t>(s_.currentInstrument)],
                                      s_.poolCursorColumn, action);

        case ScreenType::MODS: {
            ModulationState ms{p.instruments[static_cast<size_t>(s_.currentInstrument)]};
            ms.cursorPair = s_.modCursorPair;
            ms.cursorSide = s_.modCursorSide;
            return mods_
                .handle_input(p.instruments[static_cast<size_t>(s_.currentInstrument)],
                              ms.active_slot_index(), s_.modCursorRow, action)
                .modified;
        }

        // MIXER and EFFECTS take the whole PROJECT, not a cell: their fields are scattered across it
        // (a track's volume, the master strip, two send buses), and which one an action lands on is the
        // cursor's business. Kotlin's modules likewise take the Project.
        case ScreenType::MIXER:
            return mixer_.handle_input(p, s_.mixerMasterRow, s_.mixerCursorColumn, action).modified;

        case ScreenType::EFFECTS:
            return effects_.handle_input(p, s_.effectsCursorRow, action).modified;

        default:
            return false;
    }
}

void InputDispatcher::mark_modified(bool table_touched) {
    // ⚠️ The consumer caches which tables it has already pushed to the engine. push_project
    // invalidates that cache; an IN-PLACE edit cannot, so the table screen must say so itself.
    if (table_touched || s_.currentScreen == ScreenType::TABLE) host_.invalidate_tables();

    // ⚠️ An instrument's params are ENGINE STATE, not something a note carries: the engine holds one
    // slot per instrument for its drive, crush, downsample, filter, sample window and loop, and reads
    // them as a voice runs. Turn the filter while a pad rings and you must hear it turn. Nothing on the
    // event path pushes them (a note re-pushes the mods and sends, but not these), so the edit says so
    // here — the same call Kotlin's InstrumentController.updateDrive makes.
    if (on_instrument_screen()) host_.push_instrument(s_.currentInstrument);

    // The same argument one level up: the MIXER and EFFECTS screens edit state the engine holds ON ITS
    // OWN BEHALF — the mixer, the master bus, both send buses, the master EQ. None of it is a note, so
    // nothing on the event path pushes it, and an unpushed reverb setting is simply not heard.
    //
    // ⚠️ ONE DELIBERATE DIVERGENCE FROM ANDROID, and it is a bug fix. Kotlin pushes these per-field, and
    // three of its arms are guarded — `if (slot >= 0) setMasterEqSlot(slot)`, and the same for the two
    // input EQs. So DELETING an EQ slot (A+B) writes −1 into the project and then declines to tell the
    // engine, which goes on filtering with the slot it last had: the screen says "off", the audio says
    // otherwise, until the project is reloaded. −1 is the engine's own documented bypass value
    // (audio-engine.h) and Kotlin's *load* path pushes it happily (pushGlobalEffectsToBackend), so the
    // guard is simply wrong. Pushing the globals wholesale, as below, cannot express the bug. The Kotlin
    // side is fixed to match (AppInputDispatcher).
    if (on_globals_screen()) host_.push_globals();

    // An edit made WHILE PLAYING has to reach the lookahead already scheduled past it, or it is not
    // heard until the buffer happens to roll over it. Android does this from a
    // `LaunchedEffect(projectVersion)`; there is no Compose here, so it is said out loud.
    if (host_.is_playing()) host_.notify_data_changed();
}

void InputDispatcher::preview_edited_note() {
    const Project& p = *s_.project;
    const songcore::PhraseStep& step =
        p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];

    const int sr = std::max(44100, host_.sample_rate());
    host_.preview_note(std::min(std::max(step.instrument, 0), 127), step.note,
                       songcore::frames_per_step(p.tempo, sr));
}

// ─── The three generic paths ─────────────────────────────────────────────────────────────────────

void InputDispatcher::generic_input(InputAction (*fn)(const CursorContext&)) {
    const InputAction action = fn(cursor_context());
    if (action.type == ActionType::NONE) return;
    if (apply_edit(action)) mark_modified();
}

void InputDispatcher::selection_or_single(InputAction (*fn)(const CursorContext&)) {
    if (!s_.selection.active) {
        generic_input(fn);
        return;
    }
    // Every row of the selection, one at a time, through the SAME path — the cursor is walked down
    // the range and put back. Not a special-cased bulk edit: whatever a single cell does, N cells do
    // N times, so a new column can never behave differently under a selection than outside one.
    const SelectionBounds b       = s_.selection.bounds();
    const int             savedRow = cursor_row();
    bool                  any      = false;

    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
        case ScreenType::CHAIN:
        case ScreenType::SONG:
        case ScreenType::TABLE:
            for (int row = b.topLeftRow; row <= b.bottomRightRow; ++row) {
                set_cursor_row(row);
                const InputAction action = fn(cursor_context());
                if (action.type != ActionType::NONE && apply_edit(action)) any = true;
            }
            set_cursor_row(savedRow);
            if (any) mark_modified();
            break;

        default:
            generic_input(fn);
            break;
    }
}

void InputDispatcher::dpad_nav(const char* direction) {
    if (s_.selection.active) {
        const CursorPosition edgeBefore = s_.selection.end;
        s_.selection.expand(direction, max_selection_row(), max_selection_column());

        // Drag the CURSOR along with the selection's active edge, so it stays on screen — without
        // this, a SONG selection whose edge runs past row 16 scrolls out from under the anchored
        // cursor and you are editing blind. Only when the edge actually MOVED, so hitting a clamp (or
        // a D-pad in SCREEN scope, where the bounds are fixed) cannot teleport the cursor.
        const CursorPosition edge = s_.selection.end;
        if (edge != edgeBefore) {
            const ScreenType sc = s_.currentScreen;
            if (sc == ScreenType::PHRASE || sc == ScreenType::CHAIN || sc == ScreenType::SONG) {
                s_.cursorRow    = edge.row;
                s_.cursorColumn = edge.column;
                if (sc == ScreenType::SONG) scroll_song_to_row(s_, edge.row);
            } else if (sc == ScreenType::TABLE) {
                s_.tableCursorRow    = edge.row;
                s_.tableCursorColumn = edge.column;
            }
        }
        return;
    }

    const std::string d(direction);
    if (d == "UP")         move_cursor_up(s_);
    else if (d == "DOWN")  move_cursor_down(s_);
    else if (d == "LEFT")  move_cursor_left(s_);
    else if (d == "RIGHT") move_cursor_right(s_);
}

// ─── D-pad alone ─────────────────────────────────────────────────────────────────────────────────
//
// ⚠️ THE MODAL RULE (input_dispatcher.h): keyboard first, then browser, then the screen. The order is
// the specification — the keyboard opens ON TOP of the browser (SELECT+A renames a file), and a D-pad
// press there must move the KEY cursor, not the file cursor.

void InputDispatcher::on_dpad_up() {
    if (qwerty_open()) { move_key_cursor_up(s_.qwerty); return; }
    if (on_browser())  { browser_move_cursor(-1, /*page=*/false); return; }
    dpad_nav("UP");
}

void InputDispatcher::on_dpad_down() {
    if (qwerty_open()) { move_key_cursor_down(s_.qwerty); return; }
    if (on_browser())  { browser_move_cursor(+1, /*page=*/false); return; }
    dpad_nav("DOWN");
}

void InputDispatcher::on_dpad_left() {
    if (qwerty_open()) { move_key_cursor_left(s_.qwerty); return; }
    // LEFT/RIGHT PAGE the browser by a screenful — the one list in the app long enough to need it.
    if (on_browser())  { browser_move_cursor(-BROWSER_VISIBLE_ROWS, /*page=*/true); return; }
    dpad_nav("LEFT");
}

void InputDispatcher::on_dpad_right() {
    if (qwerty_open()) { move_key_cursor_right(s_.qwerty); return; }
    if (on_browser())  { browser_move_cursor(+BROWSER_VISIBLE_ROWS, /*page=*/true); return; }
    dpad_nav("RIGHT");
}

// ─── The FX-type column, and the helper it opens ──────────────────────────────────────────────────

bool InputDispatcher::on_fx_type_column() const {
    switch (s_.currentScreen) {
        case ScreenType::PHRASE:
            return s_.cursorColumn == 4 || s_.cursorColumn == 6 || s_.cursorColumn == 8;
        case ScreenType::TABLE:
            return s_.tableCursorColumn == 3 || s_.tableCursorColumn == 5 ||
                   s_.tableCursorColumn == 7;
        default:
            return false;
    }
}

int InputDispatcher::current_fx_type_index() const {
    const Project& p = *s_.project;
    int            code = 0;

    if (s_.currentScreen == ScreenType::PHRASE) {
        const songcore::PhraseStep& step =
            p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        switch (s_.cursorColumn) {
            case 4: code = step.fx1Type; break;
            case 6: code = step.fx2Type; break;
            case 8: code = step.fx3Type; break;
            default: break;
        }
    } else if (s_.currentScreen == ScreenType::TABLE) {
        const songcore::TableRow& row =
            p.tables[static_cast<size_t>(s_.currentTable)].rows[static_cast<size_t>(s_.tableCursorRow)];
        switch (s_.tableCursorColumn) {
            case 3: code = row.fx1Type; break;
            case 5: code = row.fx2Type; break;
            case 7: code = row.fx3Type; break;
            default: break;
        }
    }
    return songcore::effect_type_index(code);
}

void InputDispatcher::apply_fx_type_change(int effect_code) {
    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::PHRASE) {
        songcore::PhraseStep& step =
            p.phrases[static_cast<size_t>(s_.currentPhrase)].steps[static_cast<size_t>(s_.cursorRow)];
        switch (s_.cursorColumn) {
            case 4: step.fx1Type = effect_code; break;
            case 6: step.fx2Type = effect_code; break;
            case 8: step.fx3Type = effect_code; break;
            default: return;
        }
        mark_modified();
    } else if (s_.currentScreen == ScreenType::TABLE) {
        songcore::TableRow& row =
            p.tables[static_cast<size_t>(s_.currentTable)].rows[static_cast<size_t>(s_.tableCursorRow)];
        switch (s_.tableCursorColumn) {
            case 3: row.fx1Type = effect_code; break;
            case 5: row.fx2Type = effect_code; break;
            case 7: row.fx3Type = effect_code; break;
            default: return;
        }
        mark_modified(/*table_touched=*/true);
    }
}

// ─── A + D-pad ───────────────────────────────────────────────────────────────────────────────────

// ⚠️ The three methods below share a NAME with the free cursor.h handlers they call
// (`on_a_left` / `on_a_right` / `on_a_b`), and inside a member function unqualified lookup finds the
// MEMBER first — `selection_or_single(on_a_left)` would try to pass the method to itself. The
// `pt::ui::` qualification forces namespace-scope lookup and is load-bearing, not decoration.
//
// The names are worth the qualification: the free ones are S1's, they are goldened by `ptinput`, and
// they mirror Kotlin's `InputController.handleALeft`; the methods mirror `ButtonHandlers.onALeft`.
// Both sets are named after what they are, and both names are right.

/**
 * The INSTRUMENT screen's TYPE row. See the header for why this refuses rather than asking.
 *
 * The refusal is not timidity — SAMPLER↔SOUNDFONT is the one edit on this screen that DESTROYS
 * something (the old type's source is freed; a sampler has no use for an .sf2 and vice versa). Kotlin
 * puts a confirm dialog in front of it for exactly that reason. Silently dropping a loaded sample
 * because the user nudged A+UP one row too far is the worst of the three options; refusing with a
 * message is the honest one until the modal system lands.
 */
void InputDispatcher::toggle_instrument_type() {
    Project&    p   = host_.edit_project();
    Instrument& ins = p.instruments[static_cast<size_t>(s_.currentInstrument)];

    if (ins.sampleFilePath.has_value() || ins.soundfontPath.has_value()) {
        s_.statusMessage = "CLEAR SLOT FIRST";
        s_.statusSuccess = false;
        return;
    }

    const songcore::InstrumentType next =
        (ins.instrumentType == songcore::InstrumentType::SOUNDFONT)
            ? songcore::InstrumentType::SAMPLER
            : songcore::InstrumentType::SOUNDFONT;
    host_.set_instrument_type(s_.currentInstrument, next);

    // The row map just changed under the cursor (a SoundFont gains PRESET and loses the loop rows), and
    // the cursor is sitting on row 0 — which exists in both. Nothing to clamp, but the SF preset
    // readback must be re-taken, and the feed does that from the path+type on the next frame.
    s_.statusMessage = (next == songcore::InstrumentType::SOUNDFONT) ? "TYPE: SOUNDFONT" : "TYPE: SAMPLER";
    s_.statusSuccess = true;
}

/** True when the cursor is on INSTRUMENT's TYPE cell, the one A+UP/DOWN does not merely increment. */
bool InputDispatcher::on_instrument_type_cell() const {
    return s_.currentScreen == ScreenType::INSTRUMENT && s_.instrumentCursorRow == 0 &&
           s_.instrumentCursorColumn == 1;
}

// A+DPAD has no meaning in either modal — the keyboard's D-pad moves its key cursor (a bare press,
// handled above) and the browser's moves its file cursor. Swallowed, not passed through: an A held
// down over a browser must not reach the editor screen underneath it.

void InputDispatcher::on_a_up() {
    if (qwerty_open() || on_browser()) return;
    if (s_.fxHelper.isOpen) { fx_move_up(s_.fxHelper); return; }
    if (on_fx_type_column()) { s_.fxHelper = fx_helper_opened_at(current_fx_type_index()); return; }
    if (on_instrument_type_cell()) { toggle_instrument_type(); return; }
    selection_or_single(pt::ui::on_a);
}

void InputDispatcher::on_a_down() {
    if (qwerty_open() || on_browser()) return;
    if (s_.fxHelper.isOpen) { fx_move_down(s_.fxHelper); return; }
    if (on_fx_type_column()) { s_.fxHelper = fx_helper_opened_at(current_fx_type_index()); return; }
    if (on_instrument_type_cell()) { toggle_instrument_type(); return; }
    selection_or_single(pt::ui::on_b);   // A+DOWN DECREMENTS — `on_b` is the generic "step down"
}

void InputDispatcher::on_a_left() {
    if (qwerty_open() || on_browser()) return;
    if (s_.fxHelper.isOpen) { fx_move_left(s_.fxHelper); return; }
    selection_or_single(pt::ui::on_a_left);
}

void InputDispatcher::on_a_right() {
    if (qwerty_open() || on_browser()) return;
    if (s_.fxHelper.isOpen) { fx_move_right(s_.fxHelper); return; }
    selection_or_single(pt::ui::on_a_right);
}

void InputDispatcher::on_a_released() {
    // The FX helper commits on RELEASE, not on a press — which is what lets you hold A, read the
    // description of half a dozen effects, and let go on the one you want.
    if (!s_.fxHelper.isOpen) return;
    apply_fx_type_change(s_.fxHelper.selected_effect_code());
    s_.fxHelper = FxHelperState{};
}

// ─── A+B: delete / reset ─────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_a_b() {
    if (qwerty_open() || on_browser()) return;

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

    // The pool's NAME column: A+B CLEARS the slot (M8's EDIT+OPTION). It frees the sample's PCM and,
    // if this was the SoundFont's last user, that .sf2's engine slot too — which is the whole reason it
    // is a host verb and not a field assignment. The instrument TYPE survives, so a SoundFont slot
    // stays a (now empty) SoundFont slot rather than silently becoming a sampler under the cursor.
    if (s_.currentScreen == ScreenType::INST_POOL && s_.poolCursorColumn == 0) {
        host_.clear_instrument(s_.currentInstrument);
        mark_modified();
        return;
    }

    generic_input(pt::ui::on_a_b);
}

// ─── A,A: insert the next UNUSED item ────────────────────────────────────────────────────────────

void InputDispatcher::on_a_a() {
    if (qwerty_open() || on_browser()) return;

    // A double-tap is only a double-tap if the cursor has not moved between the presses. Anything
    // else is two separate A presses, and each of those already did something (they inserted the
    // LAST-EDITED item — see on_button_a).
    if (!hasInsertPos_ || insertScreen_ != s_.currentScreen || insertRow_ != s_.cursorRow ||
        insertCol_ != s_.cursorColumn) {
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
    }
}

// ─── B + D-pad: which item am I looking at? ──────────────────────────────────────────────────────

void InputDispatcher::cycle_current_item(int delta) {
    // Kotlin's `(value + delta).mod(max + 1)` — a FLOORING modulo, so −1 wraps to the top rather than
    // staying at −1 the way C's % would.
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
        // INSTRUMENT and MODS cycle the same thing — the instrument — because MODS *is* a view of one.
        // (INST_POOL is absent on purpose: there, the D-PAD already selects the instrument, so B+LEFT
        // would be a second, redundant way to do it. Kotlin has the same gap for the same reason.)
        case ScreenType::INSTRUMENT:
        case ScreenType::MODS:
            s_.currentInstrument    = wrap(s_.currentInstrument, 127);
            s_.lastEditedInstrument = s_.currentInstrument;
            break;
        default:
            break;
    }
}

void InputDispatcher::on_b_left()  { if (qwerty_open() || on_browser()) return; cycle_current_item(-1); }
void InputDispatcher::on_b_right() { if (qwerty_open() || on_browser()) return; cycle_current_item(+1); }

void InputDispatcher::on_b_up() {
    if (qwerty_open() || on_browser()) return;

    // The pool pages by 16 like the song does — but it CLAMPS at the ends where a single D-pad step
    // wraps 00↔7F. Paging past the end of a 128-slot list should stop at the end, not lap it.
    if (s_.currentScreen == ScreenType::INST_POOL) {
        s_.currentInstrument    = std::max(0, s_.currentInstrument - 16);
        s_.lastEditedInstrument = s_.currentInstrument;
        return;
    }
    if (s_.currentScreen != ScreenType::SONG) return;
    s_.cursorRow = std::max(0, s_.cursorRow - 16);   // TrackerController.moveSongBigUp
    scroll_song_to_row(s_, s_.cursorRow);
}

void InputDispatcher::on_b_down() {
    if (qwerty_open() || on_browser()) return;

    if (s_.currentScreen == ScreenType::INST_POOL) {
        const int last = static_cast<int>(s_.project->instruments.size()) - 1;
        s_.currentInstrument    = std::min(last, s_.currentInstrument + 16);
        s_.lastEditedInstrument = s_.currentInstrument;
        return;
    }
    if (s_.currentScreen != ScreenType::SONG) return;
    s_.cursorRow = std::min(255, s_.cursorRow + 16);  // moveSongBigDown
    scroll_song_to_row(s_, s_.cursorRow);
}

// ─── R + D-pad: move between screens — except where it does not ──────────────────────────────────
//
// ⚠️ On the two modals R+DPAD is NOT navigation, and this is the one place the modal rule pays for
// itself twice over. In the KEYBOARD, R+UP/DOWN switches layout (letters ↔ numbers) and R+LEFT/RIGHT
// moves the TEXT cursor — four bindings that have nowhere else to live on an eight-button device. In
// the BROWSER, R+UP/DOWN cycles the SORT MODE and R+LEFT goes UP A DIRECTORY, which is what its own
// bottom bar advertises ("R+<=UP R+^v=SORT").
//
// Neither may fall through to `navigate_*`: a browser is a popup, not a cell in the 5×5 screen grid,
// and R+RIGHT out of one would land the user on a screen with the browser's cursor state still live.

void InputDispatcher::on_r_up() {
    if (qwerty_open()) { s_.qwerty.layout = 0; clamp_col(s_.qwerty); return; }
    if (on_browser()) { browser_cycle_sort(+1); return; }
    const NavState ns = nav_state_of(s_);
    go_to_screen(s_, navigate_up(ns));
    s_.selection.exit();   // a selection belongs to the screen it was made on
}

void InputDispatcher::on_r_down() {
    if (qwerty_open()) { s_.qwerty.layout = 1; clamp_col(s_.qwerty); return; }
    if (on_browser()) { browser_cycle_sort(-1); return; }
    const NavState ns = nav_state_of(s_);
    go_to_screen(s_, navigate_down(ns));
    s_.selection.exit();
}

void InputDispatcher::browser_cycle_sort(int delta) {
    FileBrowserState& b = s_.fileBrowser;

    // Step through the six modes BY INDEX, which is why the enum's declaration order is behaviour
    // rather than documentation (ui/filesystem.h).
    const int next = (static_cast<int>(b.sortMode) + delta + FILE_SORT_MODE_COUNT) % FILE_SORT_MODE_COUNT;
    b.sortMode = static_cast<FileSortMode>(next);

    // ⚠️ REBUILD, not `sort_items` on what is already there — see rebuild_items. Sorting the on-screen
    // list in place would make the tie-break depend on the sort mode you happened to arrive from.
    rebuild_items(b, fs_);

    // The cursor stays where it is (Android's does — `handleRUp` copies only `sortMode`), so the row
    // under it now holds a different file. That is the point: you are re-ordering the list you are
    // looking at, not jumping somewhere.
    b.statusMessage = file_sort_label(b.sortMode);
    b.statusSuccess = true;
}

void InputDispatcher::on_r_left() {
    if (qwerty_open()) { move_text_cursor_left(s_.qwerty); return; }
    if (on_browser())  { navigate_to_parent(s_.fileBrowser, fs_); return; }
    const NavState ns = nav_state_of(s_);
    go_to_screen(s_, navigate_left(ns));
    s_.selection.exit();
}

void InputDispatcher::on_r_right() {
    if (qwerty_open()) { move_text_cursor_right(s_.qwerty); return; }
    if (on_browser())  return;   // no "down a directory" — that is what A on a folder is for
    const NavState ns = nav_state_of(s_);
    go_to_screen(s_, navigate_right(ns));
    s_.selection.exit();
}

// ─── L: selection and the clipboard ──────────────────────────────────────────────────────────────

void InputDispatcher::on_l_b() {
    if (qwerty_open()) return;

    // ⚠️ The browser's selection is a DIFFERENT machine from the grid editors'. Theirs is the multi-tap
    // CELL→ROW→SCREEN widener (ui/selection.h); the browser's is a plain anchor..cursor RANGE over a
    // list, and its second tap inside the window means SELECT ALL rather than "widen the scope". They
    // share a button and a 500 ms window and nothing else, which is why they are two pieces of code.
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
    if (qwerty_open()) return;

    // On the browser L+A is the FILE clipboard's cut/paste — the same "inside a selection it cuts,
    // outside one it pastes" shape as the grid editors below, over files instead of cells.
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

    // Inside a selection L+A CUTS; outside one it PASTES. One button, two verbs, and which one you
    // get is a function of the selection — Kotlin's `handleSelectA()`, inlined because the C++
    // selection has no InputAction to return.
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

void InputDispatcher::on_l_r() {
    if (qwerty_open()) return;
    if (on_browser()) {
        s_.fileBrowser.selectionMode   = false;
        s_.fileBrowser.selectionAnchor = -1;
        return;
    }
    s_.selection.exit();
}

// ─── L+B+A: clone ────────────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_l_b_a() {
    if (qwerty_open() || on_browser()) return;

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

// ─── The plain buttons ───────────────────────────────────────────────────────────────────────────

void InputDispatcher::on_button_a() {
    // A on the KEYBOARD types the key under the cursor — unless the cursor is on the action row, where
    // the two buttons ARE the answer: ABORT (col 0) and APPLY (col 1).
    if (qwerty_open()) {
        if (s_.qwerty.is_on_action_row()) {
            if (s_.qwerty.keyCursorCol == 0) qwerty_cancel();
            else                             qwerty_apply();
        } else {
            insert_current_key(s_.qwerty);
        }
        return;
    }

    // A on the BROWSER opens a folder, goes up, or loads the file — see browser_confirm.
    if (on_browser()) { browser_confirm(); return; }

    // A on a cell that OPENS something (INSTRUMENT's NAME / LOAD / SAVE, the pool's empty NAME). This
    // runs before the insert logic below, exactly as Kotlin's `openSubScreenAtCursor(peek = false)`
    // does, because those cells have nothing to insert.
    if (instrument_open_at_cursor()) return;

    // A on an EMPTY cell inserts the item you last edited. That is what makes A,A meaningful: press
    // A once to lay down the last chain again, press it twice to get a fresh one.
    Project& p = host_.edit_project();
    hasInsertPos_ = false;

    switch (s_.currentScreen) {
        case ScreenType::PHRASE: {
            if (s_.cursorColumn != 1 || s_.selection.active) return;
            Phrase& ph = p.phrases[static_cast<size_t>(s_.currentPhrase)];
            PhraseEditorState ps{ph};
            ps.cursorRow    = s_.cursorRow;
            ps.cursorColumn = s_.cursorColumn;
            if (!phrase_.cursor_context(ps).capabilities.isEmpty) return;

            songcore::PhraseStep& step = ph.steps[static_cast<size_t>(s_.cursorRow)];
            step.note       = s_.lastEditedNote;
            step.instrument = s_.lastEditedInstrument;
            step.volume     = s_.lastEditedVolume;
            mark_modified();

            if (s_.notePreviewEnabled && step.note != Note::EMPTY()) {
                // A WHOLE PHRASE long, not one step: this gesture lays a note down to listen to, and
                // an audition that dies after a 16th note tells you nothing about a pad.
                const int sr = std::max(44100, host_.sample_rate());
                host_.preview_note(std::min(std::max(step.instrument, 0), 127), step.note,
                                   songcore::frames_per_step(p.tempo, sr) * 16);
            }
            break;
        }

        case ScreenType::CHAIN: {
            Chain& chain = p.chains[static_cast<size_t>(s_.currentChain)];
            if (chain_row_empty(chain, s_.cursorRow)) {
                chain.phraseRefs[static_cast<size_t>(s_.cursorRow)]      = s_.lastEditedPhrase;
                chain.transposeValues[static_cast<size_t>(s_.cursorRow)] = s_.lastEditedTranspose;
                mark_modified();
                hasInsertPos_ = true;   // arm A,A — a second press here inserts the next UNUSED phrase
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

        default:
            break;
    }
}

void InputDispatcher::on_button_b() {
    if (qwerty_open()) { delete_char(s_.qwerty); return; }

    if (on_browser()) {
        FileBrowserState& fb = s_.fileBrowser;

        // B is the NO of "A=YES B=NO" — it disarms the delete rather than leaving the browser, which is
        // what makes SELECT+B safe to press by accident.
        if (fb.mode != BrowserMode::NORMAL) { fb.mode = BrowserMode::NORMAL; return; }

        // Inside a file selection, B COPIES it — the same gesture as B over a grid selection below.
        if (fb.selectionMode) {
            std::vector<std::string> files = browser_selected_paths();
            if (!files.empty()) {
                const size_t n = files.size();
                fb.fileClipboard      = std::move(files);
                fb.fileClipboardIsCut = false;
                fb.statusMessage = "CPY " + std::to_string(n) + (n == 1 ? " FILE" : " FILES");
                fb.statusSuccess = true;
            }
            fb.selectionMode   = false;
            fb.selectionAnchor = -1;
            return;
        }

        close_file_browser();
        return;
    }

    // B inside a selection COPIES it and exits — the tracker's copy gesture. Outside one, B on these
    // five screens does nothing (they are the main row; there is nowhere to go back to).
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
    // SELECT is the keyboard's ABORT — the chord alias for the button on its action row.
    if (qwerty_open()) { qwerty_cancel(); return; }

    // On the browser SELECT does nothing ALONE; it is a modifier there (SELECT+A/B/R), and its press
    // is what arms those three. Kotlin's `handleSelect()` has an empty FILE_BROWSER arm for the same
    // reason.
    if (on_browser()) return;

    // SELECT does NOT clear the cell under the cursor on the editor screens — deleting a value is
    // A+B. It is left free for CONTEXT ACTIONS, exactly as Kotlin leaves it, and S5 lands the first
    // one the port can honour.
    //
    // ⚠️ EFFECTS' TIME row: SELECT toggles DELAY SYNC — free-running milliseconds ↔ note divisions. It
    // has to be a separate gesture, because the cell's VALUE means two different things on either side
    // of it (0x40 is a delay length; 4 is a 1/16 note) and no amount of A+DPAD can express "change
    // which of those you mean". Re-clamping delayTime into 0..B on the way IN is not optional: a free
    // time of 0xF0 is not a subdivision, and an unclamped one would index past the end of the name list.
    if (s_.currentScreen == ScreenType::EFFECTS &&
        s_.effectsCursorRow == EffectModule::ROW_DLY_TIME) {
        songcore::Project& p = host_.edit_project();
        p.delaySync = !p.delaySync;
        if (p.delaySync) p.delayTime = std::min(std::max(p.delayTime, 0), 11);
        mark_modified();
        return;
    }

    // The other SELECT actions Kotlin has here all OPEN A SUB-SCREEN that the port has not built: the
    // EQ overlay (MIXER's master EQ, EFFECTS' two input EQs, the instrument EQ cells) and the qwerty
    // keyboard (the PROJECT / INSTRUMENT name rows). They land with those screens.
}

void InputDispatcher::on_stop_preview() {
    if (qwerty_open()) return;

    // Only the screens that can START an audition can stop one — `stopActivePreview()`. On PHRASE it
    // is gated on the setting, because with previews off there is nothing to silence. The three
    // instrument screens always can: their START *is* an audition, and it rings out until stopped, so
    // "press any button to silence it" is the only way to end it.
    //
    // ⚠️ The BROWSER is on this list too, and it has to be: its START auditions the file under the
    // cursor and the sample rings out (no timed kill). Moving the cursor to the next file must silence
    // the last one, or scrolling a folder of kicks stacks them on top of each other.
    const bool previewScreen = (s_.currentScreen == ScreenType::TABLE) || on_browser() ||
                               on_instrument_screen() ||
                               (s_.currentScreen == ScreenType::PHRASE && s_.notePreviewEnabled);
    if (previewScreen) host_.stop_preview();
}

void InputDispatcher::on_start() {
    // START is the keyboard's APPLY — the chord alias for the button on its action row.
    if (qwerty_open()) { qwerty_apply(); return; }

    // ⚠️ START on the BROWSER is not the transport either: it AUDITIONS the file under the cursor, on
    // the preview lane, decoded straight into slot 255. This is what a sample browser is FOR — hearing
    // a file before committing a slot to it — and it is the one note in the port that does not go
    // through `plan_note_on`, because a file being auditioned has no instrument to derive from
    // (songcore::preview_sample_file).
    if (on_browser()) {
        const BrowserItem* item = s_.fileBrowser.current();
        if (!item || item->kind != BrowserItem::Kind::FILE) return;

        const std::string ext = to_lower(item->extension);
        const bool audible = std::find(sample_extensions().begin(), sample_extensions().end(), ext) !=
                             sample_extensions().end();
        if (!audible) return;   // a .pti or an .sf2 has no waveform to play

        if (!host_.preview_file(item->path)) {
            s_.fileBrowser.statusMessage = "PREVIEW FAILED";
            s_.fileBrowser.statusSuccess = false;
        }
        return;
    }

    // ⚠️ **START IS NOT ALWAYS THE TRANSPORT.** On the four screens that edit a SOUND rather than an
    // arrangement — INSTRUMENT, INST.POOL, MODS and TABLE — it AUDITIONS the instrument at its own
    // root, on the preview lane, and the note rings until the next plain button press silences it. That
    // is the whole point of sitting on one of them: you are dialling a sound in and listening to it.
    //
    // It does not consult `is_playing()`, and that is deliberate: auditioning an instrument OVER a
    // running song is exactly what you want while you fit it into the mix, and the preview lane is a
    // ninth voice — it steals nothing from the eight the song is using.
    //
    // ⚠️ TABLE auditions **through the table it is showing** (`previewInstrumentWithTable(currentTable,
    // currentTable)` — the instrument id and the table id are the same number, because instrument N
    // owns table N). Without the override you would hear the instrument's own table instead of the
    // automation on the screen in front of you, which is the one thing the audition exists to check.
    if (on_instrument_screen()) {
        host_.preview_instrument(s_.currentInstrument);
        return;
    }
    if (s_.currentScreen == ScreenType::TABLE) {
        host_.preview_instrument(s_.currentTable, /*tableIdOverride=*/s_.currentTable);
        return;
    }

    // Everywhere else START is the transport.
    if (host_.is_playing()) {
        host_.stop();
        return;
    }
    switch (s_.currentScreen) {
        // ⚠️ SONG starts at the CURSOR ROW, not at row 0 — "play from here" is the gesture, and on a
        // 200-row arrangement starting from the top every time makes the screen unusable.
        case ScreenType::SONG:  host_.play_song(s_.cursorRow); break;
        case ScreenType::CHAIN: host_.play_chain(s_.currentChain); break;

        // ⚠️ …and the four screens with NO song cursor play the SONG, from the top. This is a bug fixed
        // in S5, not a new behaviour: the S3 comment already said "MIXER, EFFECTS, PROJECT, SETTINGS
        // start at 0" — while the code below it dropped them into the `default` and played the current
        // PHRASE. It went unnoticed because all four were placeholder screens (you could stand on one
        // and press START, and something plausible happened). What you want on the mixer is the MIX.
        case ScreenType::MIXER:
        case ScreenType::EFFECTS:
        case ScreenType::PROJECT:
        case ScreenType::SETTINGS: host_.play_song(0); break;

        // PHRASE, GROOVE, SCALE… — Kotlin's `togglePlayback()` else-arm.
        default: host_.play_phrase(s_.currentPhrase); break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════
// THE FILE BROWSER AND THE QWERTY KEYBOARD (Phase 3 S6a)
// ═════════════════════════════════════════════════════════════════════════════════════════════════

// ─── Opening and closing the browser ─────────────────────────────────────────────────────────────

void InputDispatcher::open_file_browser(AppState::BrowserPurpose purpose, const std::string& directory,
                                        const std::vector<std::string>& extensions) {
    s_.previousScreen = s_.currentScreen;
    s_.browserPurpose = purpose;

    s_.fileBrowser.fileExtensions = extensions;
    s_.fileBrowser.mode           = BrowserMode::NORMAL;
    navigate_to_folder(s_.fileBrowser, fs_, directory);

    s_.currentScreen = ScreenType::FILE_BROWSER;
}

void InputDispatcher::close_file_browser() {
    // The audition the user was scrolling through is over. Kotlin frees slot 255 here for the same
    // reason: a preview left resident is a megabyte of PCM nothing will ever play again.
    host_.clear_previews();
    s_.fileBrowser.selectionMode   = false;
    s_.fileBrowser.selectionAnchor = -1;
    s_.currentScreen = s_.previousScreen;
}

void InputDispatcher::refresh_browser() {
    // Re-list in place and KEEP THE CURSOR where it was — a refresh is not a navigation. Deleting the
    // twentieth file in a folder should leave you on the twentieth row, not throw you back to the
    // first. It has to be CLAMPED, though: the list may have got shorter, and a cursor past the end of
    // it is the "cursor vanishes" bug S2 found on the screen changes (nothing draws highlighted).
    FileBrowserState& b = s_.fileBrowser;
    rebuild_items(b, fs_);

    const int last = static_cast<int>(b.items.size()) - 1;
    b.cursor = std::min(std::max(b.cursor, 0), std::max(last, 0));

    if (b.cursor < b.scroll) b.scroll = b.cursor;
    if (b.cursor >= b.scroll + BROWSER_VISIBLE_ROWS) b.scroll = b.cursor - BROWSER_VISIBLE_ROWS + 1;
    b.scroll = std::max(0, std::min(b.scroll, std::max(0, last - BROWSER_VISIBLE_ROWS + 1)));
}

// ─── The browser's cursor ────────────────────────────────────────────────────────────────────────

void InputDispatcher::browser_move_cursor(int delta, bool page) {
    FileBrowserState& b     = s_.fileBrowser;
    const int         total = static_cast<int>(b.items.size());
    if (total == 0) return;

    // ⚠️ UP/DOWN WRAP; the LEFT/RIGHT page jump CLAMPS. That asymmetry is Kotlin's and it is the right
    // one: wrapping a single step off the end of a list is a convenience, but a PAGE that wrapped would
    // fling you from the top of a 400-file directory to the bottom on one tap.
    if (page) {
        b.cursor = std::min(std::max(b.cursor + delta, 0), total - 1);
    } else {
        b.cursor = (b.cursor + delta + total) % total;
    }

    // Keep the 19-row window around the cursor.
    if (b.cursor < b.scroll) {
        b.scroll = b.cursor;
    } else if (b.cursor >= b.scroll + BROWSER_VISIBLE_ROWS) {
        b.scroll = b.cursor - BROWSER_VISIBLE_ROWS + 1;
    }
}

// ─── A: open a folder, or LOAD the file ──────────────────────────────────────────────────────────

void InputDispatcher::browser_confirm() {
    FileBrowserState& b = s_.fileBrowser;

    // DELETE mode: A is the YES of "A=YES B=NO". This is the ONLY place the browser removes anything,
    // and it is two presses away from any accident — SELECT+B to arm, A to confirm.
    if (b.mode == BrowserMode::DELETE) {
        const BrowserItem* item = b.current();
        b.mode = BrowserMode::NORMAL;
        if (!item || item->is_parent()) return;

        const std::string name = item->displayName;
        if (fs_.delete_path(item->path)) {
            refresh_browser();
            b.statusMessage = "DELETED: " + name;
            b.statusSuccess = true;
        } else {
            b.statusMessage = "DELETE FAILED";
            b.statusSuccess = false;
        }
        return;
    }

    const BrowserItem* item = b.current();
    if (!item) return;

    if (item->is_parent()) { navigate_to_parent(b, fs_); return; }
    if (item->kind == BrowserItem::Kind::FOLDER) { navigate_to_folder(b, fs_, item->path); return; }

    // ── It is a FILE, and what happens now is the whole reason the browser was opened ────────────
    const int         id   = s_.currentInstrument;
    const std::string ext  = to_lower(item->extension);
    const std::string path = item->path;
    const std::string stem = item->displayName;

    bool ok = false;
    switch (s_.browserPurpose) {
        case AppState::BrowserPurpose::LOAD_PRESET:
            ok = host_.load_instrument_preset(id, path);
            break;

        case AppState::BrowserPurpose::LOAD_SOURCE:
            // The extension decides, not the slot's current type: picking an .sf2 from a sampler slot
            // TURNS it into a SoundFont slot (load_instrument_soundfont sets the type), which is what
            // the user asked for by picking one. The browser's filter usually makes this moot — but the
            // user can navigate anywhere, and a folder full of both is not exotic.
            if (ext == "sf2" || ext == "sf3") {
                ok = host_.load_soundfont(id, path);
            } else {
                ok = host_.load_sample(id, path);
            }
            break;
    }

    if (!ok) {
        b.statusMessage = "LOAD FAILED";
        b.statusSuccess = false;
        return;
    }

    // A still-unnamed slot adopts the file's name. Only a DEFAULT-named one ("INST07"): a slot the user
    // has named is theirs, and silently renaming it on a source swap would lose that.
    if (s_.browserPurpose == AppState::BrowserPurpose::LOAD_SOURCE) {
        Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(id)];
        if (songcore::instrument_has_default_name(ins)) ins.name = stem.substr(0, 20);
    }

    mark_modified();
    close_file_browser();
}

// ─── The multi-select and the file clipboard ─────────────────────────────────────────────────────

std::vector<std::string> InputDispatcher::browser_selected_paths() const {
    const FileBrowserState& b = s_.fileBrowser;
    std::vector<std::string> out;
    if (!b.selectionMode || b.selectionAnchor < 0) return out;

    const int lo = std::max(std::min(b.selectionAnchor, b.cursor), b.first_selectable());
    const int hi = std::max(b.selectionAnchor, b.cursor);
    for (int i = lo; i <= hi; ++i) {
        const BrowserItem* item = b.item_at(i);
        if (item && !item->is_parent()) out.push_back(item->path);
    }
    return out;
}

void InputDispatcher::browser_paste() {
    FileBrowserState& b = s_.fileBrowser;
    if (b.fileClipboard.empty()) return;

    const std::string& dest = b.currentDirectory;
    int done = 0, failed = 0;

    for (const std::string& src : b.fileClipboard) {
        if (!fs_.file_exists(src)) { ++failed; continue; }

        const std::string name = path_name(src);
        std::string       target = dest + "/" + name;
        if (target == src) { ++done; continue; }   // pasted back into the folder it was copied from

        // De-duplicate: "kick.wav" → "kick_2.wav" → "kick_3.wav". Never overwrite — a paste that
        // silently replaced a file of the same name would be a data-loss bug wearing a convenience hat.
        if (fs_.file_exists(target)) {
            const std::string ext  = path_extension(name);
            const std::string base = path_stem(name);
            for (int n = 2;; ++n) {
                target = dest + "/" + base + "_" + std::to_string(n) + (ext.empty() ? "" : "." + ext);
                if (!fs_.file_exists(target)) break;
            }
        }

        const bool ok = b.fileClipboardIsCut ? fs_.move_file(src, target) : fs_.copy_file(src, target);
        if (ok) ++done; else ++failed;
    }

    const bool  cut  = b.fileClipboardIsCut;
    const char* verb = cut ? "MOVED" : "COPIED";

    // A CUT clipboard is spent once pasted — the sources are gone. A COPY one survives, so the same
    // files can be pasted into several folders.
    if (cut) b.fileClipboard.clear();

    refresh_browser();
    b.statusMessage = failed == 0
                          ? std::string(verb) + " " + std::to_string(done) +
                                (done == 1 ? " FILE" : " FILES")
                          : std::string(verb) + " " + std::to_string(done) + ", FAILED " +
                                std::to_string(failed);
    b.statusSuccess = (failed == 0);
}

// ─── SELECT + A / B / R — the browser's file-management chords ───────────────────────────────────

void InputDispatcher::on_select_a() {
    if (qwerty_open() || !on_browser()) return;
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    const BrowserItem* item = s_.fileBrowser.current();
    if (!item || item->is_parent()) return;   // ".." is not a file and cannot be renamed

    const bool  dir   = (item->kind == BrowserItem::Kind::FOLDER);
    const std::string ext = to_lower(item->extension);
    const char* label = dir              ? "FOLDER NAME:"
                        : (ext == "wav") ? "SAMPLE NAME:"
                        : (ext == "ptp") ? "PROJECT NAME:"
                                         : "FILE NAME:";

    // A FOLDER's displayName is "[name]" — the brackets are decoration, and typing them back into the
    // rename box would make them part of the name.
    const std::string base = dir ? path_name(item->path) : item->displayName;
    open_qwerty(QwertyContext::FILE_RENAME, base, label, item->path);
}

void InputDispatcher::on_select_b() {
    if (qwerty_open() || !on_browser()) return;
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;

    const BrowserItem* item = s_.fileBrowser.current();
    if (!item || item->is_parent()) return;

    // ARM the confirm; never delete on this press. The top bar becomes "DELETE <name>? A=YES B=NO".
    s_.fileBrowser.mode          = BrowserMode::DELETE;
    s_.fileBrowser.statusMessage.clear();
    s_.fileBrowser.statusSuccess = true;
}

void InputDispatcher::on_select_r() {
    if (qwerty_open() || !on_browser()) return;
    if (s_.fileBrowser.mode != BrowserMode::NORMAL) return;
    open_qwerty(QwertyContext::FOLDER_CREATE, "NEW FOLDER", "FOLDER NAME:",
                s_.fileBrowser.currentDirectory);
}

// ─── The QWERTY keyboard ─────────────────────────────────────────────────────────────────────────

void InputDispatcher::open_qwerty(QwertyContext context, const std::string& initial_text,
                                  const std::string& field_label, const std::string& context_extra,
                                  int max_length, bool clear_on_first_b) {
    QwertyKeyboardState k{};
    k.isOpen        = true;
    k.text          = initial_text.substr(0, static_cast<size_t>(max_length));
    k.maxLength     = max_length;
    k.textCursor    = static_cast<int>(k.text.size());
    k.fieldLabel    = field_label;
    k.contextExtra  = context_extra;
    k.context       = context;
    k.clearOnFirstB = clear_on_first_b;
    k.insertBefore  = s_.insertBefore;   // read at OPEN, so flipping the setting cannot change what
    s_.qwerty       = k;                 // the buttons mean under the user's thumb mid-word
}

void InputDispatcher::qwerty_apply() {
    const QwertyKeyboardState k    = s_.qwerty;   // by value: every arm below closes the keyboard
    const std::string         text = trimmed_text(k);
    s_.qwerty = QwertyKeyboardState{};

    switch (k.context) {
        case QwertyContext::FILE_RENAME: {
            // An empty field means "leave it alone", not "name it nothing" — Kotlin's `.ifEmpty { … }`.
            const std::string name = text.empty() ? path_stem(k.contextExtra) : text;
            if (fs_.rename_file(k.contextExtra, name)) {
                refresh_browser();
                s_.fileBrowser.statusMessage = "RENAMED";
                s_.fileBrowser.statusSuccess = true;
            } else {
                s_.fileBrowser.statusMessage = "RENAME FAILED";
                s_.fileBrowser.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::FOLDER_CREATE: {
            const std::string name = text.empty() ? "NewFolder" : text;
            if (!fs_.create_folder(k.contextExtra, name).empty()) {
                refresh_browser();
                s_.fileBrowser.statusMessage = "CREATED";
                s_.fileBrowser.statusSuccess = true;
            } else {
                s_.fileBrowser.statusMessage = "CREATE FAILED";
                s_.fileBrowser.statusSuccess = false;
            }
            break;
        }

        case QwertyContext::INSTRUMENT_NAME: {
            Instrument& ins = host_.edit_project().instruments[static_cast<size_t>(s_.currentInstrument)];
            // A cleared name reverts to the default "INSTxx" rather than becoming blank — an unnamed
            // instrument still has to be identifiable in the pool.
            ins.name = text.empty() ? songcore::default_instrument_name(ins.id) : text;
            mark_modified();
            break;
        }

        case QwertyContext::INSTRUMENT_SAVE: {
            const std::string name = text.empty() ? "PRESET" : text;
            const std::string path = k.contextExtra + "/" + name + ".pti";
            if (host_.save_instrument_preset(s_.currentInstrument, path)) {
                s_.statusMessage = "SAVED: " + name;
                s_.statusSuccess = true;
            } else {
                s_.statusMessage = "SAVE FAILED";
                s_.statusSuccess = false;
            }
            break;
        }
    }
}

// ─── INSTRUMENT's buttons — the three cells S4 drew and could not press ──────────────────────────

bool InputDispatcher::instrument_open_at_cursor() {
    Project& p = host_.edit_project();

    if (s_.currentScreen == ScreenType::INST_POOL) {
        // A on the pool's NAME column of an EMPTY slot loads a source straight into it (M8's "tap EDIT
        // on an empty slot"). A slot that already HAS a source is managed on the INSTRUMENT screen —
        // loading over it from the pool, where you cannot see what is in it, would be too easy to do by
        // accident.
        if (s_.poolCursorColumn != 0) return false;
        const Instrument& ins  = p.instruments[static_cast<size_t>(s_.currentInstrument)];
        const bool        isSF = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;
        if (isSF ? ins.soundfontPath.has_value() : !songcore::instrument_is_free(ins)) return false;

        open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                          isSF ? fs_.soundfonts_directory() : fs_.samples_directory(),
                          isSF ? soundfont_extensions() : sample_extensions());
        return true;
    }

    if (s_.currentScreen != ScreenType::INSTRUMENT) return false;

    const Instrument& ins  = p.instruments[static_cast<size_t>(s_.currentInstrument)];
    const bool        isSF = ins.instrumentType == songcore::InstrumentType::SOUNDFONT;
    const int         row  = s_.instrumentCursorRow;
    const int         col  = s_.instrumentCursorColumn;

    // Row 0 — TYPE (col 1) + the PRESET buttons. A .pti carries the whole instrument: its params, its
    // mod slots, its table, and the path to its source.
    if (row == 0 && col == 2) {
        open_file_browser(AppState::BrowserPurpose::LOAD_PRESET, fs_.instruments_directory(), {"pti"});
        return true;
    }
    if (row == 0 && col == 3) {
        const std::string dir  = fs_.instruments_directory();
        const std::string name = ins.name.empty() ? songcore::default_instrument_name(ins.id) : ins.name;
        open_qwerty(QwertyContext::INSTRUMENT_SAVE, name, "SAVE PRESET:", dir, /*max_length=*/20,
                    /*clear_on_first_b=*/true);
        return true;
    }

    // Row 5 — the SOURCE row. LOAD (col 2) browses; EDIT (col 3) is the sample editor and is S6b's.
    if (row == 5 && col == 2) {
        open_file_browser(AppState::BrowserPurpose::LOAD_SOURCE,
                          isSF ? fs_.soundfonts_directory() : fs_.samples_directory(),
                          isSF ? soundfont_extensions() : sample_extensions());
        return true;
    }

    // Row 1 — NAME. The one cell here whose A is DEFERRED TO RELEASE (see defer_a_to_release).
    if (row == 1) {
        // A default-named slot opens the box EMPTY rather than with "INST07" in it: you are naming the
        // instrument, and having to delete the placeholder first is six button presses of nothing.
        const std::string current = songcore::instrument_has_default_name(ins) ? "" : ins.name;
        open_qwerty(QwertyContext::INSTRUMENT_NAME, current, "INSTRUMENT NAME:", "");
        return true;
    }

    return false;
}

bool InputDispatcher::defer_a_to_release() const {
    // ⚠️ The cells whose A OPENS something AND whose A+DPAD means something else. Firing the open on the
    // PRESS would pre-empt the combo: hold A on the NAME cell, press B to reset it, and you would be
    // looking at a keyboard instead. Kotlin defers exactly these (`openSubScreenAtCursor(peek = true)`),
    // and the mapper cancels the deferral the moment any A-combo fires.
    //
    // Today that is the INSTRUMENT NAME row. The EQ cells join it when the EQ overlay lands — they are
    // the case that makes this mechanism necessary rather than tidy, since A+DPAD dials the slot number
    // on the very cell whose A opens the editor.
    //
    // The LOAD / SAVE buttons are NOT here, deliberately: they are read-only cells with no A+DPAD
    // behaviour to protect, and Kotlin fires them on the press too.
    return s_.currentScreen == ScreenType::INSTRUMENT && s_.instrumentCursorRow == 1;
}

}  // namespace pt::ui
