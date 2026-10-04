#include "ui/modules/scale_editor.h"

#include "songcore/scales.h"
#include "ui/helpers.h"

namespace pt::ui {

namespace {

/** The pitch class a degree row names, in the current key. */
inline int degree_pitch_class(int key, int degree) { return songcore::scale_mod12(key + degree); }

/**
 * A pitch class by name, with no octave — "C", "F#". The shared table pads a natural with a "-" so
 * that a phrase cell's "C-4" lines up with "C#4"; nothing follows the name here, so that pad would
 * read as part of it.
 */
inline std::string pitch_class_name(int pc) {
    std::string name = songcore::NOTE_NAMES[songcore::scale_mod12(pc)];
    if (name.size() == 2 && name[1] == '-') name.pop_back();
    return name;
}

// ── The NAME row's three cells ───────────────────────────────────────────────────────────────────
//
// The name gets everything up to SAVE: the bank's longest is "Phrygian Dominant", 17 characters.
// ⚠️ THE `*` COLUMN IS TAKEN, NOT RESERVED: an unmarked name sits on the grid's column and the marker
// pushes it one right. SAVE's column therefore clears the longest name in its SHIFTED position.
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

    // How many notes the scale has. It is the one number that says at a glance what you have built —
    // 7 is a mode, 5 a pentatonic, 12 no constraint at all — and it is derived, never stored.
    std::string lenText = std::to_string(songcore::scale_degree_count(scale));
    if (lenText.size() < 2) lenText = " " + lenText;
    c.draw_text("LEN:" + lenText, x + WIDTH - 130, headerY, t.textParam, CHAR_SPACING, FONT_SCALE);

    // ── Row 0: NAME / SAVE / LOAD ────────────────────────────────────────────────────────────────
    const int  nameY    = y + ROW_HEIGHT + 14 + TEXT_PADDING;
    const bool onName   = (s.cursorRow == SCALE_NAME_ROW);

    // ⚠️ The name SHOWN is not always the name stored: an unnamed slot is named by its intervals
    // ("Chromatic" without a `name` field). `----` is a hand-built shape the bank has no word for.
    std::string shown = songcore::scale_display_name(scale);
    const bool  named = !shown.empty();
    if (!named) shown = "----";

    // The drift marker takes the grid column and pushes the name off it — see the note above.
    const bool marked = songcore::scale_differs_from_its_name(scale);
    if (marked) c.draw_text("*", x + SCALE_STAR_X, nameY, t.textPlayhead, CHAR_SPACING, FONT_SCALE);

    draw_cell(c, shown, x + SCALE_NAME_X + (marked ? CHAR_W : 0), nameY,
              /*is_cursor=*/onName && s.cursorColumn == SCALE_NAME_COL_NAME,
              /*is_selected=*/false, /*is_empty=*/!named, t.textValue, t);
    draw_cell(c, "SAVE", x + SCALE_SAVE_X, nameY,
              /*is_cursor=*/onName && s.cursorColumn == SCALE_NAME_COL_SAVE,
              /*is_selected=*/false, /*is_empty=*/false, t.textParam, t);
    draw_cell(c, "LOAD", x + SCALE_LOAD_X, nameY,
              /*is_cursor=*/onName && s.cursorColumn == SCALE_NAME_COL_LOAD,
              /*is_selected=*/false, /*is_empty=*/false, t.textParam, t);

    // ── Row 1: KEY ───────────────────────────────────────────────────────────────────────────────
    // ⚠️ Above the EN column header, not under it: the key belongs to the PROJECT, the twelve rows
    // below to this SLOT. Inside the column it would read as a thirteenth "EN".
    const int keyY = nameY + ROW_HEIGHT;

    const bool keyCursor = (s.cursorRow == SCALE_KEY_ROW);
    c.draw_text("KEY", labelX, keyY, keyCursor ? cursor_mark_ink(t) : t.textParam, CHAR_SPACING,
                FONT_SCALE);
    // In the value column, padded to two glyphs so the cursor box does not resize past the sharps.
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

        // ── What is sounding ─────────────────────────────────────────────────────────────────────
        // The playback `>`, here marking a PITCH: the degree the sounding note landed on.
        // ⚠️ A marker on an OFF row is not a bug — that track is under another scale or its
        // instrument has transposing disabled. Drawing it only on enabled rows would hide that.
        if ((s.soundingMask >> degree_pitch_class(s.key, degree)) & 1u)
            draw_playhead(c, valueX - CHAR_W, rowY, t);

        // The row number is the DEGREE (0-B), the same gutter every grid draws; the note name beside
        // it is what that degree sounds like in the current key, and it moves when the key does.
        draw_cell(c, hex1(degree), labelX, rowY, /*is_cursor=*/false, /*is_selected=*/false,
                  /*is_empty=*/false, isCursor ? cursor_mark_ink(t) : t.textEmpty, t);

        // An out-of-scale note is drawn dim on its own row too, so the screen reads as the set of
        // notes you can play rather than as twelve switches.
        c.draw_text(pitch_class_name(degree_pitch_class(s.key, degree)), noteX, rowY,
                    isOn ? t.textValue : t.textEmpty, CHAR_SPACING, FONT_SCALE);

        draw_cell(c, isOn ? "ON" : "--", valueX, rowY, isCursor, /*is_selected=*/false,
                  /*is_empty=*/!isOn, t.textValue, t);
    }
}

CursorContext ScaleModule::cursor_context(const ScaleState& s) const {
    if (s.cursorRow == SCALE_NAME_ROW) {
        // ⚠️ SAVE and LOAD are READ-ONLY rather than absent: they answer a bare A, and a `none()`
        // context would also switch off the cursor box that says which of the three you are on.
        if (s.cursorColumn != SCALE_NAME_COL_NAME) return cc::read_only();
        CursorContext c = cc::index_cycle(songcore::scale_bank_cycle_index(s.scale),
                                          static_cast<int>(songcore::scale_bank().size()));
        // A+B on the name resets to Chromatic — bank row 0 and the default slot — so reset and "load
        // the first entry" are one edit. This cycle has no delete, so A+B applies `defaultValue`.
        c.defaultValue = 0;
        return c;
    }

    if (s.cursorRow == SCALE_KEY_ROW) return cc::index_cycle(s.key, 12);

    const int degree = scale_row_degree(s.cursorRow);
    if (degree < 0) return cc::none();

    // ⚠️ The root, and the last degree still on, are READ-ONLY rather than a toggle that refuses:
    // the cell says "you cannot do this" by not lighting, instead of swallowing a press silently.
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

        // ⚠️ Compared before the write: a cycle landing back on the same shape must not bump the
        // dirty counter (a phantom autosave and RECOVER WORK?).
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
    if (degree <= 0) return r;  // no row, or the root, which cannot be turned off

    const int want = action.value != 0 ? 1 : 0;
    if (want == 0 && songcore::scale_degree_count(scale) <= 1) return r;

    r.modified = (scale.enabled[static_cast<size_t>(degree)] != want);
    scale.enabled[static_cast<size_t>(degree)] = want;
    return r;
}

}  // namespace pt::ui
