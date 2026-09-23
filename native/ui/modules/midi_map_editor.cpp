#include "ui/modules/midi_map_editor.h"

#include <algorithm>

#include "ui/helpers.h"

namespace pt::ui {

namespace {

using songcore::MapDest;
using songcore::MapGroup;
using songcore::MapScope;
using songcore::MidiMapping;

constexpr int NAME_X = 10;

// ─── The columns ─────────────────────────────────────────────────────────────────────────────────
//
// ⚠️ **TWENTY-EIGHT CHARACTERS IS THE WHOLE ROW**, and that is what sizes the catalogue's `cell`
// names. The panel is 510px at x = 10 and the editor clip is at 509, so the text has 489px — 28
// glyphs of CHAR_W. The last cell starts at column 23, which leaves it five.
//
// ⭐ The columns are FIXED whether or not a row uses them all: a destination with no scope number
// (the master bus, the two send buses) leaves that gap empty rather than sliding its name left, so
// the names stay in one column down the list and the eye can run down it.
constexpr int COL_CC    = 0;
constexpr int COL_VAL   = 4;
constexpr int COL_MIN   = 8;
constexpr int COL_MAX   = 12;
constexpr int COL_GROUP = 16;
constexpr int COL_SCOPE = 20;
constexpr int COL_PARAM = 23;

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** The mapping at `row`, or null on the ADD row and on a cursor that outran the list. */
const MidiMapping* mapping_at(const songcore::Project& p, int row) {
    if (row < 0 || row >= static_cast<int>(p.midiMappings.size())) return nullptr;
    return &p.midiMappings[static_cast<size_t>(row)];
}

/** How high a scope number may be dialled — the project's own count, never the catalogue's. */
int scope_max(const songcore::Project& p, MapScope s) {
    if (s == MapScope::TRACK)      return std::max(0, static_cast<int>(p.tracks.size()) - 1);
    if (s == MapScope::INSTRUMENT) return std::max(0, static_cast<int>(p.instruments.size()) - 1);
    return 0;
}

/**
 * The scope cell's text. ⚠️ A track is numbered the way the mixer numbers it (1-based) and an
 * instrument the way the whole app addresses one (hex, 00..7F) — the two are different alphabets on
 * purpose, and both are the alphabet the user already reads that thing in.
 */
std::string scope_text(MapScope s, int index) {
    if (s == MapScope::TRACK)      return dec2(index + 1);
    if (s == MapScope::INSTRUMENT) return hex2(index);
    return "";
}

}  // namespace

// ─── Draw ────────────────────────────────────────────────────────────────────────────────────────

void MidiMapModule::draw(Canvas& c, int x, int y, const MidiMapState& s) const {
    const Theme&            t = s.theme;
    const songcore::Project& p = s.project;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const auto colX = [&](int column) { return x + NAME_X + column * CHAR_W; };

    c.draw_text("MIDI MAPPING", colX(COL_CC), y + TEXT_PADDING, t.textTitle, CHAR_SPACING,
                FONT_SCALE);

    // The column headers. They are what makes six anonymous numbers a table, and they carry the
    // cursor mark for the column it is in — the IN CH row's header does the same thing.
    const int headerY = y + TEXT_PADDING + ROW_HEIGHT + 14;
    const int cursorCol = s.cursorColumn;
    const auto header = [&](const char* text, int column, MapCol lo, MapCol hi) {
        c.draw_text(text, colX(column), headerY + TEXT_PADDING,
                    header_color(cursorCol, static_cast<int>(lo), static_cast<int>(hi), t),
                    CHAR_SPACING, FONT_SCALE);
    };
    header("CC",   COL_CC,    MapCol::CC,    MapCol::CC);
    // VAL is drawn but never marked: it is read from the song, so there is no column to stand in.
    c.draw_text("VAL", colX(COL_VAL), headerY + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
    header("MIN",  COL_MIN,   MapCol::MIN,   MapCol::MIN);
    header("MAX",  COL_MAX,   MapCol::MAX,   MapCol::MAX);
    header("DEST", COL_GROUP, MapCol::GROUP, MapCol::SCOPE);

    // ── Scroll ───────────────────────────────────────────────────────────────────────────────────
    //
    // SETTINGS' arrangement exactly: DERIVED from the cursor row every frame, so there is no stored
    // scroll position that could disagree with where the cursor is. A list shorter than the viewport
    // clamps `maxScroll` to 0 and nothing moves — which is every list until someone makes fifteen
    // mappings.
    const int firstRowY = headerY + ROW_HEIGHT;
    const int footerY   = y + HEIGHT - ROW_HEIGHT;
    const int viewportH = footerY - firstRowY;
    const int rows      = midi_map_row_count(p);
    const int maxScroll = std::max(0, rows * ROW_HEIGHT - viewportH);
    const int scrollY   = clamp(s.cursorRow * ROW_HEIGHT + ROW_HEIGHT / 2 - viewportH / 2,
                                0, maxScroll);

    {
        const Canvas::ClipScope rowsClip(c, x, firstRowY, WIDTH, viewportH);

        for (int i = 0; i < rows; ++i) {
            const int  rowY   = firstRowY + i * ROW_HEIGHT - scrollY;
            const bool onRow  = s.cursorRow == i;
            const auto onCell = [&](MapCol col) {
                return onRow && s.cursorColumn == static_cast<int>(col);
            };

            const MidiMapping* m = mapping_at(p, i);
            if (m == nullptr) {
                // The ADD row — a button, drawn like PANIC and TEST are.
                const bool full = static_cast<int>(p.midiMappings.size()) >= songcore::MIDI_MAP_MAX;
                draw_cursor_cell(c, full ? "FULL: 128 MAPPINGS" : "A: ADD MAPPING", colX(COL_CC),
                                 rowY + TEXT_PADDING, onRow, full ? t.textEmpty : t.textParam, t);
                continue;
            }

            const MapDest* d = songcore::map_dest(m->dest);
            // ⚠️ Two different absences, and the row says so differently: no catalogue entry at all
            // (an id from a build that knew more parameters than this one) versus an entry whose
            // target is gone (the instrument slot was cleared). Both keep the row and dim it.
            const bool  present = songcore::map_dest_present(p, *m);
            const Argb  ink     = present ? t.textValue : t.textEmpty;
            const int   live    = songcore::read_mapped(p, *m);

            draw_cursor_cell(c, hex2(m->controller), colX(COL_CC), rowY + TEXT_PADDING,
                             onCell(MapCol::CC), ink, t);
            c.draw_text(live < 0 ? "--" : hex2(live), colX(COL_VAL), rowY + TEXT_PADDING,
                        present ? t.textValue : t.textEmpty, CHAR_SPACING, FONT_SCALE);
            draw_cursor_cell(c, hex2(m->rangeMin), colX(COL_MIN), rowY + TEXT_PADDING,
                             onCell(MapCol::MIN), ink, t);
            draw_cursor_cell(c, hex2(m->rangeMax), colX(COL_MAX), rowY + TEXT_PADDING,
                             onCell(MapCol::MAX), ink, t);

            draw_cursor_cell(c, d ? songcore::map_group_name(d->group) : "???", colX(COL_GROUP),
                             rowY + TEXT_PADDING, onCell(MapCol::GROUP), ink, t);
            if (d && map_scope_editable(d->scope))
                draw_cursor_cell(c, scope_text(d->scope, m->scopeIndex), colX(COL_SCOPE),
                                 rowY + TEXT_PADDING, onCell(MapCol::SCOPE), ink, t);
            draw_cursor_cell(c, d ? d->cell : "---", colX(COL_PARAM), rowY + TEXT_PADDING,
                             onCell(MapCol::PARAM), ink, t);
        }
    }

    // ── The footer ───────────────────────────────────────────────────────────────────────────────
    //
    // ⚠️ IT IS WHERE A DIMMED ROW SAYS WHY IT IS DIMMED. Dimming alone is a row that looks switched
    // off with nothing on screen to say what would switch it back on — and the theme editor's
    // unblendable colour is marked exactly this way.
    std::string footer;
    if (const MidiMapping* m = mapping_at(p, s.cursorRow)) {
        if (!songcore::map_dest(m->dest))              footer = "PARAMETER UNKNOWN TO THIS VERSION";
        else if (!songcore::map_dest_present(p, *m))   footer = "PARAMETER MISSING";
    } else if (p.midiMappings.empty()) {
        footer = "NO MAPPINGS YET";
    }
    // ⚠️ It follows the LAST ROW while the list is short, and only settles on the panel's bottom edge
    // once the rows reach it — a one-row list with its note pinned four inches below reads as two
    // unrelated screens.
    if (!footer.empty()) {
        const int noteY = std::min(footerY, firstRowY + rows * ROW_HEIGHT - scrollY);
        c.draw_text(footer, colX(COL_CC), noteY + TEXT_PADDING, t.textEmpty, CHAR_SPACING,
                    FONT_SCALE);
    }
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext MidiMapModule::cursor_context(const MidiMapState& s) const {
    const MidiMapping* m = mapping_at(s.project, s.cursorRow);
    if (m == nullptr) return cc::read_only();   // the ADD row — plain A is its whole behaviour

    const MapDest* d = songcore::map_dest(m->dest);

    switch (static_cast<MapCol>(s.cursorColumn)) {
        case MapCol::CC:
            // ⚠️ 0..127 AND NOT 0..255: a controller number is seven bits on the wire, so a cell that
            // dialled past 127 would be dialling a knob no cable can send.
            return cc::hex_byte(m->controller, 0, 127);

        // ⚠️ THE RANGE IS BOUNDED BY THE DESTINATION'S OWN RANGE, never by 0..FF — a crush is 0..F,
        // and a range dialled to 0x80 on it would mean nothing. A+B puts the bound back.
        case MapCol::MIN:
            if (!d) return cc::read_only();
            return cc::hex_byte(m->rangeMin, d->min, d->max, /*empty_value=*/-1,
                                /*can_delete=*/false, /*can_insert=*/false, /*can_create=*/false,
                                /*def=*/d->min);
        case MapCol::MAX:
            if (!d) return cc::read_only();
            return cc::hex_byte(m->rangeMax, d->min, d->max, /*empty_value=*/-1,
                                /*can_delete=*/false, /*can_insert=*/false, /*can_create=*/false,
                                /*def=*/d->max);

        // ⚠️ **A+B ON EITHER DESTINATION CELL DELETES THE WHOLE MAPPING**, which is why they carry the
        // delete and CC and the range rows do not. A mapping with no destination is not a thing the
        // song can hold — clearing the destination IS removing the row — and hanging the gesture on
        // the range cells instead would make "put this range back" and "throw this mapping away" the
        // same press.
        case MapCol::GROUP: {
            CursorContext c = cc::enum_cycle(d ? static_cast<int>(d->group) : 0,
                                             songcore::MAP_GROUP_COUNT);
            c.capabilities.canDelete = true;
            return c;
        }

        case MapCol::PARAM: {
            if (!d) return cc::read_only();
            CursorContext c = cc::enum_cycle(songcore::map_index_in_group(d->id),
                                             songcore::map_group_size(d->group));
            c.capabilities.canDelete = true;
            return c;
        }

        case MapCol::SCOPE:
            if (!d || !map_scope_editable(d->scope)) return cc::read_only();
            return cc::hex_byte(m->scopeIndex, 0, scope_max(s.project, d->scope));
    }
    return cc::none();
}

// ─── Input ───────────────────────────────────────────────────────────────────────────────────────

MidiMapInputResult MidiMapModule::handle_input(songcore::Project& project, int cursor_row,
                                               int cursor_column,
                                               const InputAction& action) const {
    MidiMapInputResult r;
    if (action.type == ActionType::NONE) return r;
    if (cursor_row < 0 || cursor_row >= static_cast<int>(project.midiMappings.size())) return r;

    MidiMapping&   m = project.midiMappings[static_cast<size_t>(cursor_row)];
    const MapDest* d = songcore::map_dest(m.dest);

    // ⚠️ `songcore::take_dest` and not a lambda here: the picker overlay points a mapping at a
    // destination too, and a new destination brings its own RANGE and clears its SCOPE. Three callers
    // of one function rather than three copies of four lines.
    const auto take_dest = [&](const MapDest& nd) { return songcore::take_dest(m, nd); };

    const bool isSet = (action.type == ActionType::SET_VALUE);

    switch (static_cast<MapCol>(cursor_column)) {
        case MapCol::CC:
            if (!isSet) break;
            m.controller = static_cast<uint8_t>(clamp(action.value, 0, 127));
            r.modified   = true;
            break;

        case MapCol::MIN:
            if (!isSet || !d) break;
            m.rangeMin = clamp(action.value, std::min(d->min, d->max), std::max(d->min, d->max));
            r.modified = true;
            break;

        case MapCol::MAX:
            if (!isSet || !d) break;
            m.rangeMax = clamp(action.value, std::min(d->min, d->max), std::max(d->min, d->max));
            r.modified = true;
            break;

        case MapCol::GROUP:
        case MapCol::PARAM: {
            if (action.type == ActionType::DELETE) {
                project.midiMappings.erase(project.midiMappings.begin() + cursor_row);
                r.modified   = true;
                r.rowDeleted = true;
                break;
            }
            if (!isSet || !d) break;

            if (static_cast<MapCol>(cursor_column) == MapCol::GROUP) {
                const auto g = static_cast<MapGroup>(
                    clamp(action.value, 0, songcore::MAP_GROUP_COUNT - 1));
                // The group's FIRST parameter, because a group is not a parameter: stepping sideways
                // has to land on something, and the group's own order is what the screen shows.
                if (const MapDest* nd = songcore::map_dest_in_group(g, 0)) r.modified = take_dest(*nd);
            } else {
                const int size = songcore::map_group_size(d->group);
                if (const MapDest* nd =
                        songcore::map_dest_in_group(d->group, clamp(action.value, 0, size - 1)))
                    r.modified = take_dest(*nd);
            }
            break;
        }

        case MapCol::SCOPE:
            if (!isSet || !d || !map_scope_editable(d->scope)) break;
            m.scopeIndex = static_cast<uint8_t>(
                clamp(action.value, 0, scope_max(project, d->scope)));
            r.modified = true;
            break;
    }

    return r;
}

}  // namespace pt::ui
