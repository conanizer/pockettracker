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
// ⚠️ 28 characters is the whole row, which sizes the catalogue's `cell` names (the last cell has
// five). Fixed columns: a missing scope number leaves a gap.
constexpr int COL_CC    = 0;
constexpr int COL_VAL   = 4;
constexpr int COL_MIN   = 8;
constexpr int COL_MAX   = 12;
constexpr int COL_GROUP = 16;
constexpr int COL_SCOPE = 20;
constexpr int COL_PARAM = 23;


// Null on the ADD row and past the list.
const MidiMapping* mapping_at(const songcore::Project& p, int row) {
    if (row < 0 || row >= static_cast<int>(p.midiMappings.size())) return nullptr;
    return &p.midiMappings[static_cast<size_t>(row)];
}

// The project's own count, never the catalogue's.
int scope_max(const songcore::Project& p, MapScope s) {
    if (s == MapScope::TRACK)      return std::max(0, static_cast<int>(p.tracks.size()) - 1);
    if (s == MapScope::INSTRUMENT) return std::max(0, static_cast<int>(p.instruments.size()) - 1);
    return 0;
}

// The scope cell: a track 1-based as on the mixer, an instrument in hex as everywhere else.
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

    // The column headers carry the cursor mark for its column.
    const int headerY = y + TEXT_PADDING + ROW_HEIGHT + 14;
    const int cursorCol = s.cursorColumn;
    const auto header = [&](const char* text, int column, MapCol lo, MapCol hi) {
        c.draw_text(text, colX(column), headerY + TEXT_PADDING,
                    header_color(cursorCol, static_cast<int>(lo), static_cast<int>(hi), t),
                    CHAR_SPACING, FONT_SCALE);
    };
    header("CC",   COL_CC,    MapCol::CC,    MapCol::CC);
    // VAL is never marked: it is read from the song.
    c.draw_text("VAL", colX(COL_VAL), headerY + TEXT_PADDING, t.textParam, CHAR_SPACING, FONT_SCALE);
    header("MIN",  COL_MIN,   MapCol::MIN,   MapCol::MIN);
    header("MAX",  COL_MAX,   MapCol::MAX,   MapCol::MAX);
    header("DEST", COL_GROUP, MapCol::GROUP, MapCol::SCOPE);

    // ── Scroll, derived from the cursor row every frame ──────────────────────────────────────────
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
                // The ADD row, a button.
                const bool full = static_cast<int>(p.midiMappings.size()) >= songcore::MIDI_MAP_MAX;
                draw_cursor_cell(c, full ? "FULL: 128 MAPPINGS" : "A: ADD MAPPING", colX(COL_CC),
                                 rowY + TEXT_PADDING, onRow, full ? t.textEmpty : t.textParam, t);
                continue;
            }

            const MapDest* d = songcore::map_dest(m->dest);
            // ⚠️ Two absences, dimmed differently: no catalogue entry (a newer build's id) versus a
            // target that is gone (a cleared instrument slot). Both keep the row.
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

    // ── The footer: why a dimmed row is dimmed ───────────────────────────────────────────────────
    std::string footer;
    if (const MidiMapping* m = mapping_at(p, s.cursorRow)) {
        if (!songcore::map_dest(m->dest))              footer = "PARAMETER UNKNOWN TO THIS VERSION";
        else if (!songcore::map_dest_present(p, *m))   footer = "PARAMETER MISSING";
    } else if (p.midiMappings.empty()) {
        footer = "NO MAPPINGS YET";
    }
    // Follows the last row, until the rows reach the panel's bottom.
    if (!footer.empty()) {
        const int noteY = std::min(footerY, firstRowY + rows * ROW_HEIGHT - scrollY);
        c.draw_text(footer, colX(COL_CC), noteY + TEXT_PADDING, t.textEmpty, CHAR_SPACING,
                    FONT_SCALE);
    }
}

// ─── Cursor ──────────────────────────────────────────────────────────────────────────────────────

CursorContext MidiMapModule::cursor_context(const MidiMapState& s) const {
    const MidiMapping* m = mapping_at(s.project, s.cursorRow);
    if (m == nullptr) return cc::read_only();   // the ADD row

    const MapDest* d = songcore::map_dest(m->dest);
    const auto col   = static_cast<MapCol>(s.cursorColumn);

    const auto cell = [&]() -> CursorContext {
        switch (col) {
            case MapCol::CC:
                // ⚠️ 0..127: a controller number is seven bits on the wire.
                return cc::hex_byte(m->controller, 0, 127);

            // ⚠️ Bounded by the destination's own range, never 0..FF. A+B puts the bound back.
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

            case MapCol::GROUP:
                return cc::enum_cycle(d ? static_cast<int>(d->group) : 0, songcore::MAP_GROUP_COUNT);

            case MapCol::PARAM:
                if (!d) return cc::read_only();
                return cc::enum_cycle(songcore::map_index_in_group(d->id), songcore::map_group_size(d->group));

            case MapCol::SCOPE:
                if (!d || !map_scope_editable(d->scope)) return cc::read_only();
                return cc::hex_byte(m->scopeIndex, 0, scope_max(s.project, d->scope));
        }
        return cc::none();
    };

    // ⚠️ A+B deletes the whole mapping from every cell but MIN and MAX, where it resets the bound.
    CursorContext c = cell();
    if (col != MapCol::MIN && col != MapCol::MAX) c.capabilities.canDelete = true;
    return c;
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

    // Shared with the picker: a new destination brings its range and clears its scope.
    const auto take_dest = [&](const MapDest& nd) { return songcore::take_dest(m, nd); };

    const auto col = static_cast<MapCol>(cursor_column);
    if (action.type == ActionType::DELETE && col != MapCol::MIN && col != MapCol::MAX) {
        project.midiMappings.erase(project.midiMappings.begin() + cursor_row);
        r.modified   = true;
        r.rowDeleted = true;
        return r;
    }

    const bool isSet = (action.type == ActionType::SET_VALUE);

    switch (col) {
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
            if (!isSet || !d) break;

            if (static_cast<MapCol>(cursor_column) == MapCol::GROUP) {
                const auto g = static_cast<MapGroup>(
                    clamp(action.value, 0, songcore::MAP_GROUP_COUNT - 1));
                // A group's first parameter: stepping sideways has to land on something.
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
