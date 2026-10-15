#include "ui/modules/scale_editor.h"

#include "songcore/scales.h"
#include "ui/helpers.h"

namespace pt::ui {

namespace {

/** The pitch class a degree row names, in the current key. */
inline int degree_pitch_class(int key, int degree) { return songcore::scale_mod12(key + degree); }

// A pitch class with no octave ("C", "F#") — without the "-" pad the phrase cells use.
inline std::string pitch_class_name(int pc) {
    std::string name = songcore::NOTE_NAMES[songcore::scale_mod12(pc)];
    if (name.size() == 2 && name[1] == '-') name.pop_back();
    return name;
}

// ── The NAME row: the name runs up to SAVE (the bank's longest is 17 characters) ─────────────────
// ⚠️ The `*` column is taken, not reserved: the marker pushes the name one right, and SAVE clears
// the longest name in that shifted position.
inline constexpr int SCALE_STAR_X   = 10;
inline constexpr int SCALE_NAME_X   = 10;             // …+ CHAR_W while the marker is up
inline constexpr int SCALE_SAVE_X   = 335;
inline constexpr int SCALE_LOAD_X   = 418;

}  // namespace

void ScaleModule::draw(Canvas& c, int x, int y, const ScaleState& s) const {
    const Theme&           t     = s.theme;
    const songcore::Scale& scale = s.scale;

    c.fill_rect(x, y, WIDTH, HEIGHT, t.background);

    const int labelX = x + 10;
    const int noteX  = x + 10 + 40;
    const int valueX = x + 10 + 40 + 60;

    // ── Header ───────────────────────────────────────────────────────────────────────────────────
    const int headerY = y + TEXT_PADDING;
    c.draw_text("SCALE " + hex2(scale.id), labelX, headerY, t.textTitle, CHAR_SPACING, FONT_SCALE);

    // How many notes the scale has (7 a mode, 5 a pentatonic) — derived, never stored.
    std::string lenText = std::to_string(songcore::scale_degree_count(scale));
    if (lenText.size() < 2) lenText = " " + lenText;
    c.draw_text("LEN:" + lenText, x + WIDTH - 130, headerY, t.textParam, CHAR_SPACING, FONT_SCALE);

    // ── Row 0: NAME / SAVE / LOAD ────────────────────────────────────────────────────────────────
    const int  nameY    = y + ROW_HEIGHT + 14 + TEXT_PADDING;
    const bool onName   = (s.cursorRow == SCALE_NAME_ROW);

    // ⚠️ The name shown is not always the stored one: an unnamed slot is named by its intervals.
    // `----` is a shape the bank has no word for.
    std::string shown = songcore::scale_display_name(scale);
    const bool  named = !shown.empty();
    if (!named) shown = "----";

    // The drift marker pushes the name off the grid column.
    const bool marked = songcore::scale_differs_from_its_name(scale);
    if (marked) c.draw_text("*", x + SCALE_STAR_X, nameY, t.textPlayhead, CHAR_SPACING, FONT_SCALE);

    draw_cell(c, shown, x + SCALE_NAME_X + (marked ? CHAR_W : 0), nameY,
              /*is_cursor=*/onName && s.cursorColumn == SCALE_NAME_COL_NAME,
              /*is_selected=*/false, /*is_empty=*/!named, t.textValue, t);
    draw_cursor_cell(c, "SAVE", x + SCALE_SAVE_X, nameY, onName && s.cursorColumn == SCALE_NAME_COL_SAVE,
                     t.textParam, t);
    draw_cursor_cell(c, "LOAD", x + SCALE_LOAD_X, nameY, onName && s.cursorColumn == SCALE_NAME_COL_LOAD,
                     t.textParam, t);

    // ── Row 1: KEY ───────────────────────────────────────────────────────────────────────────────
    // ⚠️ Above the EN header: the key is the project's, the rows below the slot's.
    const int keyY = nameY + ROW_HEIGHT;

    const bool keyCursor = (s.cursorRow == SCALE_KEY_ROW);
    c.draw_text("KEY", labelX, keyY, keyCursor ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING,
                FONT_SCALE);
    // Padded to two glyphs so the cursor box does not resize past the sharps.
    std::string keyName = pitch_class_name(s.key);
    if (keyName.size() < 2) keyName += " ";
    draw_cursor_cell(c, keyName, valueX, keyY, keyCursor, t.textValue, t);

    // ── Column header ────────────────────────────────────────────────────────────────────────────
    const int columnHeaderY = keyY + ROW_HEIGHT + 14;
    c.draw_text("EN", valueX, columnHeaderY, cursor_mark_ink(t), CHAR_SPACING, FONT_SCALE);

    // ── The twelve degrees ───────────────────────────────────────────────────────────────────────
    const int dataStartY = columnHeaderY + ROW_HEIGHT;

    for (int degree = 0; degree < 12; ++degree) {
        const int  row      = SCALE_KEY_ROW + 1 + degree;
        const int  rowY     = dataStartY + (degree * ROW_HEIGHT);
        const bool isCursor = (row == s.cursorRow);
        const bool isOn     = scale.enabled[static_cast<size_t>(degree)] != 0;

        // The playback `>` marks the degree the sounding note landed on. ⚠️ On an OFF row it is no
        // bug: that track is under another scale, or its instrument does not transpose.
        if ((s.soundingMask >> degree_pitch_class(s.key, degree)) & 1u)
            draw_playhead(c, valueX - CHAR_W, rowY, t);

        // The degree (0-B) in the gutter; the note name beside it follows the key.
        draw_cursor_cell(c, hex1(degree), labelX, rowY, /*is_cursor=*/false,
                         isCursor ? cursor_mark_ink(t) : t.textEmpty, t);

        // An out-of-scale note is dim on its own row too: the screen reads as the playable notes.
        c.draw_text(pitch_class_name(degree_pitch_class(s.key, degree)), noteX, rowY,
                    isOn ? t.textValue : t.textEmpty, CHAR_SPACING, FONT_SCALE);

        draw_cell(c, isOn ? "ON" : "--", valueX, rowY, isCursor, /*is_selected=*/false,
                  /*is_empty=*/!isOn, t.textValue, t);
    }
}

CursorContext ScaleModule::cursor_context(const ScaleState& s) const {
    if (s.cursorRow == SCALE_NAME_ROW) {
        // ⚠️ SAVE and LOAD are read-only, not absent: they answer a bare A, and `none()` would hide
        // the cursor box.
        if (s.cursorColumn != SCALE_NAME_COL_NAME) return cc::read_only();
        CursorContext c = cc::index_cycle(songcore::scale_bank_cycle_index(s.scale),
                                          static_cast<int>(songcore::scale_bank().size()));
        // A+B on the name resets to Chromatic (bank row 0, the default slot).
        c.defaultValue = 0;
        return c;
    }

    if (s.cursorRow == SCALE_KEY_ROW) return cc::index_cycle(s.key, 12);

    const int degree = scale_row_degree(s.cursorRow);
    if (degree < 0) return cc::none();

    // ⚠️ The root and the last degree on are read-only: the cell refuses by not lighting, not by
    // swallowing a press.
    const bool isOn = s.scale.enabled[static_cast<size_t>(degree)] != 0;
    if (degree == 0) return cc::read_only();
    if (isOn && songcore::scale_degree_count(s.scale) <= 1) return cc::read_only();

    return cc::toggle_binary(isOn);
}

ScaleInputResult ScaleModule::handle_input(songcore::Scale& scale, int key, int cursor_row,
                                           int cursor_column, const InputAction& action) const {
    ScaleInputResult r;
    if (action.type != ActionType::SET_VALUE) return r;

    if (cursor_row == SCALE_NAME_ROW) {
        if (cursor_column != SCALE_NAME_COL_NAME) return r;   // SAVE / LOAD are not values

        const int index = action.value;
        if (index < 0 || index >= static_cast<int>(songcore::scale_bank().size())) return r;

        // ⚠️ Compared first: landing back on the same shape must not bump the dirty counter.
        songcore::Scale after = scale;
        songcore::scale_apply_bank(after, index);
        r.modified = (after.name != scale.name || after.enabled != scale.enabled);
        scale      = after;
        return r;
    }

    if (cursor_row == SCALE_KEY_ROW) {
        const int k = songcore::scale_mod12(action.value);
        r.newKey    = k;
        r.modified  = (k != key);
        return r;
    }

    const int degree = scale_row_degree(cursor_row);
    if (degree <= 0) return r;  // no row, or the root

    const int want = action.value != 0 ? 1 : 0;
    if (want == 0 && songcore::scale_degree_count(scale) <= 1) return r;

    r.modified = (scale.enabled[static_cast<size_t>(degree)] != want);
    scale.enabled[static_cast<size_t>(degree)] = want;
    return r;
}

}  // namespace pt::ui
